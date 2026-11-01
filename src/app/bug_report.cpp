// SPDX-License-Identifier: MIT
#include "bug_report.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <regex>
#include <vector>

#include "components.h"
#include "files.h"
#include "glasir_version.h"
#include "log.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace glasir {
namespace {

std::wstring registry_text(const wchar_t *value) {
    wchar_t text[256] = {};
    DWORD size = sizeof(text);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", value, RRF_RT_REG_SZ,
                     nullptr, text, &size) != ERROR_SUCCESS)
        return L"";
    return text;
}

DWORD registry_number(const wchar_t *value) {
    DWORD n = 0, size = sizeof(n);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", value, RRF_RT_REG_DWORD,
                 nullptr, &n, &size);
    return n;
}

std::wstring windows_text() {
    using RtlGetVersion = LONG(WINAPI *)(OSVERSIONINFOW *);
    OSVERSIONINFOW v{sizeof(v)};
    if (auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        get(&v);
    return std::format(L"Windows {}.{}.{}.{} ({}, {})", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
                       registry_number(L"UBR"), registry_text(L"DisplayVersion"), registry_text(L"EditionID"));
}

std::wstring color_space_text(DXGI_COLOR_SPACE_TYPE c) {
    switch (c) {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return L"HDR on";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return L"SDR";
    default: return std::format(L"color space {}", int(c));
    }
}

// Removes the leading byte-order mark a UTF-8 file may start with.
std::string without_bom(std::string text) {
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    return text;
}

std::wstring profile_folder() {
    wchar_t profile[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    return n > 0 && n < MAX_PATH ? profile : L"";
}

// The profile folder's short (8.3) name, if it has one ("C:\Users\SOMEON~1").
std::wstring short_name(const std::wstring &path) {
    wchar_t out[MAX_PATH] = {};
    const DWORD n = GetShortPathNameW(path.c_str(), out, MAX_PATH);
    return n > 0 && n < MAX_PATH && _wcsicmp(out, path.c_str()) != 0 ? out : L"";
}

// The frames folder, if it is one the add-on makes: <app>\Captures\<date-time>
// (the message naming it comes over mpv's pipe, which any program can use).
std::wstring capture_folder(const std::wstring &app_dir, const std::wstring &folder) {
    if (folder.empty()) return L"";
    std::error_code ec;
    const fs::path captures = fs::weakly_canonical(fs::path(app_dir) / L"Captures", ec);
    const fs::path f = fs::weakly_canonical(folder, ec);
    if (ec || !fs::is_directory(f, ec) || _wcsicmp(f.parent_path().c_str(), captures.c_str()) != 0 ||
        !std::regex_match(f.filename().wstring(), std::wregex(LR"(\d{8}-\d{6}-\d{3}(-\d{1,2})?)"))) {
        log(L"bug report: {} is not a Captures folder; the report has no frames", folder);
        return L"";
    }
    return f.wstring();
}

std::wstring stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

}  // namespace

std::wstring system_summary() {
    std::wstring text = windows_text() + L"\n";
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return text + L"DXGI: not available\n";
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        LARGE_INTEGER umd{};
        std::wstring driver = L"?";
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        {
            driver = std::format(L"{}.{}.{}.{}", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                                 LOWORD(umd.LowPart));
            // NVIDIA's own number is the last five digits: 32.0.16.2101 = 621.01.
            if (desc.VendorId == 0x10DE) {
                const unsigned n = (HIWORD(umd.LowPart) % 10) * 10000 + LOWORD(umd.LowPart);
                driver += std::format(L" = NVIDIA {}.{:02}", n / 100, n % 100);
            }
        }
        text += std::format(L"GPU {}: {} (vendor {:04X}, device {:04X}, {} MB, driver {}){}\n", i, desc.Description,
                            desc.VendorId, desc.DeviceId, desc.DedicatedVideoMemory >> 20, driver,
                            desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ? L" software" : L"");
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC out{};
            if (FAILED(output->GetDesc(&out))) continue;
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            const bool known = EnumDisplaySettingsW(out.DeviceName, ENUM_CURRENT_SETTINGS, &mode);
            const RECT &r = out.DesktopCoordinates;
            std::wstring line = std::format(L"  screen {}: {}x{} at {},{}", out.DeviceName, r.right - r.left,
                                            r.bottom - r.top, r.left, r.top);
            if (known) line += std::format(L", {} Hz", mode.dmDisplayFrequency);
            if (out.Monitor == MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)) line += L", main";
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 d1{};
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&d1)))
                line += std::format(L", {}, {} bits, peak {:.0f} nits", color_space_text(d1.ColorSpace),
                                    d1.BitsPerColor, d1.MaxLuminance);
            text += line + L"\n";
        }
    }
    return text;
}

std::string redact_profile(const std::string &text, const std::wstring &profile) {
    if (profile.size() < 4) return text;  // not a real profile path ("C:\")
    std::vector<std::string> spellings;
    const std::string base = narrow(profile);
    spellings.push_back(base);
    std::string slash = base, doubled;
    for (char &c : slash)
        if (c == '\\') c = '/';
    for (char c : base) doubled += c == '\\' ? std::string("\\\\") : std::string(1, c);
    spellings.push_back(slash);
    spellings.push_back(doubled);
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        size_t matched = 0;
        for (const std::string &s : spellings) {
            if (s.size() <= matched || i + s.size() > text.size()) continue;
            bool same = true;
            for (size_t k = 0; k < s.size() && same; ++k) same = lower(text[i + k]) == lower(s[k]);
            if (same) matched = s.size();
        }
        if (matched) {
            out += "%USERPROFILE%";
            i += matched;
        } else {
            out += text[i++];
        }
    }
    return out;
}

bool save_bug_report(const BugReportInput &in, std::wstring &zip, std::wstring &error) {
    const std::wstring engine = in.app_dir + L"\\engine";
    const std::wstring profile = profile_folder(), profile_short = short_name(profile);
    auto redact = [&](const std::string &text) {
        const std::string once = redact_profile(text, profile);
        return profile_short.empty() ? once : redact_profile(once, profile_short);
    };
    const std::wstring frames = capture_folder(in.app_dir, in.frames);
    WorkDir work(L"bug-report");
    const fs::path stage = work.path();
    std::error_code ec;
    fs::create_directories(stage / L"logs", ec);
    fs::create_directories(stage / L"engine", ec);

    // The text files, as they are now (logs still being written included).
    struct Item {
        std::wstring from, to;
    };
    const std::vector<Item> items = {
        {in.settings_path, L"glasir.ini"},
        {in.app_dir + L"\\logs\\glasir.log", L"logs\\glasir.log"},
        {in.app_dir + L"\\logs\\glasir-previous.log", L"logs\\glasir-previous.log"},
        {in.app_dir + L"\\logs\\audio.log", L"logs\\audio.log"},
        {in.app_dir + L"\\logs\\mpv.log", L"logs\\mpv.log"},
        {engine + L"\\ReShade.log", L"engine\\ReShade.log"},
        {engine + L"\\ReShade.ini", L"engine\\ReShade.ini"},
        {engine + L"\\ReShadePreset.ini", L"engine\\ReShadePreset.ini"},
        {engine + L"\\dlss5-feed.cfg", L"engine\\dlss5-feed.cfg"},
        {engine + L"\\dlss5-feed.log", L"engine\\dlss5-feed.log"},
        {engine + L"\\RenoDX-DLSS5-crash.log", L"engine\\RenoDX-DLSS5-crash.log"},
        {engine + L"\\components.ini", L"engine\\components.ini"},
    };
    std::wstring listed;
    for (const Item &item : items) {
        std::string data;
        if (!file_exists(item.from)) continue;
        if (!read_shared(item.from, data)) {
            listed += L"  " + item.to + L": could not be read\n";
            continue;
        }
        // A long session's debug log can be huge: its end is what matters.
        constexpr size_t kMaxBytes = 16u << 20;
        if (data.size() > kMaxBytes)
            data = std::format("[the first {} MB are left out]\n", (data.size() - kMaxBytes) >> 20) +
                   data.substr(data.size() - kMaxBytes);
        std::ofstream out(stage / item.to, std::ios::binary);
        const std::string clean = redact(data);
        out.write(clean.data(), std::streamsize(clean.size()));
        listed += std::format(L"  {} ({} KB)\n", item.to, (clean.size() + 1023) / 1024);
    }

    std::wstring frames_name;
    if (!frames.empty()) frames_name = fs::path(frames).filename().wstring();

    std::wstring system = std::format(L"{} {} ({})\nMade {}: {}\n\n", widen(GLASIR_NAME), widen(GLASIR_VERSION),
                                      widen(GLASIR_GIT_DESCRIBE), stamp(), in.how);
    system += system_summary();
    system += L"\nInstalled:\n";
    for (const auto &line : installed_versions(engine)) system += L"  " + line + L"\n";
    if (!in.extra.empty()) system += L"\n" + in.extra;
    system += L"\nFiles:\n" + listed;
    system += frames_name.empty() ? L"  (no frames: F9 while playing adds them when frame generation is on)\n"
                                  : L"  " + frames_name + L"\\: the frames saved with F9\n";
    {
        const std::string text = redact(without_bom(narrow(system)));
        std::ofstream out(stage / L"system.txt", std::ios::binary);
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) return error = L"could not write the report's files in " + work.path(), false;
    }

    const std::wstring folder = in.app_dir + L"\\Bug reports";
    fs::create_directories(folder, ec);
    zip = folder + L"\\Glasir-bug-report-" + stamp() + L".zip";
    std::vector<std::wstring> args = {L"-a", L"-c", L"-f", zip, L"-C", stage.wstring()};
    for (const fs::directory_entry &e : fs::directory_iterator(stage, ec)) args.push_back(e.path().filename().wstring());
    if (!frames_name.empty()) {
        args.push_back(L"-C");
        args.push_back(fs::path(frames).parent_path().wstring());
        args.push_back(frames_name);
    }
    // Below normal priority: it may run while a game is being played.
    if (!run_tar(args, true, error)) {
        DeleteFileW(zip.c_str());
        error = L"could not make the zip (" + error + L")";
        return false;
    }
    log(L"bug report saved: {} ({} KB)", zip, (fs::file_size(zip, ec) + 1023) / 1024);
    return true;
}

}  // namespace glasir
