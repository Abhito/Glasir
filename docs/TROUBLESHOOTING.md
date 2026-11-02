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
- **"Not doubling: the display shows at most N Hz"**: twice the game's rate
  would be more than the screen can show (60 fps needs a 120 Hz screen).
- The checkbox is greyed out: the card is used through DirectShow (cards
  Media Foundation cannot open), which has no frame generation.

In the NVIDIA Control Panel, leave "Power management mode" for mpv at its
default: "Prefer maximum performance" made each frame about 2 ms slower in
tests.

## VRR / G-SYNC

- In the NVIDIA Control Panel, G-SYNC must be on **for windowed and full
  screen mode** (Glasir's full screen is a borderless window).
- G-SYNC follows the focused window: clicking a window on another screen
  turns VRR off for Glasir until you click back into it.
- Glasir's own full screen covers the whole screen, which keeps VRR. A
  window smaller than the screen (a window, or full screen with "Skip the
  black bars") keeps it only while Windows can give it a hardware overlay
  plane (MPO), which other running apps can take away; it is then composed
  by Windows, a frame later, and frame generation reaches only about 100 of
  120 fps. If the menu shows less than double, turn "Skip the black bars"
  off.

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
  of the card's 10-bit modes. Glasir's window lists the card's HDR modes when
  HDR is chosen, and marks them with "HDR" in SDR.
- Cards often offer HDR only at some sizes and rates; the Live Gamer Ultra
  2.1, for example, has 4K HDR only at 30 fps but 1440p HDR at 60.
- Switch between SDR and HDR in the menu; the picture starts again.

## The picture is soft or scaled

Glasir scales the card's picture to its place on the screen before DLSS 5.
A capture mode of your screen's height (2560x1440 on a 1440p screen) needs no
scaling; a 4K capture is scaled down anyway.

For a smaller capture, try "Scaling" in the menu: **FSR 1** gives sharper
edges on any GPU; **RTX VSR** (NVIDIA RTX) uses the GPU's video engine and
cleans up compression too. RTX VSR costs some GPU time, which DLSS 5 and
frame generation also need; if frame generation starts pausing, go back to
FSR 1. If RTX VSR cannot run, Glasir says so and uses FSR 1.

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

DLSS 5 needs an NVIDIA RTX 50 series GPU and its download: the **Update**
button in Glasir's window, or `Glasir.exe --setup`.

## Glasir closed by itself

Only one Glasir runs at a time: starting another one closes the first.
Otherwise `logs\glasir.log` says why it ended.
