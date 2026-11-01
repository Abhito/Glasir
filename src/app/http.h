// SPDX-License-Identifier: MIT
// HTTPS downloads (WinHTTP, the system's proxy settings).
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace glasir {

struct HttpRequest {
    std::wstring url;
    std::vector<std::pair<std::wstring, std::wstring>> headers;  // besides User-Agent
    const std::atomic<bool> *cancel = nullptr;                   // stops the transfer when set
    // Called as data arrives: bytes so far, total (0 if the server did not say).
    std::function<void(uint64_t, uint64_t)> progress;
};

// The body of a 200 answer; false with error set otherwise.
bool http_get(const HttpRequest &request, std::string &body, std::wstring &error);
// Saves the body of a 200 answer to file (replaced); false with error set.
bool http_download(const HttpRequest &request, const std::wstring &file, std::wstring &error);

}  // namespace glasir
