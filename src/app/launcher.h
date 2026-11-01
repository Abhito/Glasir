// SPDX-License-Identifier: MIT
// Glasir's window before playing: the card and its mode, SDR or HDR, the
// enhancements, the sound and the window mode. An ordinary window (mpv and
// ReShade are not running yet), drawn with Dear ImGui in Glasir's look.
// What it changes goes into glasir.ini when Play is pressed.
#pragma once
#include <functional>
#include <string>

namespace glasir {

class UpdateCheck;

// Setup: download the components again. SelfUpdate: install the newer
// Glasir the update check found.
enum class LauncherAction { Play, Setup, SelfUpdate, Quit };

struct LauncherExtras {
    const UpdateCheck *update = nullptr;  // shows "Glasir x.y.z is out" when it found one
    // Saves a bug report (run on a thread of its own); the zip's path.
    std::function<bool(std::wstring &zip, std::wstring &error)> bug_report;
};

LauncherAction run_launcher(const std::wstring &settings_path, const std::wstring &engine_dir,
                            const LauncherExtras &extras);

}  // namespace glasir
