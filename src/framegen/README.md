# Glasir frame generation

<!-- SPDX-License-Identifier: MPL-2.0 -->

Frame generation inside mpv, with no second app running: the ReShade add-on
`zz-glasir-framegen.addon64` (this folder, Mozilla Public License 2.0) and
Glasir's dedup feeding it. This is the design document carried over from the
prototype (MPV-SW-Capture's FrameGen work, 2026-10-24 to 10-31): the
measurements below were taken there, with the same code.

## What it does

1. **Dedup in `Glasir.exe`** (`[frames] dedup = 1`), which feeds mpv from the
   card: 10-bit P010 for HDR, 8-bit NV12 (or YUY2/UYVY) for SDR.
   A 30 fps game captured at 60 arrives
   as pairs of identical frames (A A B B). With `--dedup` only new frames are
   written to mpv, so mpv, the DLSS 5 pass and the frame generator all see the
   game's real 30 fps. Repeats need not be bit-exact: the PS5 adds faint
   noise to every refresh (a quarter of the pixels move by 1-3 8-bit steps),
   so frames are compared by 16x16 block averages, in which the noise
   cancels, plus single pixels that moved far (a HUD digit). Repeats are
   still sent every 200 ms so menus and the OSD
   keep drawing on a still picture. `--dedup` also opens a link to the add-on
   (`framegen_link.h`) with each new frame's arrival time and how many new
   frames mpv had to drop.
2. **`zz-glasir-framegen.addon64`**, a ReShade add-on that runs last, after the
   DLSS 5 pass. For each real frame B it shows a frame between the previous
   real frame A and B, then B half a frame later: 30 fps becomes 60, 60 becomes
   120.

The frame in between is **motion-compensated** (`Method=motion`): the add-on
finds how every part of the picture moved from A to B and draws each pixel
from where it was in A and where it went in B. `Method=blend` gives the old
50/50 mix (a double image on motion) for comparison.

## Use

Frame generation is on by default (`glasir.ini`: `[framegen] enabled = 1`)
and needs dedup (`[frames] dedup = 1`) and ReShade in `engine\`. **F8**
toggles it while playing (mpv's OSD says ON/OFF); the next start follows
`glasir.ini` again. With dedup off, F8 only says it needs dedup.

While frame generation is off, mpv gets every frame anyway, repeats too
(`[framegen] all_frames_when_off = 1`): the add-on asks Glasir for them
through the link, which keeps telling new frames from repeats, so F8 back on
returns to new frames only at once (`logs\glasir.log` notes each switch).
`all_frames_when_off = 0` keeps new frames only while frame generation is off
as well (DLSS 5 then runs once per game frame instead of on every captured
one). Measured at 30 fps with VRR, mpv's present to the screen: 6.77 ms with
new frames only and 5.72 ms with every frame at the driver's default power
setting (the GPU keeps its clocks up at 60 frames a second); with NVIDIA's
"Prefer maximum performance" for mpv both are 5.6-5.7 ms - but that setting
made every frame cost the GPU more at 60 fps (below), so the default is the
better choice.

Settings are in `ReShade.ini`, section `[GLASIR_FRAMEGEN]`:

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | Frame generation on at start (Glasir writes it from glasir.ini). |
| `ToggleKey` | 119 | Virtual key code of the toggle (119 or 0x77 = F8). |
| `CaptureKey` | 120 | Saves the frames around this moment (120 = F9, see Captures). |
| `DedupWhenOff` | 0 | 1 = new frames only while frame generation is off too (glasir.ini `all_frames_when_off = 0`). |
| `MaxOutputHz` | 0 | Highest output rate; 0 = the display's refresh rate. |
| `ResetMs` | 100 | A longer gap between frames starts the timing over. |
| `RequireDedupLauncher` | 1 | Only run with the link from Glasir's dedup. |
| `Method` | motion | `motion` = motion-compensated, `blend` = 50/50 mix. |
| `Quality` | high | `high`, or `fast`: fewer candidates per pixel, about half the drawing cost (glasir.ini). |
| `DebugMarker` | 0 | Magenta square in the top-left corner of generated frames. |
| `LogStats` | 1 | A line in `ReShade.log` every 5 s (see below). |
| `MpvPipe` | glasir | mpv IPC pipe for the OSD message. |

## When it generates

A frame in between is made only when

- twice the source rate fits the display (`MaxOutputHz`, or its refresh rate),
- fewer than two presents are still queued for the screen,
- the source is not paused (a still picture sends only keepalive repeats),
- mpv keeps up. New frames dropped upstream right after a generated frame
  (with mpv at its usual pace) pause generation for 5 s, then 10, 20 ... up to
  60 s each time it happens again; 30 s after the last pause and the last such
  drop reset that. Drops of mpv's own - across a stall far longer than the
  add-on's wait (DLSS 5 starting, shaders recompiling), or while not
  generating - only make it wait until mpv has gone a second without dropping.
  It also waits 2 s after mpv starts.

The source rate comes from the link (the card's arrival times), not from
mpv's frames: while generating, mpv runs at the add-on's pace, so only the
card's clock can tell that the display is too slow for twice the source rate.

## Logs

`ReShade.log`, every 5 s:

```
30.0 real + 30.0 generated fps | interval 33.32 ms (source) | display 165 Hz, cap 165 Hz
| 0 late frames, 0 dropped upstream after a generated frame | not generated: 0 too fast,
0 queue full, 0 source paused, 0 backing off, 0 mpv settling, 0 effects mix
| mpv present 0.04 ms avg, 0.11 max | real frame held on the GPU 16.68 ms avg after the
frame in between was done (done 9.41 ms avg after the add-on drew it), let go 0.017 ms late
avg, 0.029 max; of 151: 151 timed (0 of them let go early for the next frame, 0 already due
when done), 0 without the done signal, 0 with a later frame (queue full, resize); queued in
10.00 ms max
```

"mpv present" is how long mpv's present of the generated frame blocked. The
real frame is presented right after it but held on the GPU, and a thread of
the add-on lets it go half a source interval after the frame in between was
done (that is when VRR shows it): "held ... after the frame in between was
done" is how long it actually waited, "let go late" how far after its time
the thread did it. Of the held frames, "timed" ones were let go by that
thread; "let go early for the next frame" were let go before their half so as
not to hold up the next frame's DLSS 5 pass (it queues behind the hold),
"already due when done" had their deadline pass before the frame in between
was even done (the GPU is at its limit); "without the done signal" were let
go after 200 ms without one (a GPU reset); "with a later frame" were let go
together with a newer one (the hold's queue was full, or the window
resized). "Queued in" is how long the copy and present of the real frame
took on mpv's thread (Windows makes the present wait until the frame in
between is drawn). "None in this window" means nothing was generated. Without
the hold - no Direct3D 12, which the log says at the first generated frame,
or `GLASIR_FRAMEGEN_PACING=present` for comparisons - the add-on sleeps on mpv's
thread until half an interval after mpv's present and the line ends
`real frame out late by ...` instead.

In the trace (`GLASIR_FRAMEGEN_TRACE`), `real_ms` is when the real frame was let
go on the CPU: the release of the hold, or (without it) the end of the sleep
before its present; -1 when it went out with a later frame. Neither is when
it reached the screen (PresentMon has that).

`logs\glasir.log`:

- `dedup: 28.8 new frames/s of 57.5 captured (changes: repeats up to 0, new
  frames from 2556, new from 4; repeats' noise up to 5 a sample, 1.0 a block;
  threshold 3)` (a PS5 game at 30 fps): a change is a 16x16 pixel block whose
  average moved by more than the threshold, or one sampled pixel that moved by
  more than 8 times it, from the last frame sent; a frame with 4 changes is
  new. The threshold (`--dedup-threshold`, default 3) is in 10-bit steps for
  NV12 too: one 8-bit step is 4. The noise is how far the repeats' samples and
  block averages moved: 0 for a bit-exact source like the Switch 2, about 8
  and 1.5-1.8 for the PS5 in NV12 (5 and 1.0 in P010). If repeats come close
  to 4 changes, or the block noise close to the threshold, raise it. Near 60
  new frames/s from a game meant to run at 30, with new frames from thousands
  of changes, means the picture really changes on every refresh (a 40 fps or
  VRR mode, say), not that repeats slip through.
- `mpv fell behind: 1 of 150 new frames dropped in the last 5.0 s`: mpv did
  not keep up, with or without frame generation. The first seconds always
  show some (`... dropped before mpv started reading`, then DLSS 5 starting).

## How it works

- `reshade_present` (the back buffer is final: the DLSS 5 pass ran during
  ReShade's effects): copy B out, draw the frame between A and B into the back
  buffer, return. mpv's own present shows it.
- `finish_present` (ReShade's locks are released): copy B back into the back
  buffer and present it through the original swap chain, held on the GPU: the
  queue gets "the frame in between is done" (a D3D11 fence), the copy, a wait
  for a second fence and the present, and mpv's thread goes on. A thread of
  the add-on sees the first fence reach the frame, sleeps until half the
  source interval later - at most until just before the next frame's effects
  are due - and moves the second fence on from the CPU (a Direct3D 12 fence on
  the same GPU, shared with D3D11: D3D11 cannot signal one from the CPU). With
  VRR that puts B on screen half an interval after the frame in between.
  Without the hold (no Direct3D 12, or `GLASIR_FRAMEGEN_PACING=present`) it
  sleeps until half the source interval after B arrived and presents B then.
  That present bypasses ReShade, so the DLSS 5 add-ons still see exactly one
  frame per real frame.
- Two frames are only mixed when ReShade's effects ran for both or for neither.

The frame in between (`fg_interp.h`, shaders in `flow.hlsl`):

1. Luma pyramids of A and B, from half resolution down (up to 7 levels).
2. The motion both ways (A to B, B to A), coarsest level first: every 8x8
   block is searched for around twice the estimate from the level above, the
   best offset refined to a fraction of a pixel, then a 3x3 vector median. The
   search follows the design AMD documents for FidelityFX Optical Flow (MIT),
   written for shader model 5. Each block is also searched for around no
   motion, and what that finds is taken if it fits clearly better (at most
   half as badly) than what the search around the estimate found: a
   character the camera follows or turns around stays nearly still on screen
   while the scenery sweeps past, and a small part of it - the head - is lost
   to the scenery on the coarse levels; the search near the scenery's motion
   never finds its own. Without this, the head took the scenery's motion, and
   the frame in between showed faint copies of the hair and face beside it.
   Both are judged after their search: judged before it, the estimate (a few
   pixels off) lost to no motion on even stone, and a pillar the camera
   swept past got a hole. Trying the neighbouring blocks' estimates as well
   was worse: on stairs, a repeating pattern, a wrong one fits too and spread
   into blotches.
3. The motion at the frame in between, per 16x16 pixels: of the motions found
   nearby in both directions (and those found where each says the content came
   from and went to), the one under which A, shifted back half way, and B,
   shifted forward half way, agree best.
4. Each pixel tries the motions of the 3x3 cells around it and none, judged
   on a small cross of five pixels by how well A and B agree there; a motion
   several cells share is sampled once and counts as many votes. Where only
   one frame shows the right thing (background a moving object uncovers, the
   edge of the screen in a pan) a motion may also be drawn from that frame
   alone - if that frame's own motion agrees, and not where that frame's pixel
   plainly stands still (a character the camera turns around). The options
   are mixed by how well they fit, as softly as the best one fits badly: where
   one fits well the result is that one (no faint copies of the others - the
   ghosting around characters), where nothing fits (UI fading or pulsing over
   moving scenery, glass, smoke) a soft blend instead of blocks. Where nothing
   fits and no one-frame option explains the pixel either (lightning, sparks,
   an attack faster than the search), the mix may go softer still: a smear
   like motion blur instead of misplaced pieces. Pixels that are the same all
   around in both frames (black bars, a still HUD) are drawn as they are,
   without a search. And what a pixel shows in both frames it shows in
   between: near something that stands still (the character the camera
   follows or turns around, a HUD), a moving option that would bring
   something else to a pixel where both frames show the same pays for it.
   Without that, the camera turning around a character showed the ground
   through the top of the head - the spiky hair turns a little between the
   frames, and the ground beside the head, carried along with the scenery,
   fitted better. Something moving further than its own width past a still
   thing (a fast spear tip) is drawn a little less in between instead.
   Beside a character the camera turns around fast, the strip only one frame
   shows (half the scenery's step wide, 65 px in a quick orbit) has to come
   from the right frame: the other one's sample lands on the character, on a
   spike of hair poking out of blocks that carry the scenery's motion, and
   costs the same - mixed in, it drew a faint copy of the hair tips beside
   the real ones (three tips where there are two) or a faint second outline
   of the head. So a one-frame option along the scenery's motion is ruled out
   where its sample lies on (or within up to 32 px of) a block of its frame
   moving under a quarter as fast, while the other frame's sample is clear
   of one. Only where most cells around move with the scenery, so a spear
   swung across still scenery or a character running past is not touched,
   and only one way round: where both samples or neither lie near something
   slow (smoke, an effect), nothing changes.
5. Nothing in step 4 may change at the 16-pixel cell edges: the set of cells a
   pixel tries moves on by a row or column there, so each cell's say fades out
   over its last half cell instead of stopping, and a frame's own motion is
   taken between its blocks, not per block. Otherwise wherever cells disagree
   (characters, effects) the frame in between shows the cell grid as blocks.
6. The character as a layer (`layer_enable`, 2026-10-29). Steps 1-5 judge
   each pixel by which motion makes A and B agree. Thin parts of the player
   character - the spikes of the hair - deform as it runs or turns, so no
   motion fits them well, while the smooth floor behind them fits the
   scenery's motion almost perfectly: the frame in between showed floor where
   the spikes were ("the head phasing out"). No per-pixel rule can fix that
   (four were tried: anything allowed to paste the hair also pastes repeating
   texture one step off). So the character is found as a whole and drawn on
   top:
   - The background motion is modelled locally (an affine fit per 128 px
     block around the most common motion of the cells - perspective makes a
     floor's motion vary across the frame). A cell is a candidate when its own
     motion differs from that by more than 6 px and fits clearly better
     (by 0.02 luma).
   - Candidates are linked into components; one compact component is kept
     (12-1500 cells, filled at least a quarter of its box, near the centre,
     not mostly black, at most 40 cells wide, moving at most 40 px a frame -
     wider or faster ones are a close-up body or an effect, and then there is
     no layer for that frame). Its motion is searched directly on its cells.
     A deletion vote (does the scenery slide under it?) drops it when the
     order looks wrong. In a pan there is one motion and nothing runs.
   - In each frame, a pixel mask grows from the component by colour - closer
     to the layer's local colour than to the background's, or carried onto
     the other frame by the layer's own motion - within 4 cells, and only
     where the background motion does not explain the pixel (checked within
     1.5 px). Enclosed holes are filled; nothing is dilated or closed (that
     would fill the gaps between letters).
   - The masks are moved to the frame in between with the layer's motion. The
     layer is drawn from frame A only, replacing what step 4 drew with the
     background's motion: the spike comes back solid, in A's shape (half a
     frame behind its true shape - the user's choice over a missing or
     doubled spike). B's copy of a deformed spike lands a few pixels off, so
     B alone is never drawn there.
   - What counts as "the background's motion" there: within 6 px of the
     local model, or within 30 % of how fast the background moves past the
     layer if that is more (`layer_bgnear`, 2026-10-29). In a fast camera turn
     around a running character (the floor moving ~115 px a frame) the floor
     beside the head moves 92-128 px across 300 px of perspective and the model
     is 10-15 px off; with 6 px alone the floor drawn over the top of the head
     was left in place (20261029-020140-625, first generated frame). Counting
     anything nearer the background than the layer took a swinging arm too
     and pasted flecks on it.
   - Fast turns while running (2026-10-29 evening, captures 20261029-1817*,
     floor ~90-140 px a frame):
     - A component too fast or too wide to be the character (a far
       pillar's parallax) no longer cancels the layer for the frame; it is
       left out of the choice and the next one (the head) can win
       (`layer_select_gates`). Components far off the middle (HUD, minimap)
       still cannot.
     - The background model also counts plain cells, at half weight
       (`layer_bg_flat`). A fast turn blurs the floor until hardly any
       cell has detail, and the detailed ones were the character's: the
       model said the background stood still beside the head, and half
       the hair was left out of the mask in a straight line.
     - The "another motion nearby explains this pixel" rule no longer asks
       the layer's own cells (`layer_nb_own`): a head turning on a running
       body cut 16 px squares out of its own mask.
     - Off the layer, an option that does not move with it but samples the
       layer in one frame pays 1 (`layer_ghost`): in a fast turn the
       scenery's motion picked up the head from one frame and drew it again
       half a scenery-step away (the afterimage beside the hair).
     On the old captures this keeps every score, halves the ghost count on
     the running set (338 to 184 px) and cuts it on the orbits (6349 to
     5534); the thin hair still survives in 15 of 15 frames. One pair it
     cannot fix: a dodge where the body moves ~60 px one way and the hair
     another (20261029-181808-872, last frame) - no single layer motion
     fits it.
   - Other characters near the player (2026-10-29 night, captures
     20261029-1853*..1855*, a fast turn in a busy town square): the choice
     often took an NPC walking beside him, and the player's hair got no layer
     (a white smear of the floor through it for one frame). Now:
     - The player's head often comes as small pieces (the head top, 7-18
       cells, beside the rest), each losing to the NPC on size. A component
       is scored and drawn together with the small, clear pieces (at most 24
       cells, margin at least 0.1) within 2 empty cells of it that move
       alike (`layer_merge`; linking all minority cells that far apart ran
       down the body into the floor and lost the needle).
     - Off the middle across, a component crossing the screen faster than
       10 px a frame scores less, (10 / speed)^2 (`layer_still`,
       `layer_still_pow`, `layer_still_dx`: from 100 px off the middle, full
       at 250) - the camera follows the player, NPCs beside him walk at 8-25
       px 260-450 px off it. In the middle the faster part is as often the
       player's own head running ahead of the camera.
     - Farther than 250 px off the middle across a component scores less
       (`layer_hcentre`); not more than that - the orbit captures have the
       player 370 px off it.
     - A layer with no inner cells searched every offset at cost 1e9 and
       took the first, 16 px off both ways; it keeps its cells' mean now
       (`layer_refine_edge`). (20261029-185417-238 pair 1's 16 px was a
       valley along a horizontal edge, not a minimum; rejecting edge optima
       too, `layer_refine_edge 2`, changed nothing measurable.)
     The hair hole score on the 185* captures falls from 2166 to 1794 px (the
     rest is 185517 pair 2-3, where the player was already chosen);
     nothing else changes. Keeping the last pair's layer as a prior was
     tried and dropped: when the last pair had chosen the NPC it kept it.
      These three only while the dominant motion is at least 6 px a frame
      (`layer_select_dom`: the camera follows the player; in fights with a
      still camera they picked effects and pasted blocks into a fire ring
      and foliage across lightning), and only a slow component near the
      middle across may take pieces (`layer_merge_core`: a fast part off
      the middle pasted a doorway beside a shoulder and a square at a
      crossguard). Known costs: an NPC beside the player now often has no
      layer, so her hair top can go soft (185414, 185417), and an NPC's
      shadow beside the head gets a hard pale edge in 185345 pair 3-4.
   On the four running captures the thin top of the hair now survives in all
   15 measurable generated frames (5 of 15 before; `eval\needle_score.py` on
   `fg_eval --adjacent` output, head regions in `eval\head_rois.json`), at about +0.07 ms in a pan and +0.15-0.3 ms with a
   character.

Frames that do not just move:

- Fades and flashes: every comparison is made after correcting for how much
  the picture brightened or darkened - first estimated from the coarse
  pyramid, then, once A's motion is known, from content that actually
  matches (so a pan bringing in brighter scenery is not taken for a fade).
- Cuts: when more than a fifth of the frame in between matched nothing, or
  more than 40 % of its cells with detail did, the frame in between repeats A
  instead of mixing two scenes. (Flat areas match under any motion, even
  across a cut: a cut between two shots with much dark red sky in common left
  only 20.6 % of all cells unmatched, but 54 % of those with detail; the
  busiest real pair in the captures, a fast spear fight, reaches 34 %.)
- A window smaller than about 64 pixels gets the 50/50 blend; generation
  comes back at the next size.

About 2.2 ms on the RTX 5070 Ti at 3440x1440 (1.2 of it the per-pixel step,
0.5 the search both ways); the wrong-frame check beside slow things adds up
to about 0.3 ms during fast camera turns and nothing in a pan, the character
layer (step 6) about 0.07 ms in a pan and 0.15-0.3 ms with a character
(measured 2.9-3.3 ms per generated frame on a busy GPU, 2026-10-29). A cap on
the mix's softness where a both-frames option already fits (`cap_fit`,
`cap_cell`, off by default) thinned the faint second edge beside a still head
in fast orbits a little, but also wiped the faint ghost of a fast spear head
(20261025-000605-950) that a viewer reads as the spear - off.

## Measured (synthetic source through the real chain, 165 Hz display)

| Source | Window | Result after DLSS 5 warm-up |
| --- | --- | --- |
| 30 fps (as 60 with repeats) | 1280x720 | 29.9 real + 29.9 generated, 0 dropped, real frame within 0.011 ms avg |
| 30 fps (as 60 with repeats) | full screen 3440x1440 | 29.9 real + 29.9 generated, 0 dropped |
| 60 fps | full screen 3440x1440 | 60 real + ~58 generated, 0 dropped, ~2/s skipped (queue full) |

The first seconds after start drop frames with or without frame generation
(DLSS 5 starting up); the add-on waits them out and starts on its own, with
no back-off. With frame generation off, mpv drops the same frames there.
With `Method=motion` at full screen (3440x1440), 30 fps runs at 30 + 30 with
the real frame on time. 60 fps is at the edge of what the GPU can do next to
the DLSS 5 pass (about 9.5 ms of every 16.7): one run held 60 real + 55-58
generated with the real frame about 2.5 ms late, later runs dropped a few
source frames, and the add-on then backed off and generated almost nothing.
Screenshots with `DebugMarker=1` showed generated frames on screen, sharp,
without the blend's double image. All of this used a synthetic source through
`Glasir --input-raw`; the capture card has not been run with this
folder yet.

On screen with VRR (`vrr-test.ps1 -Steady`, PresentMon, 2026-10-30), how long
each frame stayed up:

| Source | Real frame timed from mpv's present (before) | Held until half an interval after the frame in between (now) |
| --- | --- | --- |
| 30 fps | generated 15.0 ms, real 18.3 ms (stdev 2.2) | generated 16.67 +- 0.39, real 16.67 +- 1.00 (stdev 0.76) |
| 60 fps | generated 5.2-5.4 ms, real 11.3-11.5 ms (stdev 3.1) | generated 8.34 +- 0.27, real 8.33 +- 0.47 (stdev 0.38) |

Before, the frame in between reached the screen about 7 ms after mpv's
present (the DLSS 5 pass and its drawing) while the real frame went out 8.3
ms after it: at 60 fps the two came almost together. Without frame
generation a 30 fps source stays up 33.33 +- 0.94 ms.

The hold can only space the frames evenly while the frame in between is done
well within half an interval; otherwise it lets the real frame go just before
the next frame's effects (counted as "let go early" in the log), so as not to
hold up its DLSS 5 pass. With NVIDIA's "Prefer maximum performance" for mpv
the frame in between took about 2 ms longer at 60 fps (GPU time per frame
10.2 ms instead of 8.2; 7.6 instead of 6.2 without frame generation), and
60 fps gave 6.2 / 10.5 ms; back at the default power setting it is 8.34 +-
0.41 / 8.33 +- 0.56 ms again (old timing 5.20 / 11.47). 30 fps is
16.66 / 16.68 either way. With DLSS 5's neural rendering on
(`NeuralUplift=1` in `[RenoDX.DLSS5]`) the DLSS 5 pass alone takes about 15.7
ms per frame: at 30 fps the frame in between is done about 16 ms after the
add-on drew it and the frames stay up 16.63 +- 0.63 / 16.70 +- 0.75 ms; at 60
fps there is no room for it and frame generation backs off by itself (60
real frames).

## Quality (offline, `eval\fg_eval.exe`)

PSNR of the frame in between against the true one, on clips made from game
screenshots (higher is better; "repeat" shows the earlier frame again):

| Clip | repeat | blend | motion |
| --- | --- | --- | --- |
| pan_slow (12 px/frame) | 22.6 | 25.2 | 60.9 |
| pan_med (40 px/frame) | 18.2 | 20.6 | 49.9 |
| pan_fast (96 px/frame) | 15.5 | 17.5 | 37.5 |
| pan_diag | 18.8 | 21.2 | 47.5 |
| zoom | 18.2 | 20.7 | 37.0 |
| sprite (object over moving background) | 17.6 | 20.3 | 35.4 |
| orbit (camera turning around a still character) | 17.3 | 19.7 | 33.6 |
| orbit_drift (the same, character drifting) | 16.7 | 19.3 | 29.6 |
| hud (still HUD over a pan) | 18.8 | 21.2 | 39.7 |
| noisy_hud (still HUD, grain on every frame) | 22.6 | 25.0 | 34.4 |
| subs (still subtitles over a pan) | 18.3 | 20.7 | 40.6 |
| ui_fade (HUD fading in over a pan) | 18.6 | 21.1 | 36.1 |
| ui_slide (menu panel sliding in) | 24.8 | 26.7 | 33.8 |
| menu_scroll (text) | 22.2 | 24.4 | 54.8 |
| pan_1440 (2560x1440) | 19.7 | 22.2 | 54.7 |
| fade_static (fade to black) | 36.3 | 57.1 | 57.1 |
| fade_pan (fade while panning) | 18.8 | 21.4 | 49.1 |
| cut (one pair across a hard cut) | 17.7 | 20.8 | 40.9 |
| **mean** | 20.2 | 23.6 | **42.9** |

`Quality=fast` (2x2 cells, one pixel compared) is cheaper and scores lower
(41.2 mean).

The scores above 45 dB are all invisible; the fade-outs at the cell edges
(step 5) and the softer mix where nothing fits cost a little there and gain on
the hard clips (sprite, subs, UI, orbits) and on game captures. Trying no
motion in the search (step 2) gains on still things over a pan (subs +2.0)
and on the game captures, and costs a little on the pans.

(At the cut the frame in between is A again; no true in-between frame exists
there, so that clip's score is mostly its three ordinary pairs.)

What is left shows beside the edges of moving things: in the strip one frame
cannot see (scenery a turning camera uncovers behind a character), the
motion is known per 16x16 pixels and the pixels drawn there are a little off.

## Captures

The capture key (F9) saves what frame generation worked with at that moment
into `Captures\<date-time>\` next to mpv.exe: five real frames in a
row as the DLSS 5 pass left them (`f01.png` - `f05.png`, 16-bit for HDR, 8-bit
for SDR - also when mpv draws SDR into a 10-bit back buffer, as it does on a
10-bit desktop, so 16-bit always means PQ to the viewer and the eval tools) and
the frames the add-on drew between them (`between_f01_f02.png`, ...). Press it
while something looks wrong (frame generation must be on); there is one short
hitch while it saves, and mpv's OSD says when the files are written (about
130 MB per capture).

`python src\framegen\eval\view_captures.py [FOLDER]` (needs Python with Pillow,
and ffmpeg on PATH) shows them: pick a capture on the left, then
Left/Right steps through f01, the generated frame, f02, ... tone-mapped like
`eval\compare.sh`. Zoom (mouse wheel, `1` = 100 %) and pan stay put while
stepping, so one spot can be compared frame to frame; Space plays the capture
in a loop (1x = 60 frames a second, as on screen, down to 1/30x); `R` skips
the generated frames for comparison; Up/Down changes capture; the status bar
gives the frame pixel under the cursor. Delete (or the Delete button) moves the
capture on show to the Recycle Bin and opens the next one, so a run of
captures can be pruned key by key; Shift+Delete deletes it for good, after
asking.

`eval\fg_eval.exe Captures` scores such folders like its own clips:
it makes the frame between f01 and f03 and compares it with f02 (and f03-f05
with f04) - twice the motion of real use, on the game's own pictures. On a
synthetic pan through the real chain: motion 38.2 dB, blend 23.0, repeat 20.6.
On sixteen captures of a game (2026-10-25/26: combat with effects, running,
camera turns and orbits around a standing or running character): motion
31.7 dB, blend 28.1, repeat 26.2. Side-by-side reviews of the frames the add-on draws in them (the frame
between each two in a row), the build before against the new one: for the
search around no motion and the cut check (44 pairs), 13 better - the faint
copies of hair and face beside a head during camera turns gone - 31 the
same, none worse except a small red dot on a tilting minimap fading for one
frame; for the persistence (all 64 pairs, 2026-10-26), 24 better - the
ground showing through the top of the head during camera turns gone, HUD
labels over moving scenery solid - 40 the same, none worse; the restored
spikes can be a little ragged at their tips; for the wrong-frame check
beside slow things (22 captures, 88 pairs, 2026-10-28), the faint extra hair
tips and second head outlines in fast orbits gone, also doubled NPCs,
dialogue lines and minimap edges; a version that also widened "still" to
anything much slower than the scenery tore holes in walls and a walking
NPC's dress, and was dropped. A spike that turns while the head drifts can
still come out soft or cut at its tip. Fast swings and limbs are as
soft as before, and the cut between two shots repeats the earlier frame
exactly. Blend comes close because PSNR favours blur where nothing
can be followed (an attack, lightning); there motion gives a blur-like smear
as well.

`eval\blockiness.py FRAME.png ...` measures how much more the picture jumps
across the 16-pixel cell edges than elsewhere (1.0 = no more). The game's own
frames in those captures: 1.08 mean, 1.22 worst. The frames the add-on drew
in the first eight, before step 5: 1.18 mean, 1.55 worst. All 64 pairs now
(`fg_eval.exe --adjacent`): 1.04 mean, 1.13 worst.

## Build and test

- `build.cmd` (repo root) builds the add-on with the rest of Glasir and
  installs it into `out\Glasir\engine`; the shaders are compiled with fxc
  into headers (`shaders.cmake` lists them). `Glasir.exe --setup` puts
  ReShade (add-on build, from reshade.me) next to mpv.exe.
- Settings: `glasir.ini` `[framegen]` (on/off, quality, every frame while
  off). Glasir writes them into `engine\ReShade.ini`, section
  `[GLASIR_FRAMEGEN]`, before mpv starts; the other keys there (DebugMarker,
  MaxOutputHz, ResetMs, ToggleKey, CaptureKey, Method, ...) can be edited by
  hand. The add-on logs to `engine\ReShade.log`.
- Without a card: `python tools\smoke_raw.py --seconds 12` runs the whole chain
  (ffmpeg test pattern -> `Glasir.exe --input-raw -` with dedup -> mpv ->
  ReShade -> the add-on); its 5 s stats lines are in ReShade.log. With
  `GLASIR_FRAMEGEN_TRACE=<file.csv>` the add-on writes one line per real
  frame (when it arrived, mpv's present, the frame in between done on the
  GPU and the real frame let go, in ms after the add-on ran).
  `GLASIR_FRAMEGEN_CAPTURE_AFTER=<s>` takes a capture, and
  `GLASIR_FRAMEGEN_TOGGLE_AFTER=<s>` presses F8 once, that many seconds in.
- `eval\fg_eval.exe` (built as `build\vs\src\framegen\Release\fg_eval.exe`)
  scores repeat, blend and motion with the same `fg_interp.h` the add-on runs:
  `fg_eval.exe [--clip NAME] [--out DIR] [--profile] [--stats] [--set NAME VALUE] SEQ_DIR`
  on the clips `eval\make_sequences.sh` makes from game screenshots, or on
  capture folders. The clips and captures are game pictures and stay out of
  the repository. (`--stats` prints each pair's brightness change and
  unmatched share; `--adjacent` makes the frames between each two in a row of
  a capture, as the add-on did, and saves them without a score; `--set NAME
  VALUE` changes one of `fg_interp.h`'s settings; `--set debug_view 1` colours
  each pixel by the option used: blue both frames, green B alone, red A alone,
  brighter for more motion; 2 shows the cost the softness follows, red where
  it may go wide; 3 how badly each cell's motion matched; 4 how far the motion
  found there goes - red A's blocks, green B's, blue the frame in between's,
  white 128 pixels and up; 5 the costs at each pixel, x2 - red standing still
  from both frames, green the best motion from both, blue the best from one
  frame alone.) A change to the shaders or `fg_interp.h` should keep or
  raise these scores; the port to Glasir gave the prototype's numbers
  exactly (synthetic mean 42.93 dB).
- `eval\compare.sh OUT.png W:H:X:Y SCALE A.png B.png ...` puts tone-mapped
  crops of HDR frames side by side; `eval\blockiness.py --diff A.png B.png`
  lists where two versions of a frame differ most; `eval\view_captures.py`
  shows captures (Tk, Pillow, ffmpeg on PATH); `eval\needle_score.py` and
  `eval\head_score.py` score thin hair and heads on captures (`FG_CAP`).
- `eval\raw_pace.exe` sends raw frames at an even 1/60 s like a card (ffmpeg
  `-re` jumps 4-43 ms between frames, and with VRR the screen shows that).
- Not ported yet: the prototype's VRR measurement under PresentMon
  (`vrr-test.ps1`, `vrr_report.py`). The pacing numbers below come from it.

## Limits of this version

- Motion is known per 16x16 pixels: edges of moving objects can show small
  errors where one frame hides what the other shows.
- What no motion search can follow (lightning, sparks, a very fast attack) is
  drawn as a soft smear, not as it would look half way.
- x2 only.
- At 60 fps full screen the GPU is near its limit next to the DLSS 5 pass (see
  Measured): earlier runs backed off; on 2026-10-30 synthetic runs held 60 + 60
  with no drops.
- Latency: the real frame is shown half a source frame after the frame in
  between, which waits for the DLSS 5 pass and its own drawing. Measured
  from mpv's present to the screen with VRR (2026-10-30): without frame
  generation 6.7 ms at 30 fps and 5.7 ms at 60; with it the frame in between
  shows at 8.5 / 7.5 ms (its drawing, +1.8) and the real frame at 25.2 / 15.9
  ms - 18.5 ms later at 30 fps, 10.2 ms at 60 (half a source frame plus the
  drawing).
- Even pacing needs VRR (G-SYNC) with mpv in the foreground; at a fixed 165 Hz
  frames land on whole refreshes (e.g. 6 and 12 ms at 60 fps).
- The hold needs Direct3D 11.4 fences and a Direct3D 12 device on the same GPU
  (ReShade wraps it like any other; the DLSS 5 add-ons logged nothing about
  it in the tests). Without
  them the real frame is timed from mpv's present as before (5 / 11.5 ms on
  screen at 60 fps).
- Every present counts as a real frame, including mpv redrawing the same video
  frame for its OSD; the frame between such a redraw and the next frame is the
  picture on screen anyway.
