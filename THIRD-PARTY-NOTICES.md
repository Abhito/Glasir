# Third-party notices

Glasir is MIT (see `LICENSE`), with the frame generation files under the
Mozilla Public License 2.0 (`LICENSES/MPL-2.0.txt`). It includes or uses the
third-party components below. Each keeps its own licence.

## Included in Glasir

### ReShade add-on API headers

Used to build Glasir's ReShade add-ons. Source: https://github.com/crosire/reshade
(tag v6.8.0). Not an endorsement of Glasir by ReShade or its author.

```
Copyright 2014 Patrick Mours. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
  * Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### mpv client API header

Used to build Glasir's mpv plugin. Source: https://github.com/mpv-player/mpv
(tag v0.41.0, `include/mpv/client.h`).

```
Copyright (C) 2017 the mpv developers

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

### WASAPI audio plugin (z-er, MPV-SW-Capture)

Glasir's audio plugin (`src/audio`, `glasir_audio.dll`) is based on the
WASAPI audio plugin by z-er (Josh S) for MPV-SW-Capture:
https://github.com/z-er/MPV-SW-Capture (`native-audio/`). Modified for Glasir.

```
MIT License

Copyright (c) 2026 TyRaS-SW

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

<!--
Added as each part is ported (docs/PLAN.md):
- MPV-SW-Capture by TyRaS-SW (MIT) - scripts we keep, marked as modified.
- AMD FSR1 (MIT), NVIDIA Image Scaling (MIT), Adaptive Sharpen (BSD-2-Clause),
  KrigBilateral (LGPL-3.0, own file + LICENSES/LGPL-3.0.txt).
- DLSS5-Feeder (MIT, Jean-Laurent Rouzies; portions NIGos, MIT) if shipped.
-->

## Downloaded by the installer, not included

The installer downloads these from their official sources on your PC. Glasir
does not distribute them; their licences apply as published by their authors.

| Component | Source | Licence |
|---|---|---|
| mpv (Windows build) | https://github.com/zhongfly/mpv-winbuild | GPL-3.0 (mpv: GPL-2.0-or-later, built with GPL-3.0 FFmpeg) |
| ReShade with add-on support | https://reshade.me | BSD-3-Clause source; binary from reshade.me |
| DLSS5-Feeder | https://github.com/jlrouzies-fr/DLSS5-Feeder | MIT |
| LumeniteFX by Kaidō (Afzaal) | https://github.com/umar-afzaal/LumeniteFX | AGNYA License (its LICENSE.md is placed beside it) |
| ReShade.fxh | https://github.com/crosire/reshade-shaders | CC0-1.0 |
| RenoDX-DLSS5 | https://github.com/RankFTW/rhi-repo (RHI mirror) | MIT (source: Yukikaze20170315/RenoDX-DLSS5-Generic, clshortfuse/renodx) |
| NVIDIA DLSS (`nvngx_dlss.dll`) | https://github.com/NVIDIA/DLSS | NVIDIA RTX SDKs licence |
| NVIDIA DLSS neural rendering (`nvngx_dlssnr.dll`) | https://github.com/RankFTW/rhi-repo (RHI mirror) | NVIDIA |
