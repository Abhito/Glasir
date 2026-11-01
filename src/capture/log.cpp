// SPDX-License-Identifier: MIT
#include "log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <share.h>

namespace glasir {
namespace {
std::mutex g_mutex;
FILE *g_file = nullptr;
}  // namespace

bool log_open(const std::wstring &path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) fclose(g_file);
    g_file = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    return g_file != nullptr;
}

void log_close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) fclose(g_file);
    g_file = nullptr;
}

void log_line(std::wstring_view text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::wstring line = std::format(L"{:02}:{:02}:{:02}.{:03} {}\n", t.wHour, t.wMinute, t.wSecond,
                                          t.wMilliseconds, text);
    std::lock_guard<std::mutex> lock(g_mutex);
    FILE *out = g_file ? g_file : stderr;
    fputws(line.c_str(), out);
    fflush(out);
}

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(n > 0 ? n : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace glasir
