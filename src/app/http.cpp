// SPDX-License-Identifier: MIT
#include "http.h"

#include <winhttp.h>

#include <format>
#include <fstream>

#include "glasir_version.h"
#include "log.h"

namespace glasir {
namespace {

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET v) : h(v) {}
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::wstring winhttp_error(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"the server's name could not be found - is the PC online?";
    case ERROR_WINHTTP_CANNOT_CONNECT: return L"could not connect to the server";
    case ERROR_WINHTTP_TIMEOUT: return L"the server stopped answering";
    case ERROR_WINHTTP_CONNECTION_ERROR: return L"the connection was lost";
    case ERROR_WINHTTP_SECURE_FAILURE: return L"the server's certificate was not accepted";
    default: return std::format(L"network error {}", e);
    }
}

// Runs the request and passes the body to sink in pieces. sink returns false
// to stop (a write failed).
bool transfer(const HttpRequest &r, const std::function<bool(const char *, DWORD)> &sink, std::wstring &error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = DWORD(std::size(path));
    wchar_t extra[4096] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = DWORD(std::size(extra));
    if (!WinHttpCrackUrl(r.url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"not an https address: " + r.url;
        return false;
    }
    const std::wstring agent = std::format(L"Glasir/{}", widen(GLASIR_VERSION));
    Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        error = winhttp_error(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 30000);
    Handle connection(WinHttpConnect(session.h, host, parts.nPort, 0));
    const std::wstring object = std::wstring(path) + extra;
    Handle request(connection.h ? WinHttpOpenRequest(connection.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr);
    if (!request.h) {
        error = winhttp_error(GetLastError());
        return false;
    }
    std::wstring headers;
    for (const auto &[name, value] : r.headers) headers += name + L": " + value + L"\r\n";
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        error = winhttp_error(GetLastError());
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = status == 403 || status == 429
                    ? std::format(L"the server refused (HTTP {}); GitHub allows 60 requests an hour, try again later",
                                  status)
                    : std::format(L"the server answered HTTP {}", status);
        return false;
    }
    uint64_t total = 0;
    wchar_t length[32] = {};
    size = sizeof(length);
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &size,
                            WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(length, nullptr, 10);

    std::vector<char> buffer(256 * 1024);
    uint64_t done = 0;
    for (;;) {
        if (r.cancel && r.cancel->load()) {
            error = L"cancelled";
            return false;
        }
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buffer.data(), DWORD(buffer.size()), &got)) {
            error = winhttp_error(GetLastError());
            return false;
        }
        if (!got) break;
        if (!sink(buffer.data(), got)) {
            error = L"could not save the download (is the disk full?)";
            return false;
        }
        done += got;
        if (r.progress) r.progress(done, total);
    }
    if (total && done != total) {
        error = std::format(L"the download ended early ({} of {} bytes)", done, total);
        return false;
    }
    return true;
}

}  // namespace

bool http_get(const HttpRequest &request, std::string &body, std::wstring &error) {
    body.clear();
    const bool ok = transfer(request, [&](const char *data, DWORD n) {
        if (body.size() + n > (64u << 20)) return false;  // not a page
        body.append(data, n);
        return true;
    }, error);
    if (!ok) log(L"GET {}: {}", request.url, error);
    return ok;
}

bool http_download(const HttpRequest &request, const std::wstring &file, std::wstring &error) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = L"could not create " + file;
        return false;
    }
    bool ok = transfer(request, [&](const char *data, DWORD n) { return bool(out.write(data, n)); }, error);
    out.close();
    if (ok && !out) {
        ok = false;
        error = L"could not save " + file;
    }
    if (!ok) {
        log(L"download {}: {}", request.url, error);
        DeleteFileW(file.c_str());
    }
    return ok;
}

}  // namespace glasir
