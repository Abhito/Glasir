// SPDX-License-Identifier: MIT
#include "ini_edit.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <optional>

namespace glasir {
namespace {

bool blank(char c) { return c == ' ' || c == '\t'; }

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && blank(s[a])) ++a;
    while (b > a && blank(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::optional<std::string> section_of(const std::string &line) {
    const std::string t = trim(line);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return std::nullopt;
    return lower(trim(t.substr(1, t.size() - 2)));
}

std::string key_of(const std::string &line) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return {};
    const size_t eq = t.find('=');
    return eq == std::string::npos ? std::string() : lower(trim(t.substr(0, eq)));
}

// "key = old ; note" -> "key = value ; note".
void replace_value(std::string &line, const std::string &value) {
    const size_t eq = line.find('=');
    size_t start = eq + 1;
    while (start < line.size() && blank(line[start])) ++start;
    size_t end = line.size();
    for (size_t k = start; k < line.size(); ++k)
        if ((line[k] == ';' || line[k] == '#') && (k == start || blank(line[k - 1]))) {
            end = k;
            while (end > start && blank(line[end - 1])) --end;
            break;
        }
    std::string prefix = line.substr(0, start);
    if (prefix.back() == '=' && eq > 0 && line[eq - 1] == ' ') prefix += ' ';  // "device =" -> "device = x"
    std::string tail = line.substr(end);
    if (!tail.empty() && !blank(tail[0])) tail = " " + tail;  // keeps it a comment
    line = prefix + value + tail;
}

}  // namespace

std::string ini_set_values(const std::string &text, const std::vector<IniValue> &values) {
    // Lines without their ends; the file's own line end is kept.
    const std::string eol =
        text.find("\r\n") != std::string::npos || text.find('\n') == std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        at = nl + 1;
    }

    for (const IniValue &v : values) {
        const std::string section = lower(v.section), key = lower(v.key);
        bool in_section = false, done = false;
        size_t section_end = std::string::npos;  // after the section's last setting
        for (size_t i = 0; i < lines.size() && !done; ++i) {
            if (const auto s = section_of(lines[i])) {
                in_section = *s == section;
                if (in_section) section_end = i + 1;
                continue;
            }
            const std::string k = key_of(lines[i]);
            if (!in_section || k.empty()) continue;
            section_end = i + 1;
            if (k != key) continue;
            replace_value(lines[i], v.value);
            done = true;
        }
        if (done) continue;
        const std::string entry = v.key + " = " + v.value;
        if (section_end != std::string::npos) {
            lines.insert(lines.begin() + std::ptrdiff_t(section_end), entry);
        } else {
            if (!lines.empty() && !lines.back().empty()) lines.push_back("");
            lines.push_back("[" + v.section + "]");
            lines.push_back(entry);
        }
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        // The last line ends as it did (a file without a final newline stays so).
        if (i + 1 < lines.size() || text.empty() || text.back() == '\n') out += eol;
    }
    return out;
}

bool ini_save_values(const std::wstring &path, const std::vector<IniValue> &values) {
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        if (in) text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const bool bom = text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0;
    const std::string updated = (bom ? "\xEF\xBB\xBF" : "") + ini_set_values(bom ? text.substr(3) : text, values);
    if (updated == text) return true;
    const std::wstring part = path + L".new";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out << updated;
        if (!out) return false;
    }
    if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(part.c_str());
        return false;
    }
    return true;
}

}  // namespace glasir
