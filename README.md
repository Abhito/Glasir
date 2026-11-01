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
> a working prototype. Nothing is ready to use yet. The plan for v1 is in
> [docs/PLAN.md](docs/PLAN.md).

## Requirements (planned for v1)

- Windows 11, 64-bit
- A capture card (any card that works with MPV-SW-Capture should work;
  no guarantees)
- Any DirectX 11 GPU for frame generation and upscaling; NVIDIA RTX 50
  series for DLSS 5

## Building

Needs Visual Studio 2026 (or 2022) with the "Desktop development with C++"
workload, and CMake 3.25 or newer.

```
build.cmd           # Visual Studio 2026
build.cmd vs2022    # Visual Studio 2022
```

This configures, builds, runs the tests and puts the portable app folder in
`out\Glasir`. Third-party programs (mpv, ReShade, DLSS 5 components) are not
in the repo; the installer downloads them.

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
