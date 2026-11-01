// SPDX-License-Identifier: MIT
#include "settings.h"

#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#include "log.h"

namespace glasir {
namespace {

std::wstring trim(const std::wstring &s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring lower(std::wstring s) {
    for (auto &c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool parse_bool(const std::wstring &v, bool &out) {
    const std::wstring l = lower(v);
    if (l == L"1" || l == L"yes" || l == L"true" || l == L"on") return out = true, true;
    if (l == L"0" || l == L"no" || l == L"false" || l == L"off") return out = false, true;
    return false;
}

bool parse_int(const std::wstring &v, long lo, long hi, long &out) {
    if (v.empty()) return false;
    wchar_t *end = nullptr;
    const long n = wcstol(v.c_str(), &end, 10);
    if (*end || n < lo || n > hi) return false;
    out = n;
    return true;
}

bool parse_double(const std::wstring &v, double lo, double hi, double &out) {
    if (v.empty()) return false;
    wchar_t *end = nullptr;
    const double n = wcstod(v.c_str(), &end);
    if (*end || n < lo || n > hi) return false;
    out = n;
    return true;
}

// false: bad value (error set). Unknown keys are logged and accepted.
bool apply(Settings &s, const std::wstring &section, const std::wstring &key, const std::wstring &value,
           std::wstring &error) {
    const std::wstring name = section + L"." + key;
    auto bad = [&](const wchar_t *expected) {
        error = L"glasir.ini: " + name + L" = " + value + L" (expected " + expected + L")";
        return false;
    };
    long n = 0;
    double d = 0;
    bool b = false;
    if (name == L"capture.device") {
        s.device = value;
    } else if (name == L"capture.width") {
        if (!parse_int(value, 0, 16384, n) || n % 2) return bad(L"0 or an even number of pixels");
        s.width = UINT32(n);
    } else if (name == L"capture.height") {
        if (!parse_int(value, 0, 16384, n) || n % 2) return bad(L"0 or an even number of pixels");
        s.height = UINT32(n);
    } else if (name == L"capture.fps") {
        if (!parse_double(value, 0, 1000, d)) return bad(L"0 or frames per second");
        s.fps = d;
    } else if (name == L"capture.path") {
        const std::wstring l = lower(value);
        if (l == L"auto") s.path = Settings::CapturePath::Auto;
        else if (l == L"mediafoundation") s.path = Settings::CapturePath::MediaFoundation;
        else if (l == L"directshow") s.path = Settings::CapturePath::DirectShow;
        else return bad(L"auto, mediafoundation or directshow");
    } else if (name == L"video.mode") {
        const std::wstring l = lower(value);
        if (l == L"sdr") s.hdr = false;
        else if (l == L"hdr") s.hdr = true;
        else return bad(L"sdr or hdr");
    } else if (name == L"video.sdr_range") {
        const std::wstring l = lower(value);
        if (l == L"limited") s.sdr_full_range = false;
        else if (l == L"full") s.sdr_full_range = true;
        else return bad(L"limited or full");
    } else if (name == L"video.hdr_peak_nits") {
        if (!parse_int(value, 80, 10000, n)) return bad(L"80 to 10000");
        s.hdr_peak_nits = int(n);
    } else if (name == L"frames.dedup") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.dedup = b;
    } else if (name == L"frames.dedup_threshold") {
        if (!parse_int(value, 0, 1023, n)) return bad(L"0 to 1023");
        s.dedup_threshold = int(n);
    } else if (name == L"frames.dedup_keepalive_ms") {
        if (!parse_int(value, 0, 60000, n)) return bad(L"0 to 60000");
        s.dedup_keepalive_ms = DWORD(n);
    } else if (name == L"window.mode") {
        const std::wstring l = lower(value);
        if (l == L"borderless") s.borderless = true;
        else if (l == L"windowed") s.borderless = false;
        else return bad(L"borderless or windowed");
    } else if (name == L"debug.mpv_log") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.mpv_log = b;
    } else {
        log(L"glasir.ini: unknown setting {}, skipped", name);
    }
    return true;
}

}  // namespace

const char *default_settings_text() {
    return R"(; Glasir settings. Lines starting with ; are comments.
; A missing file is recreated with these defaults.

[capture]
; The card's name as "Glasir.exe --list" shows it. Empty: pick automatically.
device =
; Capture size and rate. 0: automatic (60 fps if offered, the largest size
; that fits the display).
width = 0
height = 0
fps = 0
; auto: Media Foundation if the card offers a usable mode, else DirectShow.
; DirectShow works with more cards but has no dedup or frame generation.
path = auto

[video]
; sdr or hdr. HDR needs a card with a 10-bit (P010) mode and HDR on the console.
mode = sdr
; The SDR signal's range: limited (most consoles) or full.
sdr_range = limited
; Peak brightness written into the HDR10 metadata.
hdr_peak_nits = 1000

[frames]
; 1: send mpv only new frames (a 30 fps game captured at 60 arrives at 30).
; Frame generation needs it.
dedup = 1
; How much a 16x16 block's average brightness must change (10-bit steps) to
; count as a new frame. Raise it for noisy sources.
dedup_threshold = 3
; A still picture is still sent this often (ms), so menus keep drawing.
dedup_keepalive_ms = 200

[window]
; borderless (fills the screen) or windowed.
mode = borderless

[debug]
; 1: mpv writes a detailed log to logs\mpv.log.
mpv_log = 0
)";
}

bool load_settings(const std::wstring &path, Settings &s, std::wstring &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::ofstream out(path, std::ios::binary);
        if (out) {
            const std::string text = default_settings_text();
            out << text;
            log(L"wrote default settings to {}", path);
        } else {
            log(L"could not write {}; using defaults", path);
        }
        return true;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
    std::wistringstream lines(widen(bytes));
    std::wstring line, section;
    for (int number = 1; std::getline(lines, line); ++number) {
        line = trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[') {
            if (line.back() != L']') {
                error = L"glasir.ini line " + std::to_wstring(number) + L": " + line;
                return false;
            }
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            error = L"glasir.ini line " + std::to_wstring(number) + L": " + line + L" (expected key = value)";
            return false;
        }
        std::wstring value = line.substr(eq + 1);
        // A comment after a value needs a space before it: "fps = 60 ; note".
        for (size_t i = 1; i < value.size(); ++i)
            if ((value[i] == L';' || value[i] == L'#') && iswspace(value[i - 1])) {
                value.resize(i);
                break;
            }
        if (!apply(s, section, lower(trim(line.substr(0, eq))), trim(value), error)) return false;
    }
    return true;
}

bool apply_override(Settings &s, const std::wstring &assignment, std::wstring &error) {
    const size_t dot = assignment.find(L'.');
    const size_t eq = assignment.find(L'=');
    if (dot == std::wstring::npos || eq == std::wstring::npos || dot > eq) {
        error = L"--set " + assignment + L" (expected section.key=value)";
        return false;
    }
    return apply(s, lower(trim(assignment.substr(0, dot))), lower(trim(assignment.substr(dot + 1, eq - dot - 1))),
                 trim(assignment.substr(eq + 1)), error);
}

}  // namespace glasir
