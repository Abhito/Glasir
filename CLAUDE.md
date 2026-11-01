# Glasir - notes for coding agents

- What and why: see `docs/PLAN.md` (v1 scope, components, licences, porting
  order). Read it before adding features.
- Being ported piece by piece from the owner's working prototype in
  `C:\Users\Abhinav\Documents\dlss5\mpv_video\MPV-SW-Capture` (MSC +
  frame gen + capture + DLSS 5). That folder is the owner's daily app: read
  from it, never modify it.
- Build and test: `build.cmd` (CMake presets, VS 2026; `build.cmd vs2022`).
  Output: `out\Glasir`; `tools\fetch-mpv.ps1` puts mpv in `out\Glasir\engine`.
  Only the owner's PC has a capture card; say what was and wasn't tested
  with real hardware.
- End-to-end without a card: `python tools/smoke_raw.py [--format p010 --hdr]`
  (ffmpeg test pattern -> `Glasir.exe --input-raw -` -> mpv, checked over
  IPC). With the card, in a small window: `python tools/probe.py [--set ...]`;
  `--grab N` saves N decoded frames (HDR tone-mapped) to `%TEMP%\glasir_grabs\`
  so you can look at them (downscale before viewing: 2560x1440 PNGs).
  Both open a window for a few seconds. Glasir allows one instance; its IPC
  pipe is `\\.\pipe\glasir`.
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
