// SPDX-License-Identifier: MIT
// Setting values in glasir.ini's text without touching anything else (its
// comments, spacing, other keys). Used by Glasir.exe and by the in-game menu.
#pragma once
#include <string>
#include <vector>

namespace glasir {

struct IniValue {
    std::string section, key, value;  // UTF-8; section and key match without case
};

// The text with these values set: an existing key keeps its line (spacing,
// a comment after it), a missing one is added at the end of its section (the
// section at the end of the file if there is none).
std::string ini_set_values(const std::string &text, const std::vector<IniValue> &values);

// ini_set_values on a file (UTF-8, a byte order mark kept), replaced as a
// whole. False if it could not be written.
bool ini_save_values(const std::wstring &path, const std::vector<IniValue> &values);

}  // namespace glasir
