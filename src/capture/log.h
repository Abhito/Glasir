// SPDX-License-Identifier: MIT
// One log file for the whole process, safe to use from any thread, plus the
// text helpers log lines and messages share.
#pragma once
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace glasir {

// Starts a new log at path (truncating it). Until then, and if it fails,
// lines go to stderr.
bool log_open(const std::wstring &path);
void log_close();
void log_line(std::wstring_view text);

template <class... Args>
void log(std::wformat_string<Args...> fmt, Args &&...args) {
    log_line(std::format(fmt, std::forward<Args>(args)...));
}

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view text);  // to UTF-8

// "0x80004005"
inline std::wstring hresult_text(long hr) { return std::format(L"0x{:08X}", static_cast<unsigned long>(hr)); }

}  // namespace glasir
