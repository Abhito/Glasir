// SPDX-License-Identifier: MIT
// The setup windows: which components to download, and the download itself.
#pragma once
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "components.h"

namespace glasir {

struct SetupChoice {
    bool first_run = false;      // something Glasir needs is missing
    bool offer_dlss5 = true;     // show the "also DLSS 5" box
    bool dlss5_checked = false;  // ticked to start with
    std::wstring notices_file;   // THIRD-PARTY-NOTICES.md, linked from the text
};

// Asks what to download. nullopt if the user said no.
std::optional<std::vector<Component>> ask_setup(const SetupChoice &choice);

// A job with a progress window (dialogs; Cancel sets cancel) or without.
// False with error set if it failed or was cancelled ("cancelled").
using ProgressJob = std::function<bool(SetupProgress &, const std::atomic<bool> &, std::wstring &)>;
bool run_with_progress(const wchar_t *title, const ProgressJob &job, bool dialogs, std::wstring &error);

// Downloads and installs with a progress window (dialogs) or only the log.
// False with error set if it failed or was cancelled ("cancelled").
bool run_setup(const std::wstring &engine_dir, const std::vector<Component> &which, bool dialogs,
               std::wstring &error);

}  // namespace glasir
