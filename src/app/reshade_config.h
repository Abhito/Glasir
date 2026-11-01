// SPDX-License-Identifier: MIT
// ReShade.ini, ReShadePreset.ini and dlss5-feed.cfg (next to mpv.exe): Glasir
// writes what glasir.ini decides there before mpv starts, and leaves
// everything else to ReShade, the add-ons and the user.
#pragma once
#include <string>

#include "settings.h"

namespace glasir {

// False if ReShade (dxgi.dll) is not in the engine folder: no ReShade, so no
// frame generation and no DLSS 5.
bool reshade_installed(const std::wstring &engine_dir);

// The DLSS 5 components (add-ons, runtime, shader) are in the engine folder.
bool dlss5_installed(const std::wstring &engine_dir);

// dlss5: whether DLSS 5 runs this time (glasir.ini and the GPU decided).
void write_reshade_config(const std::wstring &engine_dir, const Settings &s, bool dlss5);

}  // namespace glasir
