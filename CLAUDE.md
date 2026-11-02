# Glasir - notes for coding agents

- What and why: see `docs/PLAN.md` (v1 scope, components, licences, porting
  order). Read it before adding features.
- Ported (v1 complete, not yet released) from the owner's working prototype in
  `C:\Users\Abhinav\Documents\dlss5\mpv_video\MPV-SW-Capture` (MSC +
  frame gen + capture + DLSS 5). That folder is the owner's daily app: read
  from it, never modify it.
- Build and test: `build.cmd` (CMake presets, VS 2026; `build.cmd vs2022`).
  Output: `out\Glasir`. `out\Glasir\Glasir.exe --setup --no-dialogs` downloads
  mpv, ReShade and DLSS 5 into `out\Glasir\engine` (`--components mpv,reshade,dlss5`).
  `build.cmd package` also makes the release zip in `out\`.
  Only the owner's PC has a capture card; say what was and wasn't tested
  with real hardware.
- End-to-end without a card: `python tools/smoke_raw.py [--format p010 --hdr]`
  (ffmpeg test pattern -> `Glasir.exe --input-raw -` -> mpv, checked over
  IPC). Scaling modes: `python tools/scaling_check.py [--format p010 --hdr]
  [--window 2560x1440 --size 1920x1080]` (switches each mode, checks the
  shaders / VSR filter, prints mpv's GPU time per pass). Screenshots (`s`,
  src/overlay/screenshot.h): `python tools/screenshot_check.py [--hdr]`
  (removes its files from `out\Glasir\Screenshots`, the owner's folder);
  the HDR tone mapping on game frames: `shot_convert.exe
  Captures\<when>\f01.png OUT` (build\vs\src\overlay\Release), compare with
  libplacebo. With the card, in a small window: `python tools/probe.py [--set ...]`;
  `--grab N` saves N decoded frames (HDR tone-mapped) to `%TEMP%\glasir_grabs\`
  so you can look at them (downscale before viewing: 2560x1440 PNGs).
  Both open a window for a few seconds. Glasir allows one instance; its IPC
  pipe is `\\.\pipe\glasir`.
- Frame generation: read docs/FRAMEGEN-LEARNINGS.md first (what was tried,
  dead ends, how to judge changes: across all captures, never one).
- Frame generation (`src/framegen`, MPL-2.0, design in its README.md): after a
  change to the shaders or `fg_interp.h`, compare `fg_eval.exe` scores before
  and after (clips/captures stay outside the repo; the prototype's are in
  `mpv_video\MPV-SW-Capture\framegen-addon\eval\seq` and `framegen-captures`).
  `GLASIR_FRAMEGEN_CAPTURE_AFTER=<s>` makes the add-on save frames to
  `out\Glasir\Captures` (16-bit PQ for HDR: tone-map before viewing).
- Glasir.exe opens its launcher window unless `--no-dialogs`, `--play` or
  `--input-raw` is given; the tools pass `--no-dialogs`. The in-game menu
  (Tab) logs "Glasir menu: ..." to `engine\ReShade.log`, with how many frames
  it was drawn on when closed. To look at a window without bringing it to the
  front (the owner may be using the PC), capture it with `PrintWindow`
  (flag 3) rather than copying the screen; sending keys needs the window in
  front, so check `GetForegroundWindow()` first.
  The launcher can be driven without focus by posting WM_KEYDOWN/WM_KEYUP
  (ImGui keyboard navigation: arrows, Space); posted mouse clicks are not
  reliable, as Windows sends WM_MOUSELEAVE when the real cursor is elsewhere.
- mpv's window (`src/app/backdrop.cpp`, Glasir's own thread): borderless
  full screen covers the whole monitor, black bars included, by default.
  `[window] skip_black_bars = 1` (launcher: "Skip the black bars") sizes
  mpv's top-level window to the picture with a black window of Glasir's
  behind it. A window smaller than the monitor (that, or a child window,
  `--wid`) keeps independent flip only while Windows gives it an overlay
  plane (MPO); on the owner's PC that stopped after a reboot (11-01, cause
  not found), it was composed and frame gen fell to ~100 of 120 fps, so it
  is off by default. The menu's "Show as" posts `script-message
  glasir-window borderless|windowed`; Glasir sets mpv's `border` over IPC and
  places the window (test it over IPC the same way). Its third choice, "Full
  screen", is mpv's own `fullscreen` (`f`, double-click; `window.mode =
  fullscreen`); leaving it, `glasir_window.lua` posts `glasir-window again`
  and Glasir puts the borderless window back (the picture's size with
  skip_black_bars). Neither is exclusive full screen (`d3d11-exclusive-fs`
  is off). Check presentation
  changes with PresentMon (`MPV-SW-Capture\tools\presentmon`).
- Updates and bug reports (`src/app/update.cpp`, `bug_report.cpp`): the repo
  is public and releases are real (v0.1.0 first); publishing one is the
  owner's call. Test with `GLASIR_UPDATE_TEST_RELEASE=<release.json>` (GitHub's
  answer from a file), `GLASIR_UPDATE_REPO=owner/name`,
  `Glasir.exe --update-from out\Glasir-x.y.z.zip`, or an older-version build
  running `--update` against GitHub, always on a copy of the app folder
  (never on out\Glasir while the owner uses it). F9 can be posted to mpv's
  window (class `mpv`) without focus; reports go to `Bug reports\`.
- Do not build in a folder under `%TEMP%`: MSBuild does not track headers
  there, so a changed `glasir_version.h` is not recompiled (the exe keeps the
  old version). Use a worktree elsewhere, e.g. next to the repo.
- `main` has a ruleset (no force pushes or deletion; others need a pull
  request); the repository admin bypasses it.
- Driving mpv in tests: mpv ignores key messages posted from another
  process (ReShade's input does see them: F9, Tab); use IPC `keypress KEY` to
  run mpv's own bindings. To see the picture with the overlay (toasts, menu),
  capture mpv's window with `PrintWindow` flag 3 (it may be covered); never
  copy the screen, which shows whatever the owner has open.
- Git Bash heredocs collapse `\\` to `\` (e.g. `\\.\pipe\...`): write
  scripts with the Write tool, not `cat <<EOF`.
- Licences: MIT everywhere except `src/framegen/` (MPL-2.0). Every source
  file starts with `SPDX-License-Identifier:`. Never commit third-party
  binaries, game captures, or third-party code with unclear licences
  (`third_party/README.md` lists what is vendored, with hashes).
- The product name and version are set only in the root `CMakeLists.txt`;
  code uses `glasir_version.h`.
- Must work on AMD/Intel GPUs too; NVIDIA-only features are optional and
  detected at runtime.
