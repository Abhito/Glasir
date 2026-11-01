// SPDX-License-Identifier: MIT
// ReShade.ini (next to mpv.exe): Glasir writes what glasir.ini decides there
// before mpv starts, and leaves everything else to ReShade and the user.
#pragma once
#include <string>

#include "settings.h"

namespace glasir {

// False if ReShade (dxgi.dll) is not in the engine folder: no ReShade, so no
// frame generation (and later no DLSS 5).
bool reshade_installed(const std::wstring &engine_dir);

void write_reshade_config(const std::wstring &engine_dir, const Settings &s);

}  // namespace glasir
