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

// dlss5_loaded: the DLSS 5 add-ons work this time (installed, and the GPU
// can or glasir.ini says try); dlss5_on: its effects start switched on. Loaded
// but off, the in-game menu can switch them on without a restart.
void write_reshade_config(const std::wstring &engine_dir, const Settings &s, bool dlss5_loaded, bool dlss5_on);

}  // namespace glasir
