# Glasir

Modern PC graphics for game consoles. Glasir shows your console (PS5,
Switch 2, Xbox Series, ...) through a capture card on your Windows PC, with low
latency and modern enhancements applied live:

- **Frame generation** (30 → 60, 60 → 120 fps), motion-compensated, any GPU
- **VRR / G-SYNC / FreeSync** pacing
- **DLSS 5 neural rendering** (NVIDIA RTX 50 series)
- **Upscaling** (FSR1 on any GPU, RTX Video Super Resolution on NVIDIA)
- **HDR and SDR**, low-latency WASAPI audio with boost and delay

> **Status: early development.** Glasir is being ported piece by piece from
> a working prototype. The plan for v1 is in [docs/PLAN.md](docs/PLAN.md).

## Requirements (planned for v1)

- Windows 11, 64-bit
- A capture card (any card that works with MPV-SW-Capture should work;
  no guarantees)
- Any DirectX 11 GPU for frame generation and upscaling; NVIDIA RTX 50
  series for DLSS 5

## Installing

1. Unzip `Glasir-x.y.z.zip` into a folder you can write to (for example
   `C:\Games\Glasir`; not Program Files, because Glasir keeps its settings,
   logs and screenshots in its own folder).
2. Start `Glasir.exe`. The first time, it asks to download the programs it
   runs on, each from where its authors publish it:
   [mpv](https://github.com/zhongfly/mpv-winbuild),
   [ReShade](https://reshade.me) with add-on support, and DLSS 5 on an
   NVIDIA RTX 50 series GPU (DLSS5-Feeder, RenoDX-DLSS5 and NVIDIA's neural
   rendering from [RHI's mirror](https://github.com/RankFTW/rhi-repo),
   [NVIDIA DLSS](https://github.com/NVIDIA/DLSS), LumeniteFX). Downloads are
   checked against their published SHA-256 hashes, and NVIDIA's DLL against
   NVIDIA's signature.
3. Later, `Glasir.exe --setup` downloads the newest versions.
   `engine\components.ini` says which are installed.

To remove Glasir, delete its folder.

## Using it

`glasir.ini` picks the card, size, rate, SDR or HDR, dedup, frame
generation, DLSS 5, sound and the window mode; every key is explained in the
file. The card's own audio input is found automatically. DLSS 5 turns itself
on with an NVIDIA RTX 50 series GPU (`[dlss5] enabled = auto`). Volume, boost
and audio delay you change while playing are kept in `glasir.ini`, and so is
what you change in the menu.

The menu (`Tab`) is drawn after DLSS 5 and frame generation, on every frame
that reaches the screen, so it stays sharp and answers at once. Keys and the
mouse go to it while it is open.

| Key | |
|---|---|
| `Tab` | the menu: DLSS 5, frame generation, sound, full screen, SDR/HDR (`Esc` or `Tab` closes it) |
| double-click, `f` | full screen |
| `Up` / `Down`, mouse wheel | volume |
| `m` | mute |
| `Ctrl+Up` / `Ctrl+Down` | audio boost (up to 400 %) |
| `[` / `]` | audio delay -/+ 10 ms (lip sync) |
| `Ctrl+r` | restart the sound (after changing the output device) |
| `F6` | DLSS 5 neural rendering on/off (this session) |
| `F8` | frame generation on/off (this session; needs ReShade) |
| `F9` | save the frames around now for a bug report (in `Captures\`) |
| `s` | screenshot (in `Screenshots\`) |
| `q` | quit |
| `Home` | ReShade's overlay |

DLSS 5 starts with its authors' default settings; tuning it is up to you, in
ReShade's overlay (`Home`): RenoDX-DLSS5 on the Add-ons tab, DLSS5_Feed on
the Home tab (untick "Performance Mode" to edit it). Your changes stay in
`engine\ReShade.ini` and `engine\ReShadePreset.ini`; Glasir only switches
DLSS 5 on or off there.

Logs are in `logs\` (`glasir.log`, `audio.log`); ReShade, frame
generation and DLSS 5 log to `engine\ReShade.log` (and `engine\dlss5-feed.log`).

## Building

Needs Visual Studio 2026 (or 2022) with the "Desktop development with C++"
workload, and CMake 3.25 or newer.

```
build.cmd                # Visual Studio 2026
build.cmd vs2022         # Visual Studio 2022
build.cmd package        # ... and the release zip, out\Glasir-x.y.z.zip
```

This configures, builds, runs the tests and puts the portable app folder in
`out\Glasir`. Third-party programs are not in the repo:

```
out\Glasir\Glasir.exe --setup --no-dialogs   # mpv, ReShade (and DLSS 5 on RTX 50) into out\Glasir\engine
out\Glasir\Glasir.exe --list                 # the capture cards and their modes
```

`--setup --no-dialogs --components mpv,reshade,dlss5` picks what to download.

Testing without a capture card (needs ffmpeg and Python):

```
python tools\smoke_raw.py                    # NV12, a 30 fps game captured at 60
python tools\smoke_raw.py --format p010 --hdr
python tools\probe.py --set video.mode=hdr   # with a real card, in a small window
```

## Licence

- Glasir is under the [MIT licence](LICENSE).
- The frame generation code is under the [Mozilla Public License 2.0](LICENSES/MPL-2.0.txt):
  you can use it in any project, including closed-source ones, but changes
  to those files must stay public.
- Each source file says which one applies in its first line
  (`SPDX-License-Identifier:`).
- Third-party components and their licences:
  [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Credits

Glasir is based on [MPV-SW-Capture](https://github.com/TyRaS-SW/MPV-SW-Capture)
by TyRaS-SW (MIT). Its WASAPI audio plugin is by
[z-er](https://github.com/z-er/MPV-SW-Capture) (MIT). DLSS 5 support uses
[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) by jlrouzies-fr
and RenoDX-DLSS5. Built on [mpv](https://mpv.io) and
[ReShade](https://reshade.me) by crosire.

Glasir is not affiliated with or endorsed by NVIDIA, AMD, Sony, Nintendo,
Microsoft, or the authors of the projects above. DLSS, G-SYNC and RTX are
trademarks of NVIDIA Corporation; other names belong to their owners.
