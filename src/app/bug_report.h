// SPDX-License-Identifier: MIT
// "Save a bug report": one zip with what is needed to see what went wrong -
// the logs, the settings (glasir.ini, ReShade's, DLSS 5's), what is
// installed, the PC (Windows, GPUs and their drivers, the screens) and, when
// it is made with F9 while playing, the frames frame generation saved then.
// Paths in it have the user's profile folder replaced by %USERPROFILE%, so
// the Windows user name is not in it.
#pragma once
#include <string>

namespace glasir {

struct BugReportInput {
    std::wstring app_dir;        // Glasir.exe's folder
    std::wstring settings_path;  // glasir.ini
    std::wstring frames;         // a Captures\<date-time> folder to add, "" for none
    std::wstring how;            // how it was made, for system.txt ("F9 while playing")
    std::wstring extra;          // more for system.txt (the capture cards)
};

// Saves the zip in <app_dir>\Bug reports\; zip is its path.
bool save_bug_report(const BugReportInput &input, std::wstring &zip, std::wstring &error);

// Windows, the GPUs with their drivers, the screens.
std::wstring system_summary();

// text with every spelling of profile ("C:\Users\Name", with / or \\ too)
// replaced by %USERPROFILE%, whatever its case.
std::string redact_profile(const std::string &text, const std::wstring &profile);

}  // namespace glasir
