# Glasir

**Your console, with PC graphics.**

[![Latest release](https://img.shields.io/github/v/release/Abhito/Glasir?label=download)](https://github.com/Abhito/Glasir/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Abhito/Glasir/total)](https://github.com/Abhito/Glasir/releases)
![Windows 11](https://img.shields.io/badge/Windows-11-0078D4)
[![Licence: MIT](https://img.shields.io/badge/licence-MIT-green)](LICENSE)

Plug your PS5, Xbox Series or Switch 2 into a capture card and play it on
your Windows PC, with frame generation, DLSS 5 and AI upscaling applied live.
No calibration and no tweaking: pick your card and press **Play**.

**[⬇ Download Glasir](https://github.com/Abhito/Glasir/releases/latest)**
for Windows 11 · [How to install](#-installing)

> Made for console-only games, like **GTA VI** at launch on PS5 and Xbox
> Series, or **The Legend of Zelda: Ocarina of Time** on Switch 2: play them
> on your PC screen, smoother and sharper than the capture card shows them.

---

## ✨ Features

- **30 fps games, smooth at 60.** Motion-compensated frame generation
  (30 → 60, 60 → 120 fps) on any GPU: NVIDIA, AMD or Intel.
- **HDR that looks right from the start.** SDR and HDR from the same app,
  switched in the menu, with nothing to calibrate.
- **DLSS 5 neural rendering** on NVIDIA RTX 50 series GPUs.
- **Sharper than the card sends it.** A 1080p capture scaled up to your
  screen by a neural network, not just stretched (see [Scaling](#scaling)).
- **Even frame pacing.** VRR / G-SYNC / FreeSync support, and repeated
  frames from the card are dropped, so a 30 fps game is shown at an even
  30 fps.
- **Low-latency sound** from the card (WASAPI), with boost and a lip-sync
  delay.
- **An in-game menu** (`Tab`), drawn after DLSS 5 and frame generation, so
  it stays sharp and quick.
- **Screenshots** (`s`) of the picture on screen, in HDR too (`.jxr`).
- **One-click updates**, and **bug reports** with one key (`F9`).

> [!NOTE]
> Frame generation makes motion smoother. It does not make the game react
> faster: the console still runs at its own rate, and the generated frames
> add a few milliseconds.

---

## 📋 Requirements

| | |
|---|---|
| **System** | Windows 11, 64-bit |
| **Capture card** | Any card Windows' own video capture can open gets every feature. Other cards run in a basic mode (no frame generation, no dropping of repeated frames), and Glasir tells you when that happens. |
| **GPU** | Any DirectX 11 GPU for frame generation and upscaling. NVIDIA RTX or Intel for video super resolution. NVIDIA RTX 50 series for DLSS 5. |
| **Screen** | A VRR screen (G-SYNC or FreeSync) is best for frame pacing, but not needed. |

We can't promise that every capture card works.

### Tested with

| | |
|---|---|
| **Consoles** | PS5 and Switch 2, in SDR and HDR |
| **Capture cards** | AVerMedia Live Gamer Ultra 2.1 (SDR and HDR)<br>StreamLine Mini+ GC311G2 (1080p60, SDR only: it has no 10-bit mode) |
| **GPU** | NVIDIA GeForce RTX 5070 Ti |
| **Screen** | 3440x1440 at 165 Hz, G-SYNC, HDR |

Not yet tested: AMD and Intel GPUs, other capture cards, other screens.
Reports from other setups are welcome (see
[Something wrong?](#-something-wrong)).

---

## 📦 Installing

1. **[Download the latest Glasir](https://github.com/Abhito/Glasir/releases/latest)**
   (`Glasir-x.y.z.zip`, under "Assets").
2. **Unzip it** into a folder you can write to, for example
   `C:\Games\Glasir`. Not Program Files: Glasir keeps its settings, logs
   and screenshots in its own folder.
3. **Start `Glasir.exe`.** The first time, it offers to download the
   programs it runs on, each from where its authors publish it (details
   below).
4. **Pick your capture card and press Play.**

To remove Glasir, delete its folder.

<details>
<summary>What Glasir downloads on first start</summary>

- [mpv](https://github.com/zhongfly/mpv-winbuild)
- [ReShade](https://reshade.me) with add-on support
- DLSS 5, on an NVIDIA RTX 50 series GPU only: DLSS5-Feeder, RenoDX-DLSS5
  and NVIDIA's neural rendering from
  [RHI's mirror](https://github.com/RankFTW/rhi-repo),
  [NVIDIA DLSS](https://github.com/NVIDIA/DLSS), LumeniteFX

Downloads are checked against their published SHA-256 hashes, and NVIDIA's
DLL against NVIDIA's signature. The **Update** button in Glasir's window
(or `Glasir.exe --setup`) downloads their newest versions later;
`engine\components.ini` says which are installed.

</details>

### Updating

When a newer Glasir is out, its window says so under **Play**:

- **Update Glasir** downloads it from
  [Glasir's releases](https://github.com/Abhito/Glasir/releases), checks it
  against the SHA-256 GitHub publishes for it, installs it and starts the
  new version.
- **What's new** opens the release's page.

Your settings (`glasir.ini`), screenshots, logs and downloaded programs stay
as they are. If anything goes wrong on the way, the version you have stays
as it was.

Glasir asks GitHub only when its window opens; `[update] check = 0` in
`glasir.ini` turns that off. `Glasir.exe --update` looks and installs from a
command line.

---

## 🎮 Using it

`Glasir.exe` opens its window first: pick the capture card and its mode,
SDR or HDR, DLSS 5, frame generation, the sound and the window mode, then
press **Play**.

- Untick "Show this window when Glasir starts" to play at once. Hold
  **Shift** while starting Glasir to see the window again, or start it with
  `Glasir.exe --play` to skip it.
- Everything is kept in `glasir.ini`, and every key is explained in the
  file. What you change while playing (volume, boost, audio delay, the menu)
  is saved there too.
- The card's own audio input is found automatically. DLSS 5 turns itself on
  with an NVIDIA RTX 50 series GPU (`[dlss5] enabled = auto`).

### Keys

| Key | What it does |
|---|---|
| `Tab` | The menu: DLSS 5, frame generation, scaling, sound, full screen, SDR/HDR (`Esc` or `Tab` closes it) |
| `f`, double-click | Full screen on / off |
| `Up` / `Down`, mouse wheel | Volume |
| `m` | Mute |
| `Ctrl+Up` / `Ctrl+Down` | Audio boost (up to 400 %) |
| `[` / `]` | Audio delay -/+ 10 ms (lip sync) |
| `Ctrl+r` | Restart the sound (after changing the output device) |
| `F6` | DLSS 5 on / off (this session) |
| `F8` | Frame generation on / off (this session; needs ReShade) |
| `F9` | Save a bug report, with the frames on screen |
| `s` | Screenshot |
| `Home` | ReShade's overlay |
| `q`, `Esc` | Quit (`Esc` closes the menu first when it is open) |

While the menu is open, the keyboard and mouse go to it. What the keys
change is shown at the top of the picture, drawn like the menu: sharp, and
without lag.

### Window modes

The menu's **Show as** switches at once and keeps the choice:

- **Borderless**: a window without a frame that fills the screen (the
  default).
- **Full screen**: mpv's own, what `f` or a double-click switches. Leaving
  it goes back to the borderless window.
- **Window**: a normal window.

Neither full screen is exclusive, but Windows flips both straight to the
display (Independent Flip), which is as fast.

On a screen of another shape than the picture (a 16:9 console on an
ultrawide) the picture is as large as fits, with black bars beside it.
DLSS 5 works on the bars too (on 3440x1440, about 9.7 ms a frame instead of
5.5 for the picture alone). **Skip the black bars** in Glasir's window
(`[window] skip_black_bars = 1`) makes the window the picture's size, so
DLSS 5 and frame generation skip the bars. It is off by default: on some
PCs Windows then composes the window, and frame generation falls short of
double (see [Troubleshooting](docs/TROUBLESHOOTING.md)).

### Scaling

A capture smaller than its place on the screen (1080p on a 1440p screen) is
scaled up before DLSS 5. Pick how under "Scaling", in Glasir's window or
the menu (it switches at once):

| Scaling | GPU | What it's like |
|---|---|---|
| mpv's own (Lanczos) | any | Plain, the lightest |
| **AMD FSR 1** | any | Sharper edges |
| **Video super resolution** (VSR) | NVIDIA RTX, Intel | The GPU's video engine; FSR 1 where it cannot run |
| **ArtCNN** | any | A small neural network that rebuilds fine detail; the sharpest, for some more GPU time |
| **Anime4K** | any | Made for drawn and cel-shaded styles (can look painted on realistic games); the most GPU time |

A capture mode of the screen's height (2560x1440 on a 1440p screen) needs no
scaling at all.

### Screenshots

`s` saves the picture as it is on screen to `Screenshots\`: after scaling
and DLSS 5, without the menu, the messages or the black bars.

- **SDR**: a PNG.
- **HDR**: a `.jxr` (Windows' own HDR picture format; Photos shows it in
  HDR) and a PNG made SDR for sharing.

Without ReShade, `s` saves the card's picture as it comes in.

### Tuning DLSS 5

DLSS 5 starts with its authors' default settings. To tune it, open
ReShade's overlay (`Home`): RenoDX-DLSS5 is on the Add-ons tab, DLSS5_Feed
on the Home tab (untick "Performance Mode" to edit it). Your changes stay
in `engine\ReShade.ini` and `engine\ReShadePreset.ini`; Glasir only switches
DLSS 5 on or off there.

---

## 🩺 Something wrong?

**[Troubleshooting](docs/TROUBLESHOOTING.md)** covers the usual questions:
frame generation not doubling, VRR, HDR, the black bars.

To report a problem:

1. Press **`F9`** while it is happening. Glasir saves a zip in
   `Bug reports\` with the logs, the settings, what is installed, your PC's
   GPUs, drivers and screens, and (with frame generation on) the frames on
   screen at that moment.
2. [Open an issue on GitHub](https://github.com/Abhito/Glasir/issues), say
   what you saw, and attach the zip. A report with frames is 10-100 MB, too
   big for an issue: share it through a file host instead.

**Save a bug report** in Glasir's window, or `Glasir.exe --bug-report`,
saves one without frames. Paths in it have your user folder replaced by
`%USERPROFILE%`, so your Windows user name is not in it.

Logs are in `logs\` (`glasir.log`, `audio.log`). ReShade, frame generation
and DLSS 5 log to `engine\ReShade.log` (and `engine\dlss5-feed.log`).

---

## 🛠 Building

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

<details>
<summary>Making a release</summary>

`build.cmd package` makes `out\Glasir-x.y.z.zip` (the version is set in the
root `CMakeLists.txt`). Publish it as an asset of a GitHub release tagged
`vx.y.z`; the update check finds the newest release's `Glasir-x.y.z.zip`
and GitHub's SHA-256 for it. `Glasir.exe --update-from Glasir-x.y.z.zip`
installs a zip from disk (to try one before publishing).

</details>

---

## 📄 Licence

- Glasir is under the [MIT licence](LICENSE).
- The frame generation code is under the [Mozilla Public License 2.0](LICENSES/MPL-2.0.txt):
  you can use it in any project, including closed-source ones, but changes
  to those files must stay public.
- Each source file says which one applies in its first line
  (`SPDX-License-Identifier:`).
- Third-party components and their licences:
  [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## 🙏 Credits

Glasir is based on [MPV-SW-Capture](https://github.com/TyRaS-SW/MPV-SW-Capture)
by TyRaS-SW (MIT). Its WASAPI audio plugin is by
[z-er](https://github.com/z-er/MPV-SW-Capture) (MIT). DLSS 5 support uses
[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) by jlrouzies-fr
and RenoDX-DLSS5. Built on [mpv](https://mpv.io) and
[ReShade](https://reshade.me) by crosire.

## 🏷 Trademarks

Glasir is not affiliated with or endorsed by NVIDIA, AMD, Intel, Sony,
Nintendo, Microsoft, Rockstar Games, Take-Two Interactive, or the authors of
the projects above. DLSS, G-SYNC and RTX are trademarks of NVIDIA
Corporation; Grand Theft Auto and GTA are trademarks of Take-Two
Interactive; The Legend of Zelda is a trademark of Nintendo; other names
belong to their owners.
