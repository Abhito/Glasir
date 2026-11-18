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
- 3x and 4x frame generation (`Multiplier`, glasir.ini `[framegen]
  multiplier`, "Rate" in the launcher and the menu) are provided as is, not
  recommended and never the default: say so wherever the docs describe them
  (README, glasir.ini's comments, src/framegen/README.md). Not in the "Rate"
  tooltips: the owner had it taken out of them (11-17).
  `fg_eval --multiplier 3|4` scores them on clips made with `STEPS=3|4
  make_sequences.sh` (FG_SEQ, FG_SRC: folders outside the repo).
- Frame generation (`src/framegen`, MPL-2.0, design in its README.md): after a
  change to the shaders or `fg_interp.h`, compare `fg_eval.exe` scores before
  and after (clips/captures stay outside the repo; the prototype's are in
  `mpv_video\MPV-SW-Capture\framegen-addon\eval\seq` and `framegen-captures`).
  The prototype's captures are 3440 wide with the picture at x = 440-3000:
  `--picture 440 0 3000 1440` scores what the add-on draws there
  (`PictureOnly`). `fg_eval`'s defaults are the add-on's (edge rules, HUD
  hold); `--set edge_cost -1 --set edge_explains 1 --set hud_detail 0` is
  without them.
  WarpPS is so large that code added to it costs time even when never run
  (1.4 ms a frame once): put new work in a pass of its own, and time
  `fg_eval --profile` against the build before with the new thing off, the
  builds in turn. `eval\hud_score.py` scores a HUD over moving scenery. A
  few frames differ between two runs of the same build (the layer passes):
  run twice before calling a frame changed.
  Its switches (`PictureOnly`, `EdgeRules`, `StillHud`: on by default;
  `HistorySearch`: a test, off) are in ReShade.ini only while one differs
  from its default (`switch_setting`), so a default can still be changed
  later: never write them at start as the other keys are.
  They are also checkboxes on ReShade's Add-ons page
  (`reshade_ui.cpp`, README.md there). To drive ReShade's overlay in a test
  without focus: post Home to mpv's window, then arrow keys, Enter (into an
  add-on's box), Space, Esc; Tab also opens Glasir's menu. Its mouse follows
  the real cursor whatever is posted. It opens on the page it was closed on:
  `Selected=0x1BEC192E` in ReShade.ini's `[OVERLAY] Docking` is Add-ons.
  `GLASIR_FRAMEGEN_CAPTURE_AFTER=<s>` makes the add-on save frames to
  `out\Glasir\Captures` (16-bit PQ for HDR: tone-map before viewing).
- NVIDIA Video Frame Generation (`Method=vfg`, `src/framegen/fg_vfg.h`, its
  section in src/framegen/README.md): an opt-in generator on RTX 40/50,
  glasir.ini `[framegen] method = nvidia`, `nvidia_quality`. Its DLLs are
  NVIDIA's and never go into the repo or the zip: setup downloads them to
  `engine\nvidia-vfg` only after NVIDIA's licences are accepted (the
  launcher's dialog, or `--setup --components vfg --no-dialogs
  --accept-nvidia-licence`, on a copy of the app folder).
  `GLASIR_VFG_RUNTIME=<folder>` uses DLLs from elsewhere. Test with
  `fg_eval --methods repeat,vfg` (must give `eval\vfg_eval.py`'s scores) and
  `tools/smoke_raw.py` on a copy whose ReShade.ini says `Method=vfg`
  (`--window 2560x1440` for a larger window); `GLASIR_VFG_PROFILE=1` logs
  the calling thread's time per frame. NVIDIA's licence (Software License
  Agreement 8.9) forbids giving benchmark, comparison or performance results
  of its software to others: never put this method's scores or timings in
  the repo, commit messages, issues, pull requests or logs users send in.
  Tell the owner in chat only.
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
  Find a window to post to by its class and its title (the launcher:
  `GlasirLauncher`; setup's dialogs: `#32770` titled "Glasir setup"): other
  programs keep hidden windows of the plain dialog class, and a search by
  class alone finds theirs. A dialog left open is on the owner's screen and
  may get clicked: close it (WM_COMMAND with IDCANCEL) as soon as it is seen.
- The look of the launcher and the menu (`src/common/ui_style.h`,
  `ui_widgets.h`: cards, switches, rows of choices, sliders, "More
  options"; redesigned 2026-11-17 because the owner found them dated and
  cluttered). Keep them short: what is changed for a session is in sight
  (card, mode, SDR/HDR, DLSS 5, frame generation, volume), what is set once
  goes behind "More options" in the launcher and behind each card's own
  "More" in the menu (boost and audio delay with the sound, scaling and the
  window with the picture), an explanation goes into a tooltip
  (`ui::tooltip`, also shown for the keyboard's item), and a note stays on
  screen only where it says something to do. A new setting gets one of the
  controls there, not a row of radio buttons or a paragraph. Before posting
  Space or Enter in a test, capture the window and look where the keyboard's
  ring is: on the launcher's Play it starts playing from the card.
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
- Downloaded programs are pinned (`src/app/components.h`): setup installs
  the versions in `src/app/components.json`, built into Glasir.exe, each
  checked against its SHA-256. A pinned download
  that is gone (HTTP 404; zhongfly keeps mpv builds about a month) falls
  back to the newest. `--setup --newest` takes the newest of everything;
  `python tools/pin_components.py --engine DIR` writes components.json from
  that folder's components.ini. `GLASIR_COMPONENTS=<file>` uses another
  components.json without building. Test setup on a copy of the app folder
  (e.g. `cmake --install build\vs --config Release --prefix build\x\Glasir`),
  not on out\Glasir. Judge a new DLSS 5 part by RenoDX's last
  `NR-VERDICT` in engine\ReShade.log (`state=ENGAGED`, its eval ratio), the
  Feeder's `effects:` and `600 frames` lines in dlss5-feed.log and frame
  generation's `real + generated fps` lines, on the card and with
  `tools/screenshot_check.py --dlss5` (SDR and `--hdr`). Run each test twice:
  ReShade compiles a changed shader once per picture size and SDR/HDR
  (DLSS5-Feeder 2.0 with its built-in motion vectors: ~17 s, twice, and the first one
  stops the picture), so a
  first short run can end before DLSS 5 engages.
- SDR or HDR (`video.mode = auto`, the default; `src/capture/signal_kind.h`):
  told from the 10-bit capture's top luma and chroma. `GLASIR_SIGNAL_LOG=1`
  logs what it measures every 2 s ("signal:" lines in glasir.log). Its
  limits come from measured signals (the header lists them): change them
  only with new measurements from a real console in both modes. The tools
  pass `video.mode` themselves (and `video.rtx_hdr=0` with `--hdr`: their
  test pattern looks like SDR).
- A 10-bit capture is switched between SDR and HDR while playing, never by
  starting mpv again (`picture_commands` in `src/app/mpv_launch.cpp`, the
  loop in `main.cpp`'s `run`): the labelled tag filter `@glasir-colour` is
  replaced, `script-message glasir-signal hdr|sdr|rtx` tells
  `glasir_scaling.lua` (its filters depend on the tags), and
  `target-colorspace-hint yes|no` switches mpv's swap chain. Never set
  `d3d11-output-csp` while playing: mpv builds its output again, ReShade and
  all (about 9 s). The menu's Picture sends `script-message glasir-picture
  auto|sdr|hdr`; only an 8-bit capture (`GLASIR_PICTURE_LIVE=0`) still quits
  mpv with code 42 to have the card opened in another mode. `video.rtx_hdr`
  ("RTX HDR for SDR", NVIDIA RTX, Picture = HDR): the signal is watched
  there too and SDR goes through mpv's `d3d11vpp=nvidia-true-hdr`, in the
  same filter as VSR when both run; off, everything is HDR whatever the
  picture looks like. Test with `python tools/picture_check.py [--mode hdr]
  [--no-rtx-hdr] [--menu]` (a P010 pattern that looks like SDR, then HDR,
  then SDR; about half a minute in a window), with DLSS 5 on too
  (`--set dlss5.enabled=on --each 25`).
- Updates and bug reports (`src/app/update.cpp`, `bug_report.cpp`): the repo
  is public and releases are real (v0.1.0 first); publishing one is the
  owner's call. Test with `GLASIR_UPDATE_TEST_RELEASE=<release.json>` (GitHub's
  answer from a file), `GLASIR_UPDATE_REPO=owner/name`,
  `Glasir.exe --update-from out\Glasir-x.y.z.zip`, or an older-version build
  running `--update` against GitHub, always on a copy of the app folder
  (never on out\Glasir while the owner uses it). F9 can be posted to mpv's
  window (class `mpv`) without focus; reports go to `Bug reports\`. The zip
  holds JPEG pictures of the F9 frames (`src/app/frame_preview.h`, under
  GitHub's 25 MB for an attachment); the frames themselves stay in
  `Captures\`.
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
