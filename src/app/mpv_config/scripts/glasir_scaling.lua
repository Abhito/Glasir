-- SPDX-License-Identifier: MIT
-- How Glasir scales the capture up to its place on the screen, before DLSS 5
-- and frame generation (they work on what mpv presents):
--
--   mpv   mpv's own scaler (Lanczos)
--   fsr   AMD FSR 1 (shaders/glasir_fsr1.glsl): edge-adaptive upscaling and
--         sharpening, any GPU
--   vsr   NVIDIA RTX Video Super Resolution (mpv's d3d11vpp filter, in the
--         GPU's video processor), RTX GPUs; FSR 1 where it cannot run
--
-- Nothing is done when the capture is not smaller than its place (a 1440p
-- capture on a 1440p screen). VSR's scale follows the window: it is set again
-- when the window or the capture changes size.
--
-- Options (script-opts glasir_scaling-...): mode, hdr (yes: HDR10 frames),
-- hdr_peak (nits, as tagged). "script-message glasir-scaling <mode>" switches
-- while playing (the in-game menu); user-data/glasir/scaling says the mode.
local mp = require 'mp'
local opts = { mode = 'mpv', hdr = false, hdr_peak = 1000 }
require('mp.options').read_options(opts, 'glasir_scaling')

local SHADER = '~~/shaders/glasir_fsr1.glsl'
local LABELS = { '@glasir-vsr-in', '@glasir-vsr', '@glasir-vsr-out' }
local mode = opts.mode
local vsr_scale = nil  -- the scale VSR runs at; nil: not in the filter chain
local vsr_failed = false
local pending = nil

local function toast(text)
    mp.commandv('script-message', 'glasir-toast', text)
end

local function set_fsr(on)
    local list = mp.get_property_native('glsl-shaders') or {}
    local present = false
    for _, s in ipairs(list) do
        if s == SHADER or s:find('glasir_fsr1.glsl', 1, true) then present = true end
    end
    if on and not present then
        mp.commandv('change-list', 'glsl-shaders', 'append', SHADER)
    elseif not on and present then
        mp.commandv('change-list', 'glsl-shaders', 'remove', SHADER)
    end
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
    chain[#chain + 1] = string.format('@glasir-vsr:d3d11vpp=format=%s:scaling-mode=nvidia:scale=%.6f',
        opts.hdr and 'p010' or 'nv12', scale)
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

-- Puts the chosen mode in place for the current sizes.
local function apply()
    pending = nil
    if mode == 'vsr' and not vsr_failed then
        set_fsr(false)
        local scale = wanted_scale()
        if scale == nil then
            remove_vsr()
        elseif vsr_scale == nil or math.abs(vsr_scale - scale) > 0.005 then
            remove_vsr()
            if add_vsr(scale) then
                vsr_scale = scale
                mp.msg.info(string.format('RTX VSR at %.3fx', scale))
            else
                vsr_failed = true
                mp.msg.warn('RTX Video Super Resolution could not start; FSR 1 instead')
                toast('RTX Video Super Resolution is not available here: FSR 1 instead')
            end
        end
    end
    if mode ~= 'vsr' then remove_vsr() end
    set_fsr(mode == 'fsr' or (mode == 'vsr' and vsr_failed))
    mp.set_property('user-data/glasir/scaling', mode)
end

-- Sizes settle over a few events (the window opening, full screen): once.
local function soon()
    if pending then pending:kill() end
    pending = mp.add_timeout(0.3, apply)
end

local NAMES = { mpv = 'mpv (Lanczos)', fsr = 'AMD FSR 1', vsr = 'NVIDIA RTX Video Super Resolution' }

mp.register_script_message('glasir-scaling', function(m)
    if not NAMES[m] then return end
    mode = m
    if m == 'vsr' then vsr_failed = false end
    apply()
    toast('Scaling: ' .. NAMES[m])
end)

mp.observe_property('osd-dimensions', 'native', function() if mode == 'vsr' then soon() end end)
mp.observe_property('video-params', 'native', function() soon() end)
mp.set_property('user-data/glasir/scaling', mode)
