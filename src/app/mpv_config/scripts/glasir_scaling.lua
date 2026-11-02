-- SPDX-License-Identifier: MIT
-- How Glasir scales the capture up to its place on the screen, before DLSS 5
-- and frame generation (they work on what mpv presents):
--
--   mpv      mpv's own scaler (Lanczos)
--   fsr      AMD FSR 1 (shaders/glasir_fsr1.glsl): edge-adaptive upscaling
--            and sharpening, any GPU
--   vsr      the GPU's video super resolution (mpv's d3d11vpp filter, in the
--            GPU's video processor): NVIDIA RTX or Intel; FSR 1 where it
--            cannot run
--   cnn      a small neural network that doubles the detail (ArtCNN), then
--            FSR 1 for what is left; any GPU, more GPU time
--   anime4k  Anime4K's neural upscaler, for cel-shaded and drawn styles;
--            any GPU, more GPU time
--
-- Nothing is done when the capture is not smaller than its place (a 1440p
-- capture on a 1440p screen). The scale follows the window: it is checked
-- again when the window or the capture changes size.
--
-- Options (script-opts glasir_scaling-...): mode, vsr (nvidia, intel or
-- none: what the GPU has), hdr (yes: HDR10 frames), hdr_peak (nits, as
-- tagged). "script-message glasir-scaling <mode>" switches while playing
-- (the in-game menu); user-data/glasir/scaling says the mode.
local mp = require 'mp'
local opts = { mode = 'mpv', vsr = 'nvidia', hdr = false, hdr_peak = 1000 }
local options_changed  -- set below
require('mp.options').read_options(opts, 'glasir_scaling', function() options_changed() end)

local FSR = '~~/shaders/glasir_fsr1.glsl'
-- The shaders each mode runs, in order. Each runs only when the picture is
-- made larger: the CNN doubles from 1.3x up, and FSR 1 after it does what is
-- left (all of it below 1.3x, beyond 2x the rest); Anime4K's own passes
-- choose by the scale (from 1.2x).
local SHADERS = {
    fsr = { FSR },
    cnn = { '~~/shaders/ArtCNN_C4F16.glsl', FSR },
    anime4k = {
        '~~/shaders/Anime4K_Clamp_Highlights.glsl',
        '~~/shaders/Anime4K_Restore_CNN_M.glsl',
        '~~/shaders/Anime4K_Upscale_CNN_x2_M.glsl',
        '~~/shaders/Anime4K_AutoDownscalePre_x2.glsl',
        '~~/shaders/Anime4K_AutoDownscalePre_x4.glsl',
        '~~/shaders/Anime4K_Upscale_CNN_x2_S.glsl',
    },
}
local NAMES = { mpv = 'mpv (Lanczos)', fsr = 'AMD FSR 1', cnn = 'ArtCNN', anime4k = 'Anime4K' }
local function vsr_name()
    return ({ nvidia = 'NVIDIA RTX Video Super Resolution', intel = 'Intel Video Super Resolution' })[opts.vsr]
        or 'Video Super Resolution'
end
NAMES.vsr = vsr_name()
local LABELS = { '@glasir-vsr-in', '@glasir-vsr', '@glasir-vsr-out' }
local mode = NAMES[opts.mode] and opts.mode or 'mpv'
local vsr_scale = nil  -- the scale VSR runs at; nil: not in the filter chain
local function has_vsr() return opts.vsr == 'nvidia' or opts.vsr == 'intel' end
local vsr_failed = not has_vsr()
local shaders = {}     -- the shaders this script put in glsl-shaders
local pending = nil

local function toast(text)
    mp.commandv('script-message', 'glasir-toast', text)
end

local function same(a, b)
    if #a ~= #b then return false end
    for i = 1, #a do if a[i] ~= b[i] then return false end end
    return true
end

-- Puts these shaders (in this order) in mpv's list instead of the ones this
-- script put there before; anything else in the list stays.
local function set_shaders(want)
    if same(want, shaders) then return end
    for _, s in ipairs(shaders) do mp.commandv('change-list', 'glsl-shaders', 'remove', s) end
    for _, s in ipairs(want) do mp.commandv('change-list', 'glsl-shaders', 'append', s) end
    shaders = want
end

local function remove_vsr()
    if vsr_scale == nil then return end
    for _, label in ipairs(LABELS) do mp.command_native({ 'vf', 'remove', label }) end
    vsr_scale = nil
end

-- The scale that makes the capture fill its place, or nil when it already
-- does (or the sizes are not known yet).
local function wanted_scale()
    local w = mp.get_property_number('video-params/w')
    local h = mp.get_property_number('video-params/h')
    local dims = mp.get_property_native('osd-dimensions')
    if not w or not h or w < 16 or h < 16 or not dims then return nil end
    local ow = dims.w - (dims.ml or 0) - (dims.mr or 0)
    local oh = dims.h - (dims.mt or 0) - (dims.mb or 0)
    if ow < 16 or oh < 16 then return nil end
    local fit = math.min(ow / w, oh / h, 4)
    if fit <= 1.001 then return nil end
    -- Even output sizes, which the video processor wants.
    local out_h = math.floor(h * fit / 2) * 2
    return out_h / h
end

local function add_vsr(scale)
    local chain = {}
    -- The video processor shifts PQ / BT.2020 frames toward magenta when it
    -- scales them: they go in tagged as BT.709 and come out tagged as HDR10
    -- again. Only the tags change; the values pass through as they are.
    if opts.hdr then
        chain[#chain + 1] = '@glasir-vsr-in:format=colormatrix=bt.709:colorlevels=limited:primaries=bt.709:gamma=bt.1886'
    end
    chain[#chain + 1] = string.format('@glasir-vsr:d3d11vpp=format=%s:scaling-mode=%s:scale=%.6f',
        opts.hdr and 'p010' or 'nv12', opts.vsr, scale)
    if opts.hdr then
        chain[#chain + 1] = string.format(
            '@glasir-vsr-out:format=colormatrix=bt.2020-ncl:colorlevels=limited:primaries=bt.2020:gamma=pq:max-luma=%d:max_cll=%d',
            opts.hdr_peak, opts.hdr_peak)
    end
    for _, f in ipairs(chain) do
        local ok, err = mp.command_native({ 'vf', 'append', f })
        if ok == nil and err then
            for _, label in ipairs(LABELS) do mp.command_native({ 'vf', 'remove', label }) end
            return false
        end
    end
    return true
end

local function vsr_unavailable()
    vsr_failed = true
    mp.msg.warn(NAMES.vsr .. ' could not start; FSR 1 instead')
    toast(NAMES.vsr .. ' is not available here: FSR 1 instead')
end

-- Puts the chosen mode in place for the current sizes.
local function apply()
    pending = nil
    local scale = wanted_scale()
    if mode == 'vsr' and not vsr_failed and scale ~= nil then
        if vsr_scale == nil or math.abs(vsr_scale - scale) > 0.005 then
            remove_vsr()
            if add_vsr(scale) then
                vsr_scale = scale
                mp.msg.info(string.format('%s at %.3fx', NAMES.vsr, scale))
            else
                vsr_unavailable()
            end
        end
    end
    if mode ~= 'vsr' or vsr_failed or scale == nil then remove_vsr() end
    local want = {}
    if scale ~= nil then
        want = SHADERS[mode] or {}
        if mode == 'vsr' and vsr_failed then want = SHADERS.fsr end
    end
    set_shaders(want)
    mp.set_property('user-data/glasir/scaling', mode)
end

-- Sizes settle over a few events (the window opening, full screen): once.
local function soon()
    if pending then pending:kill() end
    pending = mp.add_timeout(0.3, apply)
end

-- The filter loads even where the GPU turns the super resolution down: the
-- video processor then scales on its own (plainly), and only says so in a
-- warning, when the first frame goes through.
mp.enable_messages('warn')
mp.register_event('log-message', function(e)
    if mode == 'vsr' and not vsr_failed and vsr_scale ~= nil and
        e.text:find('Failed to enable', 1, true) and e.text:find('Super Resolution', 1, true) then
        vsr_unavailable()
        apply()
    end
end)

mp.register_script_message('glasir-scaling', function(m)
    if not NAMES[m] then return end
    mode = m
    if m == 'vsr' then vsr_failed = not has_vsr() end
    apply()
    if m == 'vsr' and vsr_failed then
        toast(NAMES.vsr .. ' is not available here: FSR 1 instead')
    else
        toast('Scaling: ' .. NAMES[m])
    end
end)

-- Options changed while playing (script-opts; the tools try another VSR).
options_changed = function()
    NAMES.vsr = vsr_name()
    remove_vsr()
    vsr_failed = not has_vsr()
    apply()
end

mp.observe_property('osd-dimensions', 'native', function() soon() end)
mp.observe_property('video-params', 'native', function() soon() end)
mp.set_property('user-data/glasir/scaling', mode)
if mode == 'vsr' and vsr_failed then
    mp.msg.warn('this GPU has no video super resolution; FSR 1 instead')
end
