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
-- The GPU's video processor (the same filter) also runs NVIDIA RTX Video HDR,
-- which turns an SDR signal into HDR10: Glasir asks for it when the picture
-- is set to HDR and the console sends SDR (video.rtx_hdr in glasir.ini).
-- Where it cannot run, the SDR picture is shown as it is.
--
-- Options (script-opts glasir_scaling-...): mode, vsr (nvidia, intel or
-- none: what the GPU has), hdr (yes: HDR10 frames), hdr_peak (nits, as
-- tagged), rtx_hdr (yes: SDR frames through RTX Video HDR). While playing:
-- "script-message glasir-scaling <mode>" switches the mode (the in-game
-- menu), "script-message glasir-signal hdr|sdr|rtx" says what the frames are
-- from now on (Glasir, when the console's signal or the menu's choice
-- changes). user-data/glasir/scaling says the mode, user-data/glasir/rtx-hdr
-- whether RTX Video HDR runs (on, off or unavailable).
local mp = require 'mp'
local opts = { mode = 'mpv', vsr = 'nvidia', hdr = false, hdr_peak = 1000, rtx_hdr = false }
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
local LABELS = { '@glasir-vsr-in', '@glasir-vsr', '@glasir-vsr-out', '@glasir-rtx-hdr' }
local RTX_HDR = 'NVIDIA RTX Video HDR'
local mode = NAMES[opts.mode] and opts.mode or 'mpv'
-- What the frames are now: as mpv started, then as Glasir says. Not read
-- from the options again when those change (they say how mpv started).
local hdr = opts.hdr
local rtx_hdr = opts.rtx_hdr and not opts.hdr
-- The video processor in the filter chain: { scale = the scale VSR runs at
-- or nil, rtx = RTX Video HDR, hdr = made for HDR10 frames }; nil: none.
local processor = nil
local function has_vsr() return opts.vsr == 'nvidia' or opts.vsr == 'intel' end
local vsr_failed = not has_vsr()
local rtx_failed = false
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

local function remove_processor()
    if processor == nil then return end
    for _, label in ipairs(LABELS) do mp.command_native({ 'vf', 'remove', label }) end
    processor = nil
end

local function say_rtx_hdr()
    mp.set_property('user-data/glasir/rtx-hdr',
        processor and processor.rtx and 'on' or (rtx_failed and 'unavailable' or 'off'))
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

-- The video processor wanted for this scale (nil: the picture is not made
-- larger), or nil when there is nothing for it to do.
local function wanted_processor(scale)
    local vsr = mode == 'vsr' and not vsr_failed and scale ~= nil
    local rtx = rtx_hdr and not hdr and not rtx_failed
    if not vsr and not rtx then return nil end
    return { scale = vsr and scale or nil, rtx = rtx, hdr = hdr }
end

local function same_processor(a, b)
    if a == nil or b == nil then return a == b end
    if a.rtx ~= b.rtx or a.hdr ~= b.hdr or (a.scale == nil) ~= (b.scale == nil) then return false end
    return a.scale == nil or math.abs(a.scale - b.scale) <= 0.005
end

local function add_processor(p)
    local chain = {}
    -- The video processor shifts PQ / BT.2020 frames toward magenta when it
    -- scales them: they go in tagged as BT.709 and come out tagged as HDR10
    -- again. Only the tags change; the values pass through as they are.
    if p.hdr then
        chain[#chain + 1] = '@glasir-vsr-in:format=colormatrix=bt.709:colorlevels=limited:primaries=bt.709:gamma=bt.1886'
    end
    -- RTX Video HDR gives 10-bit RGB tagged BT.2020 PQ: mpv picks the format
    -- it needs and tags the frames itself.
    local does = {}
    if not p.rtx then does[#does + 1] = 'format=' .. (p.hdr and 'p010' or 'nv12') end
    if p.scale then does[#does + 1] = string.format('scaling-mode=%s:scale=%.6f', opts.vsr, p.scale) end
    if p.rtx then does[#does + 1] = 'nvidia-true-hdr=yes' end
    chain[#chain + 1] = (p.scale and '@glasir-vsr' or '@glasir-rtx-hdr') .. ':d3d11vpp=' .. table.concat(does, ':')
    if p.hdr then
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

local function rtx_unavailable()
    rtx_failed = true
    mp.msg.warn(RTX_HDR .. ' could not start; the SDR picture is shown as it is')
    toast(RTX_HDR .. ' is not available here: SDR is shown as it is')
end

-- Puts the chosen mode in place for the current sizes and frames.
local function apply()
    pending = nil
    local scale = wanted_scale()
    -- What cannot start is left out and the rest tried again.
    for _ = 1, 3 do
        local want = wanted_processor(scale)
        if same_processor(want, processor) then break end
        remove_processor()
        if want == nil then break end
        if add_processor(want) then
            processor = want
            mp.msg.info(string.format('video processor: %s%s%s',
                want.scale and string.format('%s at %.3fx', NAMES.vsr, want.scale) or '',
                want.scale and want.rtx and ', ' or '', want.rtx and RTX_HDR or ''))
            break
        end
        -- Both asked of one filter: the scaling alone tells which of them it was.
        if want.rtx and want.scale then
            local alone = { scale = want.scale, rtx = false, hdr = want.hdr }
            if add_processor(alone) then
                processor = alone
                rtx_unavailable()
                break
            end
            vsr_unavailable()
        elseif want.rtx then
            rtx_unavailable()
        else
            vsr_unavailable()
        end
    end
    say_rtx_hdr()
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
    if processor == nil then return end
    if processor.scale ~= nil and
        e.text:find('Failed to enable', 1, true) and e.text:find('Super Resolution', 1, true) then
        vsr_unavailable()
        apply()
    elseif processor.rtx and e.text:find('RTX Video HDR', 1, true) and
        (e.text:find('Failed to enable', 1, true) or e.text:find('not supported', 1, true)) then
        rtx_unavailable()
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

-- What the frames are from now on (Glasir: the console went from SDR to HDR
-- or back, or the menu's Picture changed). The tags are Glasir's; this
-- script's filters are made again for them.
mp.register_script_message('glasir-signal', function(kind)
    if kind ~= 'hdr' and kind ~= 'sdr' and kind ~= 'rtx' then return end
    hdr = kind == 'hdr'
    rtx_hdr = kind == 'rtx'
    apply()
end)

-- The menu's switch (Glasir hears it too, and says what the frames are):
-- switched on, RTX Video HDR is tried again where it could not start.
mp.register_script_message('glasir-rtx-hdr', function(state)
    if state == 'on' then rtx_failed = false end
end)

-- Options changed while playing (script-opts; the tools try another VSR).
options_changed = function()
    NAMES.vsr = vsr_name()
    remove_processor()
    vsr_failed = not has_vsr()
    apply()
end

mp.observe_property('osd-dimensions', 'native', function() soon() end)
mp.observe_property('video-params', 'native', function() soon() end)
mp.set_property('user-data/glasir/scaling', mode)
say_rtx_hdr()
if mode == 'vsr' and vsr_failed then
    mp.msg.warn('this GPU has no video super resolution; FSR 1 instead')
end
