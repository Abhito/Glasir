-- SPDX-License-Identifier: MIT
-- How Glasir scales the capture up to its place on the screen, before DLSS 5
-- and frame generation (they work on what mpv presents):
--
--   mpv      mpv's own scaler (Lanczos)
--   fsr      AMD FSR 1 (shaders/glasir_fsr1.glsl): edge-adaptive upscaling
--            and sharpening, any GPU
--   vsr      the GPU's video super resolution (mpv's d3d11vpp filter, in the
--            GPU's video processor): NVIDIA RTX or Intel; FSR 1 where it
--            cannot run. NVIDIA's quality level (1 to 4, Auto) cannot be
--            set from here: the filter can only switch the super resolution
--            on (NVIDIA's extension has version, method and enable, no
--            more), and the driver takes the level from its own setting
--            (the NVIDIA app: System, Video). Intel's has no levels
--   cnn      a small neural network that doubles the detail (ArtCNN), then
--            FSR 1 for what is left; any GPU, more GPU time
--   anime4k  Anime4K's neural upscaler, for cel-shaded and drawn styles;
--            any GPU, more GPU time
--   nvidia   NVIDIA's Video Super Resolution through its Video Effects SDK,
--            with a choice of model and without the loss of G-SYNC: not
--            done here but by Glasir's add-on in ReShade, on the frame mpv
--            presents (src/framegen, "NVIDIA Super Resolution"). This script
--            only has mpv draw the picture as large as it comes
--            (video-unscaled) and says so (user-data/glasir/sr = on), once
--            the add-on has said that it can ("script-message glasir-sr
--            ready"); until then, and where it cannot ("unavailable": no
--            ReShade, not downloaded, another GPU), FSR 1 as above. With
--            a target the add-on has the effect make that size and takes it
--            to the window itself (mpv's scaler has no part in it)
--
-- Nothing is done when the capture is not smaller than its place (a 1440p
-- capture on a 1440p screen). The scale follows the window: it is checked
-- again when the window or the capture changes size.
--
-- "Scale to" (target: a height in lines, 0 = the picture's place on the
-- screen) makes the picture that tall whatever the window's size, and mpv's
-- own scaler takes it from there to the window: down when the target is
-- larger than the screen (4K on a 1440p screen: supersampling; with Lanczos,
-- dscale in mpv.conf, as mpv's own default softens it), up when it is
-- smaller. The shaders size their output by the window (OUTPUT in their
-- //!WHEN, //!WIDTH and //!HEIGHT lines), and mpv gives them no other size to
-- go by: for a target, each is written out again with the target's numbers
-- in place of OUTPUT (shaders/sized_<w>x<h>_<name>, removed at the next
-- start). The originals stay as they are. mpv's own scaling has no step to
-- give a size to: there the target does nothing.
--
-- The GPU's video processor (the same filter) also runs NVIDIA RTX Video HDR,
-- which turns an SDR signal into HDR10: Glasir asks for it when the picture
-- is set to HDR and the console sends SDR (video.rtx_hdr in glasir.ini).
-- Where it cannot run, the SDR picture is shown as it is.
--
-- Options (script-opts glasir_scaling-...): mode, vsr (nvidia, intel or
-- none: what the GPU has), hdr (yes: HDR10 frames), hdr_peak (nits, as
-- tagged), rtx_hdr (yes: SDR frames through RTX Video HDR), target (lines,
-- 0: the screen). While playing:
-- "script-message glasir-scaling <mode>" switches the mode (the in-game
-- menu), "script-message glasir-target <lines>" the size to scale to,
-- "script-message glasir-signal hdr|sdr|rtx" says what the frames are
-- from now on (Glasir, when the console's signal or the menu's choice
-- changes). user-data/glasir/scaling says the mode, user-data/glasir/target
-- the size to scale to, user-data/glasir/rtx-hdr whether RTX Video HDR runs
-- (on, off or unavailable), user-data/glasir/scaled what is in place now: the
-- size the picture is made and by which mode ("3840x2160 fsr": FSR 1 also
-- where it stands in for another), or nothing where mpv's own scaler or the
-- add-on does it, or the picture is not made larger.
--
-- "Sharpen" (sharpen: 0 off, 1 low, 2 medium, 3 high; "script-message
-- glasir-sharpen <n>" while playing, user-data/glasir/sharpen says it) is
-- apart from the scaling: FSR 1's sharpening pass alone
-- (shaders/glasir_rcas.glsl) on the picture mpv has finished, at the size it
-- is shown - after mpv's scaler and the shaders above, or on a picture
-- nothing scaled. With mode = nvidia the add-on scales after mpv, so there it
-- is the small picture that is sharpened, before NVIDIA's model.
local mp = require 'mp'
local utils = require 'mp.utils'
local opts = { mode = 'mpv', vsr = 'nvidia', hdr = false, hdr_peak = 1000, rtx_hdr = false, target = 0, sharpen = 0 }
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
local NAMES = { mpv = 'mpv (Lanczos)', fsr = 'AMD FSR 1', cnn = 'ArtCNN', anime4k = 'Anime4K',
                nvidia = 'NVIDIA Super Resolution' }
local function vsr_name()
    return ({ nvidia = 'NVIDIA RTX Video Super Resolution', intel = 'Intel Video Super Resolution' })[opts.vsr]
        or 'Video Super Resolution'
end
NAMES.vsr = vsr_name()
local LABELS = { '@glasir-vsr-in', '@glasir-vsr', '@glasir-vsr-out', '@glasir-rtx-hdr' }
local RTX_HDR = 'NVIDIA RTX Video HDR'
local mode = NAMES[opts.mode] and opts.mode or 'mpv'
-- The height to scale to, in lines; 0: the picture's place in the window.
local function lines(n)
    n = math.floor((tonumber(n) or 0) / 2) * 2
    return (n >= 480 and n <= 4320) and n or 0
end
local target = lines(opts.target)
local target_failed = false  -- a shader could not be written out for the target: by the window instead
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
-- mode = nvidia: what the add-on last said ('ready' or 'unavailable'; nil:
-- nothing yet), and whether mpv draws the picture as large as it comes for it.
local sr_state = nil
local unscaled = false
local unscale_timer = nil

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

-- The scale that makes the capture fill its place - or, with a target, as
-- tall as the target - or nil when it already is (or the sizes are not known
-- yet). With a target also the size that makes: { w, h }. Third, for what
-- is said of it (user-data/glasir/scaled): the capture's size and, without a
-- target, its place in the window: { w, h, place = { w, h } }.
local MAX_SIDE = 16384  -- the widest picture Direct3D 11 holds: no target is made wider (the add-on counts the same)
local function wanted_scale()
    local w = mp.get_property_number('video-params/w')
    local h = mp.get_property_number('video-params/h')
    if not w or not h or w < 16 or h < 16 then return nil end
    local fixed = target > 0 and not target_failed
    local fit, place
    if fixed then
        fit = math.min(target / h, 4, MAX_SIDE / w)
    else
        local dims = mp.get_property_native('osd-dimensions')
        if not dims then return nil end
        local ow = dims.w - (dims.ml or 0) - (dims.mr or 0)
        local oh = dims.h - (dims.mt or 0) - (dims.mb or 0)
        if ow < 16 or oh < 16 then return nil end
        place = { w = ow, h = oh }
        -- Drawn as large as it comes, the margins are around the small
        -- picture: its place is what fits the window.
        if mode == 'nvidia' then
            ow, oh = dims.w, dims.h
            local whole = math.min(ow / w, oh / h)
            place = { w = math.floor(w * whole + 0.5), h = math.floor(h * whole + 0.5) }
        end
        fit = math.min(ow / w, oh / h, 4)
    end
    if fit <= 1.001 then return nil end
    -- Even output sizes, which the video processor wants.
    local out_h = math.floor(h * fit / 2) * 2
    local scale = out_h / h
    return scale, fixed and { w = math.floor(w * scale / 2 + 0.5) * 2, h = out_h } or nil, { w = w, h = h, place = place }
end

-- G-SYNC and NVIDIA's super resolution. The NVIDIA driver does not run
-- G-SYNC on a Direct3D device that RTX Video Super Resolution runs on, and
-- leaves it off for that device once the filter is gone (found 2026-11-19 with
-- PresentMon and the driver's own word, in full screen and in a window:
-- another filter, or the video processor without super resolution, keep
-- G-SYNC; leaving full screen and coming back, or telling the driver "off"
-- on a video processor of that device, do not bring it back). A new device
-- has it again. So when this scaling is left for another, where G-SYNC is
-- known to run on this PC (the frame generation add-on has seen the driver
-- run it and says so: user-data/glasir/gsync), mpv makes its output anew:
-- two to four seconds without a picture, once. While the super resolution
-- runs there is no G-SYNC, and nothing here can change that. (ReShade loads
-- its add-ons anew with the new device; the in-game menu opens again if it
-- was open, which mpv keeps for it in user-data/glasir/menu.)
local vsr_ran = false  -- NVIDIA's super resolution ran on mpv's output as it is now
local vsr_ran_before = false  -- what vsr_ran was before the filter was last asked for

local function output_anew()
    vsr_ran = false
    vsr_ran_before = false
    mp.msg.info("leaving NVIDIA's super resolution: mpv's output is made anew, which brings G-SYNC back")
    -- The new window as large as this one (mpv would make it by the size it
    -- started with; another size is also another wait while ReShade builds
    -- its effects for it). In mpv's own full screen the size is the screen's.
    local fullscreen = mp.get_property_bool('fullscreen')
    local dims = mp.get_property_native('osd-dimensions')
    if not fullscreen and dims and (dims.w or 0) >= 64 and (dims.h or 0) >= 64 then
        mp.set_property('geometry', string.format('%dx%d', dims.w, dims.h))
    end
    -- d3d11-warp says whether a software renderer may stand in where there is
    -- no GPU: auto and no are the same with one, and a change of it is what
    -- makes mpv build its output (device, swap chain, window) again.
    mp.set_property('d3d11-warp', mp.get_property('d3d11-warp') == 'no' and 'auto' or 'no')
    -- The window is a new one: Glasir, which keeps a borderless one in its
    -- place, is told (mpv's own full screen is where it should be by itself).
    mp.add_timeout(1.0, function()
        if not mp.get_property_bool('fullscreen') then mp.commandv('script-message', 'glasir-window', 'rebuilt') end
    end)
end

-- mode = nvidia: mpv draws the picture as large as it comes and the add-on
-- makes it larger, or mpv scales again. The two do not hear of a change at
-- the same moment, and the add-on would make a mess of a frame mpv had
-- already scaled: so mpv draws small a moment before the add-on is told to
-- scale, and the add-on is told to stop a moment before mpv scales again.
local function set_unscaled(on)
    if on == unscaled then return end
    unscaled = on
    if unscale_timer then unscale_timer:kill() end
    if on then
        mp.set_property('video-unscaled', 'yes')
        unscale_timer = mp.add_timeout(0.15, function() mp.set_property('user-data/glasir/sr', 'on') end)
    else
        mp.set_property('user-data/glasir/sr', 'off')
        unscale_timer = mp.add_timeout(0.15, function() mp.set_property('video-unscaled', 'no') end)
    end
end

-- A shader's copy that sizes its output by this size instead of the window's:
-- its path as mpv takes it, or nil if it could not be written.
local sized_made = {}
local function sized(shader, size)
    local out = string.format('~~/shaders/sized_%dx%d_%s', size.w, size.h, shader:match('[^/]+$'))
    if sized_made[out] then return out end
    local from = io.open(mp.command_native({ 'expand-path', shader }), 'rb')
    if not from then return nil end
    local text = from:read('*a')
    from:close()
    -- Only in the lines that say when a pass runs and how large its output is.
    text = text:gsub('//![^\n]*', function(line)
        return (line:gsub('OUTPUT%.w', tostring(size.w)):gsub('OUTPUT%.h', tostring(size.h)))
    end)
    local to = io.open(mp.command_native({ 'expand-path', out }), 'wb')
    if not to then return nil end
    local ok = to:write(text)
    to:close()
    if not ok then return nil end
    sized_made[out] = true
    return out
end

-- The copies of an earlier run: none is in use when the script starts.
local function remove_sized()
    local dir = mp.command_native({ 'expand-path', '~~/shaders' })
    for _, name in ipairs(utils.readdir(dir, 'files') or {}) do
        if name:find('^sized_%d+x%d+_') then os.remove(utils.join_path(dir, name)) end
    end
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
    local scale, size, capture = wanted_scale()
    -- The add-on scales (mode = nvidia, and it has said that it can): mpv
    -- only draws the picture as it comes - which it can only where the whole
    -- of it fits the window (a larger one it would cut off).
    local own = mode == 'nvidia' and sr_state == 'ready' and scale ~= nil
    if own then
        local dims = mp.get_property_native('osd-dimensions')
        own = dims ~= nil and (mp.get_property_number('video-params/w') or 1e9) <= dims.w and
            (mp.get_property_number('video-params/h') or 1e9) <= dims.h
    end
    set_unscaled(own)
    -- What cannot start is left out and the rest tried again.
    for _ = 1, 3 do
        local want = wanted_processor(scale)
        if same_processor(want, processor) then break end
        remove_processor()
        if want == nil then break end
        if add_processor(want) then
            processor = want
            vsr_ran_before = vsr_ran
            vsr_ran = vsr_ran or (want.scale ~= nil and opts.vsr == 'nvidia')
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
                vsr_ran_before = vsr_ran
                vsr_ran = vsr_ran or opts.vsr == 'nvidia'
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
    local by = nil  -- what makes the picture larger here (a mode's name); nil: nothing, or the add-on
    if scale ~= nil and not own then
        want = SHADERS[mode] or {}
        by = SHADERS[mode] and mode or nil
        if mode == 'vsr' then
            if vsr_failed then want, by = SHADERS.fsr, 'fsr' else by = processor and processor.scale and 'vsr' or nil end
        end
        if mode == 'nvidia' then want, by = SHADERS.fsr, 'fsr' end  -- until the add-on is ready, and where it cannot be
    end
    if size ~= nil then
        -- For a target: each shader's copy with the target's size in it.
        local copies = {}
        for i, shader in ipairs(want) do
            copies[i] = sized(shader, size)
            if not copies[i] then
                -- Everything by the window then, the video processor too.
                target_failed = true
                mp.msg.warn(string.format('could not write %s for %dx%d; scaling to the window', shader, size.w, size.h))
                toast('Scale to: the shaders could not be written, scaling to the screen')
                return apply()
            end
        end
        want = copies
    end
    set_shaders(want)
    mp.set_property('user-data/glasir/scaling', mode)
    mp.set_property('user-data/glasir/target', tostring(target))
    -- For the menu's top line: the size the picture is made, and by what.
    -- With a target, that size. By the window, the picture's place: FSR 1
    -- and Anime4K end there whatever the scale (OUTPUT), ArtCNN makes twice the
    -- capture and FSR 1 or mpv's scaler takes that to the place; the video
    -- processor makes what its scale gives (four times at most; mpv's scaler
    -- does the rest). It is what was asked of the mode: its shaders still
    -- choose by the scale which of their passes run (ArtCNN from 1.3x, Anime4K
    -- from 1.2x and by whole doublings).
    local made = ''
    if by then
        local to = size
        if not to and by == 'vsr' then
            to = { w = math.floor(capture.w * scale + 0.5), h = math.floor(capture.h * scale + 0.5) }
        end
        to = to or capture.place
        made = string.format('%dx%d %s', to.w, to.h, by)
    end
    mp.set_property('user-data/glasir/scaled', made)
end

-- Sharpen: RCAS on mpv's finished picture. Its strength in the shader is in
-- stops under RCAS's most, each one halving it: a quarter, a half and 71 % of
-- it (measured on a console's 1440p HDR picture, 2026-11-20: the finest
-- detail's energy x1.2, x1.7 and x2.4; all of it, x5.8, is mostly grain).
local RCAS = '~~/shaders/glasir_rcas.glsl'
local SHARPEN_STOPS = { 2.0, 1.0, 0.5 }
local SHARPEN_NAMES = { [0] = 'Off', 'Low', 'Medium', 'High' }
local function sharpen_level(n)
    n = math.floor(tonumber(n) or 0)
    return (n >= 0 and n <= 3) and n or 0
end
local sharpen = sharpen_level(opts.sharpen)
local sharpening = false  -- the shader is in mpv's list

local function apply_sharpen()
    if sharpen > 0 then
        -- The shader's parameter, beside whatever else is set there.
        local set = mp.get_property_native('glsl-shader-opts') or {}
        set.glasir_sharpen = string.format('%.2f', SHARPEN_STOPS[sharpen])
        mp.set_property_native('glsl-shader-opts', set)
    end
    if (sharpen > 0) ~= sharpening then
        sharpening = sharpen > 0
        mp.commandv('change-list', 'glsl-shaders', sharpening and 'append' or 'remove', RCAS)
    end
    mp.set_property('user-data/glasir/sharpen', tostring(sharpen))
end

mp.register_script_message('glasir-sharpen', function(n)
    sharpen = sharpen_level(n)
    apply_sharpen()
    toast('Sharpen: ' .. SHARPEN_NAMES[sharpen])
end)

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
        vsr_ran = vsr_ran_before  -- the driver turned it down on its first frame: it did not run
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
    elseif m == 'nvidia' and sr_state == 'unavailable' then
        toast(NAMES.nvidia .. ' is not available here: FSR 1 instead')
    elseif m ~= 'vsr' and vsr_ran and mp.get_property_native('user-data/glasir/gsync') == 'yes' then
        -- Said once the picture is back (the overlay that shows it is made anew too).
        output_anew()
        mp.add_timeout(3.0, function() toast('Scaling: ' .. NAMES[m] .. ' (the picture started again, for G-SYNC)') end)
    else
        toast('Scaling: ' .. NAMES[m])
    end
end)

-- The add-on, whenever mode = nvidia and it knows: it can make the picture
-- larger itself (ready), or cannot (unavailable: FSR 1 in mpv instead).
mp.register_script_message('glasir-sr', function(state)
    if state ~= 'ready' and state ~= 'unavailable' then return end
    local before = sr_state
    sr_state = state
    if mode ~= 'nvidia' then return end
    apply()
    if state == 'unavailable' and before ~= 'unavailable' then
        mp.msg.warn(NAMES.nvidia .. ' cannot run (engine\\ReShade.log says why); FSR 1 instead')
        toast(NAMES.nvidia .. ' is not available here: FSR 1 instead')
    end
end)

-- The size to scale to (the in-game menu's Supersample switch, and "Scale
-- to" under it): a height in lines, 0 for the picture's place on the screen,
-- which is the switch off.
local TARGETS = { [1440] = '1440p', [2160] = '4K', [2880] = '5K', [4320] = '8K' }
mp.register_script_message('glasir-target', function(n)
    target = lines(n)
    target_failed = false
    apply()
    local name = target == 0 and 'off' or TARGETS[target] or (target .. ' lines')
    if target_failed then return end  -- said by apply()
    toast('Supersample: ' .. name .. (target > 0 and mode == 'mpv' and " (not used by mpv's own scaling)" or ''))
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

remove_sized()
mp.observe_property('osd-dimensions', 'native', function() soon() end)
mp.observe_property('video-params', 'native', function() soon() end)
mp.set_property('user-data/glasir/scaling', mode)
mp.set_property('user-data/glasir/target', tostring(target))
mp.set_property('user-data/glasir/sr', 'off')
mp.set_property('user-data/glasir/scaled', '')
apply_sharpen()
say_rtx_hdr()
if mode == 'vsr' and vsr_failed then
    mp.msg.warn('this GPU has no video super resolution; FSR 1 instead')
end
