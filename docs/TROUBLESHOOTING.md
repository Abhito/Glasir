<!-- SPDX-License-Identifier: MIT -->
# Troubleshooting

If none of this helps, press **F9** while the problem is happening and attach
the zip from `Bug reports\` to an
[issue](https://github.com/Abhito/Glasir/issues) (see the README's
"Something wrong?").

## Frame generation does not double the frame rate

Open the menu (`Tab`). Its top line shows the rate, e.g. `60 → 120 fps` when
frame generation is working. Under "Frame generation" it says why it is not:

- **"Paused, back in N s: the GPU could not keep up"**: mpv dropped frames, so
  frame generation stepped back for a while (5 s, then longer if it keeps
  happening). DLSS 5 and frame generation share the GPU; at 60 → 120 fps each
  frame has about 8 ms. Choose **Fast** quality, use a lighter DLSS 5 setting,
  or capture at a lower resolution. Close other programs that use the GPU.
- **With "Made by: NVIDIA", Quality: Auto keeps changing**: it picks the
  largest of NVIDIA's models that keeps up, and picks again when the load
  changes. A change now and then is what it is for; `engine\ReShade.log` has
  a line for each ("VfgQuality=auto: ... now", with why). If the short stop
  at a change bothers you, choose Low, Medium or High yourself.
- **"Not doubling: the display shows at most N Hz"**: twice the game's rate
  would be more than the screen can show (60 fps needs a 120 Hz screen).
- The checkbox is greyed out: the card is used through DirectShow (cards
  Media Foundation cannot open), which has no frame generation.

In the NVIDIA Control Panel, leave "Power management mode" for mpv at its
default: "Prefer maximum performance" made each frame about 2 ms slower in
tests.

## VRR / G-SYNC

- In the NVIDIA Control Panel, G-SYNC must be on **for windowed and full
  screen mode** (Glasir's full screen and borderless modes are both windows
  that cover the screen; neither is exclusive full screen).
- G-SYNC follows the focused window: clicking a window on another screen
  turns VRR off for Glasir until you click back into it.
- Glasir's own full screen covers the whole screen, which keeps VRR. A
  window smaller than the screen (a window, or borderless with "Skip the
  black bars") keeps it only while Windows can give it a hardware overlay
  plane (MPO), which other running apps can take away; it is then composed
  by Windows, a frame later, and frame generation reaches only about 100 of
  120 fps. If the menu shows less than double, turn "Skip the black bars"
  off. With an NVIDIA GPU the menu says so itself, under the picture's More:
  "Windows is composing this window".
- **Is it on?** With an NVIDIA GPU, the menu's top line ends in `G-SYNC`
  while the driver runs it for Glasir's window.
- **NVIDIA's video super resolution (Scaling: VSR) and G-SYNC do not run
  together.** The NVIDIA driver turns G-SYNC off for a program while RTX
  Video Super Resolution works in it. Pick another scaling (FSR 1, ArtCNN,
  or NVIDIA Super Resolution, which is NVIDIA's again but run by Glasir)
  to have G-SYNC. When you switch away from VSR in the menu, Glasir starts
  the picture again (a second or two without it) because the driver would
  otherwise keep G-SYNC off until Glasir is closed.

## Black bars on an ultrawide screen

A 16:9 console on a 21:9 screen is shown as large as fits, with black bars
at the sides. DLSS 5 works on the whole screen, bars included (on 3440x1440
about 9.7 ms a frame instead of 5.5 for the picture alone); at 60 fps with
frame generation that can leave too little room, and frame generation then
pauses on its own. Neither the DLSS 5 add-on nor its feeder can be limited
to a part of the screen, but Glasir's window can: "Skip the black bars" in
Glasir's window (`[window] skip_black_bars = 1`) makes it the picture's
size, black around it (about 5.5 ms instead of 9.7). Check the fps in the
menu (Tab) afterwards: where Windows composes such a window, frame
generation falls short of double, and the option is better off.

## HDR

- HDR needs HDR on the console, HDR on in Windows (Settings, Display) and one
  of the card's 10-bit modes. Glasir's window lists the card's 10-bit modes
  when Automatic or HDR is chosen, and marks them with "HDR" in SDR.
- Cards often offer HDR only at some sizes and rates; the Live Gamer Ultra
  2.1, for example, has 4K HDR only at 30 fps but 1440p HDR at 60. Automatic
  uses those modes too: for an SDR console at 4K 60, choose SDR.
- With **Picture: Automatic** Glasir tells SDR from HDR by the picture and
  follows the console while you play (about a second for SDR, five for HDR;
  a black screen says nothing, so it waits for a picture). The picture
  changes in place, with a line at the top saying so. The menu's top line
  says what it is showing, e.g. `HDR (auto)`.
- **RTX HDR for SDR** (NVIDIA RTX, with Picture: HDR) needs HDR on in
  Windows and a driver with RTX Video HDR. Where it cannot start, a line at
  the top says so and the SDR picture is shown as it is; the menu's top line
  says `SDR (RTX HDR)` while it runs. If a game that is HDR looks glaring and
  wrong with it, Glasir took its signal for SDR: switch RTX HDR for SDR off
  (the menu, under the picture's More), and everything is shown as HDR.
- **The colours are wrong with Automatic** (washed out and dim: HDR shown as
  SDR; glaring: SDR shown as HDR): choose SDR or HDR yourself, in the menu or
  Glasir's window. It can happen with a console whose HDR peak is set to
  5000 nits or more (very bright scenes then read as SDR), and with a grey,
  dim SDR game that shows no white for five seconds (read as HDR).
  `logs\glasir.log` has a "signal:" line whenever what it sees changes; a
  bug report (F9) with it helps.

## The picture is soft or scaled

Glasir scales the card's picture to its place on the screen before DLSS 5.
A capture mode of your screen's height (2560x1440 on a 1440p screen) needs no
scaling; a 4K capture is scaled down anyway.

For a smaller capture, try "Scaling" in the menu: **FSR 1** gives sharper
edges on any GPU; **VSR** (NVIDIA RTX, Intel) uses the GPU's video engine
and cleans up compression too; **ArtCNN** rebuilds the most detail on any
GPU; **Anime4K** suits drawn and cel-shaded games. VSR, ArtCNN and Anime4K
cost GPU time, which DLSS 5 and frame generation also need; if frame
generation starts pausing, go back to FSR 1. If VSR cannot run (the driver
turns it down), Glasir says so and uses FSR 1. How strong NVIDIA's VSR is
(1 to 4, or Auto) is set in the NVIDIA app, under System, Video, Super
resolution, not in Glasir.

**NVIDIA Super Resolution** (NVIDIA RTX) is NVIDIA's super resolution run
by Glasir itself: its "Quality" (Low to Ultra) is picked in Glasir, and
G-SYNC stays on with it. It is a download of NVIDIA's, which Glasir's
window offers under Scaling.

- **The menu says it cannot run here**, or the top line says FSR 1: it is
  not downloaded (Glasir's window, Scaling, "Download it"), ReShade is not
  installed, or the GPU is not an NVIDIA RTX. `engine\ReShade.log` has a
  line saying which ("NVIDIA Video Super Resolution cannot scale the
  picture"). FSR 1 scales meanwhile.
- **Frame generation shows less than double with it, or pauses**: its
  models take GPU time. Pick a lower Quality, or FSR 1. With DLSS 5 on a
  60 fps game the GPU has little time left for it; a 30 fps game leaves
  more.
- **The picture stops for a moment** when it is switched on: Glasir's
  add-on loads NVIDIA's model (FSR 1 scales meanwhile), and does so again
  for another picture size or bit depth.

"Scale to" beside it picks the size the picture is scaled up to. A size
larger than your screen (4K on a 1440p screen) is scaled down to the screen
afterwards, which smooths edges: it helps FSR 1 the most, VSR a little, and
ArtCNN and Anime4K hardly at all. It costs GPU time too; "Screen" is the
lightest. mpv's own scaling does not use it. With NVIDIA Super Resolution
it is NVIDIA's model that makes the larger size.

## The capture card does not open

Another program may be using it: OBS, Discord, a browser tab, or the card's
own app. Close it and start Glasir again. `Glasir.exe --list` shows the cards
Glasir finds and their modes.

## No sound, or it is out of sync

- Glasir plays the card's own audio input; `logs\audio.log` says which one it
  found. `Ctrl+R` restarts the sound after changing the output device.
- `[` / `]` hold the sound back or bring it forward (10 ms a press), to match
  the picture; frame generation and DLSS 5 delay the picture a little.

## DLSS 5 is not available

DLSS 5 needs an NVIDIA RTX 30, 40 or 50 series GPU and its download: the
**Download DLSS 5** button in Glasir's window (**Download again** under More
options once it is there), or `Glasir.exe --setup`.

The first time Glasir plays after DLSS 5 is installed or updated, the
picture can stand still for about 20 seconds, and DLSS 5 shows up after
about 40: ReShade compiles DLSS 5's shader (with its motion estimator) and
keeps the result. It does so once for each picture size and for SDR and
HDR, so the first switch to the other one (also when the console's signal
changes while playing), or to another window size, waits the same way once.
Later starts and switches are quick again.

On RTX 30 and 40 series GPUs DLSS 5 takes more GPU time than on an RTX 50
(about 1.5 times as much on an RTX 4070 SUPER as on an RTX 5070 Ti). If the
picture stutters with it on, turn it off in the menu (Tab) or with the
launcher's checkbox; both keep it off. F6 turns it off until Glasir closes.

## Glasir closed by itself

Only one Glasir runs at a time: starting another one closes the first.
Otherwise `logs\glasir.log` says why it ended.
