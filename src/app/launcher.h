// SPDX-License-Identifier: MIT
// Glasir's window before playing: the card and its mode, SDR or HDR, the
// enhancements, the sound and the window mode. An ordinary window (mpv and
// ReShade are not running yet), drawn with Dear ImGui in Glasir's look.
// What it changes goes into glasir.ini when Play is pressed.
#pragma once
#include <string>

namespace glasir {

enum class LauncherAction { Play, Setup, Quit };

LauncherAction run_launcher(const std::wstring &settings_path, const std::wstring &engine_dir);

}  // namespace glasir
