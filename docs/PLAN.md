# Project plan (v1)

Name: Glasir.
Decisions made 2026-10-31. This file is the reference for what v1 is and is not.

Status (2026-11-01): all eight porting steps below are done; v1 is complete
and not yet released.

## Goal

Let people play modern consoles (PS5, Switch 2, Xbox Series, ...) through a
capture card on a Windows PC with modern graphics enhancements: DLSS 5,
frame generation, upscaling, HDR, VRR, with as little latency as possible.

Base: MPV-SW-Capture by TyRaS-SW (MIT). New repo, ported piece by piece, with
credit. MPV-SW-Capture stays the owner's daily app until this one replaces it.

## Platform

- Windows 11 only (for now).
- Engine: mpv + ReShade add-ons (kept for v1; a native renderer may come later).
- One GPU. Second-GPU offload (frame gen / depth on another GPU) is a later add-on.
- All GPU vendors: frame generation, VRR pacing, dedup and upscaling must work
  on AMD and Intel too. NVIDIA-only features turn on only when detected:
  - DLSS 5: NVIDIA RTX 50 series minimum (RTX 30/40 unlock mods maybe later).
  - RTX Video Super Resolution.
- Capture cards: best effort, no guarantees. Goal: any card MPV-SW-Capture
  works with. Our Media Foundation capture (dedup, frame gen) first, with
  mpv's DirectShow input as a fallback; the app says which features the
  fallback loses.

## v1 features

Keep / build:

- Capture: card, resolution and fps picker.
- Audio: low-latency WASAPI (z-er's msc_audio, built from source), volume,
  mute, audio boost, audio delay (lip-sync with DLSS 5 / frame gen latency),
  volume sidebar inside the new menu.
- SDR and HDR from one launcher (switch in the menu / per-console presets;
  restarts the pipeline). Auto-detecting HDR from the picture is a later experiment.
- Dedup, frame generation x2 with VRR hold pacing, low-latency "send repeats"
  switch (frame gen off).
- DLSS 5 with neural rendering on/off.
- Upscaling: mpv's built-in scaling + FSR1 (any GPU); RTX VSR on NVIDIA.
  DLSS / FSR upscaling later, enabled by compatibility. *(done: [video]
  scaling = mpv | fsr | vsr, glasir_scaling.lua + Glasir's own FSR 1 port;
  launcher and menu)*
- Screenshots; window / full-screen controls (the menu switches at once).
  Full screen covers the whole screen, black bars included, unless
  `window.skip_black_bars` (off by default: a boxed window loses independent
  flip where Windows gives it no overlay plane, see FRAMEGEN-LEARNINGS.md).
- Our own update check with one-click self-update (GitHub releases).
- Hidden "save bug report" (the F9 frame-gen captures + capture viewer).

UI:

- A separate launcher window (normal Windows app, not touched by ReShade) for
  card, preset, SDR/HDR and feature choices.
- A small in-game menu drawn by our add-on AFTER DLSS 5 and frame generation,
  on every presented frame, so it stays responsive (the old mpv OSD menu goes
  through DLSS 5 and frame gen and lags).
- One settings file; the app writes ReShade.ini / mpv.conf / input.conf /
  dlss5-feed.cfg from it.
- English only for v1, with all UI text in one place for later translations.

Dropped: CRT / retro shaders, shapes, bezels, NSO crops, rotation, mirror,
old FSR1/NVScaler shader presets of MSC, TyRaS's version check, recording
(users record the window with OBS), FFplay audio mode, LumeniteFX extras
beyond what DLSS5_Feed needs.

## Third-party components

Nothing third-party that is restricted goes into the repo or our release zip.
Glasir's setup (in Glasir.exe) fetches on the user's PC:

| Component | Source |
|---|---|
| mpv (GPLv3 build) | zhongfly/mpv-winbuild releases |
| ReShade (add-on build) | reshade.me |
| DLSS5-Feeder (MIT) | jlrouzies-fr/DLSS5-Feeder releases |
| LumeniteFX (motion for DLSS5_Feed; AGNYA licence) | umar-afzaal/LumeniteFX (ask the author before public release) |
| RenoDX-DLSS5, nvngx_dlssnr.dll | RankFTW/rhi-repo releases (RHI's mirror) |
| nvngx_dlss.dll | NVIDIA/DLSS releases |

Shipped with notices: our code, MSC scripts we keep (MIT, TyRaS-SW),
msc_audio (MIT, z-er), FSR1 / NIS / Adaptive Sharpen / KrigBilateral (LGPL,
own file) shaders, ReShade API headers (BSD-3), ReShade.fxh (CC0).

## Licences

- MIT for the project (`LICENSE`, `LICENSES/MIT.txt`), copyright Abhito.
- MPL-2.0 for the frame-generation files (`LICENSES/MPL-2.0.txt`): flow.hlsl,
  blend.hlsl, fg_interp.h, msc_framegen.cpp (incl. VRR hold pacing), eval tools.
  The shared link header (msc_framegen_link.h) is MIT. No Exhibit B.
- Every source file starts with an `SPDX-License-Identifier:` line.
- Contributors sign off commits (DCO).

## Repo and builds

- In: source, build scripts, eval tools, docs (README + docs/ or wiki).
- Out: game captures (copyrighted; eval points at a local folder), downloaded
  binaries.
- Local git -> private GitHub repo once named -> public later.
- Builds on the owner's PC first, GitHub Actions later. Real-card testing is
  only possible on the owner's PC.

## Porting order

Each step tested against the real capture card before the next:

1. Repo skeleton, build scripts, licences/notices. *(done)*
2. Capture (msc_hdr_capture: MF NV12/P010, dedup) + mpv launch, SDR and HDR,
   DirectShow fallback. *(done: Glasir.exe replaces the two .bat launchers;
   also YUY2/UYVY cards, automatic card and mode choice, glasir.ini)*
3. Audio (msc_audio from z-er's source) + boost + delay. *(done: glasir_audio.dll,
   based on z-er's public source; finds the card's audio input by itself)*
4. Frame generation add-on + VRR hold + send-repeats. *(done: src/framegen, MPL-2.0;
   same shaders and scores as the prototype; VRR measurement tools not ported yet)*
5. DLSS 5 (Feeder + RenoDX-DLSS5) wiring. *(done: download, [dlss5]
   auto/on/off with RTX 50 detection; nvngx_dlss.dll from NVIDIA's own SDK works)*
6. Installer (component downloads) + settings file. *(done: Glasir.exe is its own
   installer: release zip, first start asks and downloads mpv, ReShade and DLSS 5
   (`--setup` to update); volume, boost and delay changed while playing are kept
   in glasir.ini. mpv's options go on its command line from glasir.ini; mpv.conf
   and input.conf are Glasir's own files. F6/F8 stay session toggles; DLSS 5
   tuning in the ReShade overlay stays in ReShade.ini, starting from its
   authors' defaults)*
7. Launcher + in-game menu (after-effects overlay prototype first). *(done: the menu
   (src/overlay, Dear ImGui) is drawn by the frame generation add-on on every
   presented frame, after DLSS 5 and frame generation, and built again for each;
   it saves to glasir.ini, switches SDR/HDR by restarting the picture (mpv exit
   code 42). The launcher is Glasir.exe's own window (Dear ImGui on D3D11),
   [launcher] show / Shift / --play. The key-press OSD texts (volume,
   boost, F8...) go through mpv's OSD, so through DLSS 5 and frame generation;
   they could move into the overlay)* *(done: toasts drawn by the overlay;
   input.conf uses no-osd, everything else sends script-message glasir-toast)*
8. Update check / self-update, bug-report capture, docs. *(done: update
   check in the launcher + `--update`, self-update from the release zip with
   SHA-256 and rollback; F9 / launcher / `--bug-report` save a zip with logs,
   settings, system and F9 frames; README sections + docs/TROUBLESHOOTING.md)*
