// SPDX-License-Identifier: MIT
// Glasir.exe: reads glasir.ini, opens the capture card, starts mpv and passes
// the card's frames to it.
//
// Media Foundation first: Glasir reads the card's frames (NV12, YUY2 or UYVY
// for SDR, 10-bit P010 for HDR) and writes them, tightly packed, to mpv's
// standard input, minus repeats with dedup. That is what frame generation
// needs. Cards Media Foundation cannot use fall back to mpv's own DirectShow
// input: every frame, no dedup, no frame generation.
//
// Usage:
//   Glasir.exe [--play] [--settings FILE] [--set section.key=value ...] [--no-dialogs]
//   Glasir.exe --input-raw FILE|- [--raw-format nv12|p010|yuy2|uyvy] --set capture.width=W ...
//   Glasir.exe --list
//   Glasir.exe --version
//   Glasir.exe --setup [--components mpv,reshade,dlss5,vfg] [--newest] [--no-dialogs]
//                      [--accept-nvidia-licence]
//   Glasir.exe --update [--no-dialogs]
//   Glasir.exe --update-from Glasir-x.y.z.zip [--no-dialogs]
//   Glasir.exe --bug-report [--no-dialogs]
//
// --input-raw reads tightly packed frames from a file or standard input
// instead of the card (capture.width, capture.height and capture.fps must be
// set), for testing the same path without one.
//
// Glasir.exe opens its window first (launcher.cpp) unless glasir.ini says
// [launcher] show = 0 (Shift held at start shows it anyway), --play is given,
// or nothing may be shown (--no-dialogs, --input-raw).
//
// --setup downloads (or updates) the programs Glasir runs on: it asks which,
// or with --no-dialogs takes --components (default: mpv, ReShade, and DLSS 5
// if it is there already or the GPU is an RTX 30, 40 or 50). The first start
// does the same when mpv or ReShade is missing. What is downloaded is the
// version of each this Glasir was tested with (components.h); --newest takes
// the newest instead, to try before it is made the tested one.
// vfg is NVIDIA Video Frame Generation: never downloaded unasked. The window
// offers it and shows NVIDIA's licences first; --no-dialogs takes it only with
// --accept-nvidia-licence, which says the user has read and accepts them.
//
// --update installs the newest Glasir from its GitHub releases if it is
// newer (update.h); --update-from installs a release zip from disk. The
// launcher offers the same when its update check finds one.
//
// --bug-report saves a bug report (bug_report.h) in "Bug reports"; F9 while
// playing saves one with the frames on screen, and the launcher has a link.

#include <windows.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../common/framegen_link.h"
#include "audio_endpoint.h"
#include "backdrop.h"
#include "bug_report.h"
#include "capture.h"
#include "components.h"
#include "devices.h"
#include "files.h"
#include "glasir_version.h"
#include "launcher.h"
#include "log.h"
#include "mpv_launch.h"
#include "gpu.h"
#include "reshade_config.h"
#include "settings.h"
#include "setup_ui.h"
#include "update.h"

using namespace glasir;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kIpcPipe[] = L"\\\\.\\pipe\\glasir";
constexpr wchar_t kInstanceMutex[] = L"Local\\Glasir.Instance";
constexpr wchar_t kQuitEvent[] = L"Local\\Glasir.Quit";
constexpr wchar_t kAnotherApp[] = L"Is another app (OBS, Discord, the card's own app...) using it?";

struct Args {
    std::wstring settings_path;
    std::vector<std::wstring> overrides;
    std::wstring input_raw;
    std::wstring raw_format;
    std::wstring components;  // --setup: what to download, comma-separated
    std::wstring update_from;  // --update-from: a release zip
    bool list = false;
    bool version = false;
    bool setup = false;
    bool newest = false;  // --setup: the newest versions, not the tested ones
    bool accept_nvidia = false;  // --setup --components vfg without a window: NVIDIA's licences are accepted
    bool update = false;
    bool bug_report = false;
    bool from_launcher = false;  // setup asked for in the launcher: it comes back after
    bool play = false;           // no launcher
    bool dialogs = true;
};

// The error the user is shown. It is shown only once Glasir has let go of
// everything (the card, mpv, the one-instance lock), so a dialog left open
// blocks nothing - not the card, not the next Glasir.
std::wstring g_error;

void report(const std::wstring &text) {
    log(L"error: {}", text);
    if (g_error.empty()) g_error = text;
}

// An error as the end of a sentence: with a full stop unless it has one
// ("... is the PC online?").
std::wstring sentence(const std::wstring &text) {
    if (!text.empty() && (text.back() == L'.' || text.back() == L'?' || text.back() == L'!')) return text;
    return text + L".";
}

void show(const std::wstring &text, UINT icon) {
    MessageBoxW(nullptr, text.c_str(), L"Glasir", MB_OK | icon | MB_SETFOREGROUND);
}

std::wstring app_dir() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    return path.substr(0, path.find_last_of(L'\\'));
}

bool parse_args(Args &a, std::wstring &error) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool ok = true;
    for (int i = 1; i < argc && ok; ++i) {
        const std::wstring arg = argv[i];
        auto value = [&](std::wstring &out) {
            if (i + 1 >= argc) {
                error = L"Missing value for " + arg + L".";
                return ok = false;
            }
            out = argv[++i];
            return true;
        };
        if (arg == L"--list") {
            a.list = true;
        } else if (arg == L"--version") {
            a.version = true;
        } else if (arg == L"--play") {
            a.play = true;
        } else if (arg == L"--setup") {
            a.setup = true;
        } else if (arg == L"--newest") {
            a.newest = true;
        } else if (arg == L"--accept-nvidia-licence") {
            a.accept_nvidia = true;
        } else if (arg == L"--update") {
            a.update = true;
        } else if (arg == L"--update-from") {
            value(a.update_from);
        } else if (arg == L"--bug-report") {
            a.bug_report = true;
        } else if (arg == L"--components") {
            value(a.components);
        } else if (arg == L"--no-dialogs") {
            a.dialogs = false;
        } else if (arg == L"--settings") {
            value(a.settings_path);
        } else if (arg == L"--set") {
            std::wstring v;
            if (value(v)) a.overrides.push_back(v);
        } else if (arg == L"--input-raw") {
            value(a.input_raw);
        } else if (arg == L"--raw-format") {
            value(a.raw_format);
        } else {
            error = L"Unknown argument " + arg + L".";
            ok = false;
        }
    }
    LocalFree(argv);
    return ok;
}

// Text for --list, --version and argument errors: to the file or pipe output
// goes to, else the console Glasir was started from (Glasir is a windowed
// program, so it attaches only here), else a dialog (a shortcut has neither).
void print(const std::wstring &text, bool dialogs) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD kind = GetFileType(out) & ~FILE_TYPE_REMOTE;
    DWORD mode = 0;
    const bool device = kind == FILE_TYPE_CHAR && !GetConsoleMode(out, &mode);  // e.g. "> NUL"
    if (kind == FILE_TYPE_DISK || kind == FILE_TYPE_PIPE || device) {
        const std::string utf8 = narrow(text);
        DWORD written = 0;
        if (WriteFile(out, utf8.data(), DWORD(utf8.size()), &written, nullptr) && written == utf8.size()) return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
        if (con != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteConsoleW(con, text.c_str(), DWORD(text.size()), &written, nullptr);
            CloseHandle(con);
            return;
        }
    }
    if (dialogs) show(text, MB_ICONINFORMATION);
}

std::wstring device_list_text() {
    std::wstring text;
    HRESULT hr = S_OK;
    const auto devices = list_devices(&hr);
    if (FAILED(hr)) text += std::format(L"Media Foundation device list failed ({})\n", hresult_text(hr));
    for (const auto &d : devices) {
        text += d.name + L"\n";
        if (!d.opened) text += L"  (could not be opened)\n";
        // One line per format and size, its rates fastest first.
        auto modes = d.modes;
        std::stable_sort(modes.begin(), modes.end(), [](const VideoMode &a, const VideoMode &b) {
            if (a.supported != b.supported) return a.supported;
            if (a.subtype != b.subtype) return a.subtype < b.subtype;
            if (a.width * a.height != b.width * b.height) return a.width * a.height > b.width * b.height;
            if (a.width != b.width) return a.width > b.width;
            return a.fps > b.fps;
        });
        for (size_t i = 0; i < modes.size();) {
            const VideoMode &m = modes[i];
            text += std::format(L"  {:<5} {:>4}x{:<4} @", m.subtype, m.width, m.height);
            for (; i < modes.size() && modes[i].subtype == m.subtype && modes[i].width == m.width &&
                   modes[i].height == m.height;
                 ++i)
                text += std::format(L" {}", std::round(modes[i].fps * 1000) / 1000);
            text += m.supported ? L"\n" : L"  (DirectShow only)\n";
        }
    }
    bool header = false;
    for (const auto &name : list_directshow_devices()) {
        bool known = false;
        for (const auto &d : devices) known = known || names_match(d.name, name);
        if (known) continue;
        if (!header) text += L"\nDirectShow only (no dedup or frame generation):\n";
        header = true;
        text += name + L"\n";
    }
    if (text.empty()) text = L"No capture devices found.\n";
    return text;
}

// One Glasir at a time: a second one asks the first to close (it ends its
// mpv) and takes over. Nothing else is touched - not other mpv windows.
class Instance {
public:
    explicit Instance(HANDLE quit_event) : quit_(quit_event) {}
    Instance(const Instance &) = delete;
    Instance &operator=(const Instance &) = delete;
    ~Instance() { release(); }

    bool take() {
        mutex_ = CreateMutexW(nullptr, FALSE, kInstanceMutex);
        if (!mutex_) return false;
        DWORD w = WaitForSingleObject(mutex_, 0);
        if (w == WAIT_TIMEOUT) {
            log(L"another Glasir is running: asking it to close");
            // Asked again and again: the other one may have just taken the
            // lock and cleared an earlier request before it was watching.
            const ULONGLONG deadline = GetTickCount64() + 10000;
            do {
                SetEvent(quit_);
                w = WaitForSingleObject(mutex_, 250);
            } while (w == WAIT_TIMEOUT && GetTickCount64() < deadline);
        }
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
            CloseHandle(mutex_);
            mutex_ = nullptr;
            return false;
        }
        owned_ = true;
        ResetEvent(quit_);
        return true;
    }

    void release() {
        if (owned_) ReleaseMutex(mutex_);
        owned_ = false;
        if (mutex_) CloseHandle(mutex_);
        mutex_ = nullptr;
    }

private:
    HANDLE quit_;
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
};

bool parse_raw_format(const std::wstring &name, bool hdr, PixelFormat &f) {
    if (name.empty()) {
        f = hdr ? PixelFormat::P010 : PixelFormat::NV12;
        return true;
    }
    for (PixelFormat c : {PixelFormat::NV12, PixelFormat::P010, PixelFormat::YUY2, PixelFormat::UYVY})
        if (CompareStringOrdinal(name.c_str(), -1, format_name(c), -1, TRUE) == CSTR_EQUAL) {
            f = c;
            return true;
        }
    return false;
}

std::wstring mode_text(const VideoMode &m) {
    return std::format(L"{} {}x{} @ {:.3f}", m.subtype, m.width, m.height, m.fps);
}

std::wstring request_text(const Settings &s) {
    std::wstring t = s.width && s.height ? std::format(L"{}x{}", s.width, s.height) : L"any size";
    t += s.fps > 0 ? std::format(L" @ {}", s.fps) : L"";
    return t;
}

// Every 5 s in which mpv missed new frames gets a log line, so the log shows
// whether mpv keeps up with the source.
class BehindLog {
public:
    void check(FrameSlot &slot) {
        const ULONGLONG now = GetTickCount64();
        UINT64 published = 0, dropped = 0;
        if (start_ == 0) {
            start_ = now;
            slot.counts(published_, dropped_);
            if (dropped_) log(L"{} of {} new frames dropped before mpv started reading", dropped_, published_);
            return;
        }
        if (now - start_ < 5000) return;
        slot.counts(published, dropped);
        if (dropped > dropped_)
            log(L"mpv fell behind: {} of {} new frames dropped in the last {:.1f} s", dropped - dropped_,
                published - published_, (now - start_) / 1000.0);
        published_ = published;
        dropped_ = dropped;
        start_ = now;
    }

private:
    ULONGLONG start_ = 0;
    UINT64 published_ = 0, dropped_ = 0;
};

// The block the frame generation add-on in mpv reads (common/framegen_link.h).
GlasirFrameGenLink *create_link(DWORD mpv_pid, HANDLE &mapping) {
    const std::wstring name = std::format(L"Local\\Glasir_FrameGen_{}", mpv_pid);
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(GlasirFrameGenLink),
                                 name.c_str());
    if (!mapping) {
        log(L"could not create the frame generation link (error {})", GetLastError());
        return nullptr;
    }
    auto *link = static_cast<GlasirFrameGenLink *>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GlasirFrameGenLink)));
    if (!link) {
        log(L"could not map the frame generation link (error {})", GetLastError());
        CloseHandle(mapping);
        mapping = nullptr;
        return nullptr;
    }
    link->version = GLASIR_FRAMEGEN_LINK_VERSION;
    MemoryBarrier();
    link->magic = GLASIR_FRAMEGEN_LINK_MAGIC;  // last: a reader that sees it sees the rest
    return link;
}

struct Source {
    Source() = default;
    Source(const Source &) = delete;
    Source &operator=(const Source &) = delete;
    ~Source() { close(); }

    // Lets go of the card: a read still waiting is cancelled.
    void close() {
        reader.Reset();
        if (source) source->Shutdown();
        source.Reset();
        if (raw_input && raw_owned) CloseHandle(raw_input);
        raw_input = nullptr;
    }

    bool raw = true;
    bool hdr_available = false;  // the card has a 10-bit mode (P010) Glasir can read: the menu offers HDR
    // Media Foundation
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    std::wstring device_name;
    std::wstring symbolic_link;  // the card's, for finding its audio input
    // --input-raw
    HANDLE raw_input = nullptr;
    bool raw_owned = false;
    FrameLayout layout;
    double fps = 0;
    // DirectShow
    std::wstring dshow_device;
    std::optional<VideoMode> dshow_mode;
};

std::wstring open_error(const std::wstring &device, HRESULT hr) {
    if (hr == MF_E_NOT_FOUND)
        return L"The capture card \"" + device + L"\" disappeared while Glasir was opening it. Check its cable.";
    if (hr == MF_E_INVALIDMEDIATYPE)
        return L"The capture card \"" + device + L"\" stopped offering the mode Glasir picked. Start Glasir again.";
    return L"Could not open the capture card \"" + device + L"\" (" + hresult_text(hr) + L"). " + kAnotherApp;
}

// Picks and opens the source. False after reporting why there is none.
// video.mode = auto: s.hdr becomes what the picture is shown as to start
// with - what auto found last time with a 10-bit capture, SDR without one.
bool open_source(const Args &args, Settings &s, Source &src) {
    if (!args.input_raw.empty()) {
        if (!s.width || !s.height || s.fps <= 0) {
            report(L"--input-raw needs capture.width, capture.height and capture.fps.");
            return false;
        }
        if (!parse_raw_format(args.raw_format, s.hdr || s.hdr_auto, src.layout.format)) {
            report(L"Unknown --raw-format " + args.raw_format + L" (nv12, p010, yuy2 or uyvy).");
            return false;
        }
        // A 10-bit input is shown as what the signal was found to be last time.
        if (s.hdr_auto) s.hdr = src.layout.format == PixelFormat::P010 && s.auto_found_hdr;
        src.raw_owned = args.input_raw != L"-";
        src.raw_input = src.raw_owned ? CreateFileW(args.input_raw.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)
                                      : GetStdHandle(STD_INPUT_HANDLE);
        if (src.raw_input == INVALID_HANDLE_VALUE || !src.raw_input) {
            src.raw_input = nullptr;
            report(L"Could not open the raw input " + args.input_raw + L".");
            return false;
        }
        src.layout.width = s.width;
        src.layout.height = s.height;
        src.fps = s.fps;
        src.device_name = args.input_raw;
        src.hdr_available = true;  // whatever the input is, --raw-format says
        return true;
    }

    ModeRequest request;
    request.hdr = s.hdr;
    request.width = s.width;
    request.height = s.height;
    request.fps = s.fps;
    request.max_auto_height = UINT32(GetSystemMetrics(SM_CYSCREEN));
    if (s.hdr_auto) request.hdr = s.hdr = false;  // the card is picked as for SDR; its 10-bit mode if it has one

    // The card list also guides the DirectShow path: which device is the
    // card, and which mode to ask for.
    const std::vector<CaptureDevice> devices = list_devices();
    ModeRequest any = request;
    any.supported_only = false;
    const CaptureDevice *device = nullptr;
    auto hdr_modes = [](const CaptureDevice &d) {
        for (const auto &m : d.modes)
            if (m.supported && m.format == PixelFormat::P010) return true;
        return false;
    };
    if (s.path != Settings::CapturePath::DirectShow) {
        device = choose_device(devices, s.device, request);
        if (!device && s.hdr_auto) {
            // A card whose only mode at this size and rate is 10-bit.
            ModeRequest ten_bit = request;
            ten_bit.hdr = true;
            device = choose_device(devices, s.device, ten_bit);
        }
        if (device) {
            auto mode = choose_mode(device->modes, request);
            if (s.hdr_auto) {
                ModeRequest ten_bit = request;
                ten_bit.hdr = true;
                if (const auto m = choose_mode(device->modes, ten_bit)) {
                    mode = m;
                    s.hdr = s.auto_found_hdr;
                } else {
                    log(L"video.mode = auto: \"{}\" has no 10-bit mode at {}: SDR", device->name, request_text(s));
                }
            }
            if (mode) {
                log(L"capture: \"{}\", {} (Media Foundation)", device->name, mode_text(*mode));
                const HRESULT hr = open_reader(*device, *mode, src.source, src.reader);
                if (FAILED(hr)) {
                    report(open_error(device->name, hr));
                    return false;
                }
                src.raw = true;
                src.layout = {mode->format, mode->width, mode->height};
                src.fps = mode->fps;
                src.device_name = device->name;
                src.symbolic_link = device->symbolic_link;
                src.hdr_available = hdr_modes(*device);
                return true;
            }
        }
    }
    if (!device && !s.hdr) device = choose_device(devices, s.device, any);
    if (!device && s.device.empty()) {
        // No card offers what glasir.ini asks: still find the card, so the
        // message below can say so.
        ModeRequest loose = any;
        loose.hdr = false;
        loose.width = loose.height = 0;
        loose.fps = 0;
        device = choose_device(devices, L"", loose);
    }

    // No usable Media Foundation mode.
    const std::wstring wanted = s.device.empty() ? L"" : L" \"" + s.device + L"\"";
    if (s.hdr) {
        report(device ? L"The capture card \"" + device->name + L"\" has no 10-bit HDR mode (P010) at " +
                            request_text(s) + L". Set video.mode = sdr in glasir.ini, or another size or rate."
                      : L"No capture card" + wanted + L" with a 10-bit HDR mode (P010) was found.");
        return false;
    }
    if (s.path == Settings::CapturePath::MediaFoundation) {
        report(device ? L"The capture card \"" + device->name + L"\" offers no mode Glasir can read at " +
                            request_text(s) + L". Set capture.path = auto to let mpv open it through DirectShow."
                      : L"No capture card" + wanted + L" was found through Media Foundation.");
        return false;
    }
    const auto dshow = list_directshow_devices();
    std::wstring name;
    if (device) {
        name = device->name;
    } else if (!s.device.empty()) {
        for (const auto &n : dshow)
            if (names_match(n, s.device)) name = n;
        for (const auto &n : dshow)
            if (name.empty() && name_contains(n, s.device)) name = n;
    } else {
        // Only DirectShow sees it: the first that does not look like a camera.
        for (const auto &n : dshow)
            if (name.empty() && !looks_like_camera(n)) name = n;
        if (name.empty() && !dshow.empty()) name = dshow.front();
    }
    if (name.empty()) {
        report(s.device.empty() ? std::wstring(L"No capture card was found. Check that it is plugged in and its "
                                               L"driver is installed.")
                                : L"The capture card \"" + s.device +
                                      L"\" from glasir.ini was not found. \"Glasir.exe --list\" shows the cards "
                                      L"this PC has.");
        return false;
    }
    if (device) src.dshow_mode = choose_mode(device->modes, any);
    if (device && !device->modes.empty() && !src.dshow_mode) {
        // The card says what it offers, and what glasir.ini asks is not there:
        // say so rather than let the driver pick something else.
        report(L"The capture card \"" + name + L"\" does not offer " + request_text(s) +
               L". \"Glasir.exe --list\" shows its modes; set capture.width, capture.height and capture.fps to "
               L"one of them, or to 0.");
        return false;
    }
    src.raw = false;
    src.dshow_device = name;
    src.device_name = name;
    if (device) src.symbolic_link = device->symbolic_link;
    log(L"capture: \"{}\"{} (DirectShow: no dedup or frame generation)", name,
        src.dshow_mode ? L", " + mode_text(*src.dshow_mode) : L"");
    return true;
}

// Waits for the quit event (another Glasir starting) and ends mpv.
class QuitWatcher {
public:
    QuitWatcher(HANDLE quit_event, MpvProcess &mpv) : stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        thread_ = std::thread([this, quit_event, &mpv] {
            HANDLE handles[] = {quit_event, stop_};
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0) {
                log(L"another Glasir started: closing");
                requested = true;
                mpv.stop(kIpcPipe, 2000);
            }
        });
    }
    ~QuitWatcher() {
        SetEvent(stop_);
        thread_.join();
        CloseHandle(stop_);
    }
    std::atomic<bool> requested{false};

private:
    HANDLE stop_;
    std::thread thread_;
};

// The card's audio input for the audio plugin. Without one there is no sound
// (mpv says so on screen); --input-raw tests get sound only when asked.
void choose_audio(const Settings &s, const Source &src, MpvPlan &plan) {
    if (src.raw_input && s.audio_device.empty()) return;
    const auto inputs = list_audio_inputs();
    const auto container = video_container(src.symbolic_link);
    if (const auto audio = find_card_audio(inputs, src.device_name, container, s.audio_device)) {
        plan.audio_device = audio->id;
        log(L"audio: \"{}\"{}", audio->name, s.audio_device.empty() ? L" (the card's)" : L" (glasir.ini)");
        return;
    }
    plan.audio_missing = true;
    log(L"no audio input found for \"{}\"{}; audio inputs:", src.device_name,
        s.audio_device.empty() ? L"" : L" named \"" + s.audio_device + L"\"");
    for (const auto &a : inputs) log(L"  {}", a.name);
}

std::wstring capture_end_error(HRESULT ended, const std::wstring &device) {
    if (card_in_use(ended))
        return L"The capture card \"" + device + L"\" is in use by another app (OBS, Discord, the card's own "
               L"app...). Close it and start Glasir again.";
    if (ended == kUnreadableFrames)
        return L"The capture card \"" + device + L"\" sends frames in a layout Glasir cannot read (logs\\glasir.log "
               L"has the details). Set capture.path = directshow in glasir.ini to let mpv read it.";
    return L"The capture card \"" + device + L"\" stopped sending frames (" + hresult_text(ended) +
           L"). Check its cable and that the console is on.";
}

enum class SetupResult { Play, Stop, Failed };

// Downloads what Glasir runs on: when mpv or ReShade is missing, or with
// --setup (which then ends Glasir). Play: go on (with what is there).
SetupResult setup(const Args &args, const std::wstring &dir, const std::wstring &engine) {
    const bool need = !component_installed(engine, Component::Mpv) || !component_installed(engine, Component::ReShade);
    if (!args.setup && !need) return SetupResult::Play;
    const bool dlss5_default = component_installed(engine, Component::Dlss5) || dlss5_gpu(display_gpu());
    std::vector<Component> which;
    if (!args.dialogs) {
        // Nothing is downloaded unasked when no one can be asked.
        if (!args.setup) return SetupResult::Play;
        if (args.components.empty()) {
            which = {Component::Mpv, Component::ReShade};
            if (dlss5_default) which.push_back(Component::Dlss5);
            // Asked for before, with NVIDIA's licences accepted then.
            if (component_installed(engine, Component::Vfg)) which.push_back(Component::Vfg);
        }
        for (size_t at = 0; at < args.components.size();) {
            size_t comma = args.components.find(L',', at);
            if (comma == std::wstring::npos) comma = args.components.size();
            const std::wstring name = args.components.substr(at, comma - at);
            at = comma + 1;
            if (name == L"mpv") which.push_back(Component::Mpv);
            else if (name == L"reshade") which.push_back(Component::ReShade);
            else if (name == L"dlss5") which.push_back(Component::Dlss5);
            else if (name == L"vfg") {
                if (!args.accept_nvidia) {
                    report(L"vfg is NVIDIA Video Frame Generation, under NVIDIA's licences (the NVIDIA Software License "
                           L"Agreement, the Product-Specific Terms for NVIDIA AI Products and the NVIDIA Open Model "
                           L"License, on nvidia.com/en-us/agreements). Add --accept-nvidia-licence if you accept them.");
                    return SetupResult::Failed;
                }
                which.push_back(Component::Vfg);
            } else {
                report(L"Unknown component " + name + L" (mpv, reshade, dlss5 or vfg).");
                return SetupResult::Failed;
            }
        }
    } else {
        SetupChoice choice;
        choice.first_run = !args.setup;
        choice.dlss5_checked = dlss5_default;
        choice.notices_file = dir + L"\\THIRD-PARTY-NOTICES.md";
        const auto chosen = ask_setup(choice);
        if (!chosen) {
            log(L"setup: not now");
            return args.setup || !component_installed(engine, Component::Mpv) ? SetupResult::Stop : SetupResult::Play;
        }
        which = *chosen;
        // Asked for before, with NVIDIA's licences accepted then: kept up to date with the rest
        // (435 MB: only when it is not the version this Glasir was tested with).
        if (component_installed(engine, Component::Vfg) && !component_as_tested(engine, Component::Vfg))
            which.push_back(Component::Vfg);
    }

    std::wstring error;
    if (!run_setup(engine, which, args.newest ? Versions::Newest : Versions::Tested, args.dialogs, error)) {
        if (error != L"cancelled") {
            const std::wstring text = L"Glasir could not download everything: " + sentence(error) +
                                      L"\n\nStart Glasir again to try again. logs\\glasir.log has the details.";
            // Shown now: nothing is held yet but the one-instance lock.
            if (args.dialogs) show(text, MB_ICONERROR);
            else report(text);
        }
        if (args.setup) return error == L"cancelled" ? SetupResult::Stop : SetupResult::Failed;
        return component_installed(engine, Component::Mpv) ? SetupResult::Play : SetupResult::Stop;
    }
    if (args.setup) {
        if (args.dialogs) {
            if (!args.from_launcher) show(L"Glasir is set up. Start it again to play.", MB_ICONINFORMATION);
        } else {
            std::wstring text = L"Installed in " + engine + L":\n";
            for (const auto &line : installed_versions(engine)) text += L"  " + line + L"\n";
            print(text, false);
        }
        return SetupResult::Stop;
    }
    return SetupResult::Play;
}

// What the user changed while playing (volume, boost, delay) is kept in
// glasir.ini for next time. Mute is not: a muted start would be a surprise.
void keep_changes(const MpvWatch &watch, const std::wstring &settings_path) {
    std::vector<SettingValue> values;
    auto keep = [&](const char *property, const wchar_t *key, long lo, long hi) {
        const auto v = watch.changed(property);
        if (!v) return;
        char *end = nullptr;
        const double d = std::strtod(v->c_str(), &end);
        if (end == v->c_str()) return;
        const long n = std::clamp(std::lround(d), lo, hi);
        values.push_back({L"audio", key, std::to_wstring(n)});
        log(L"keeping audio.{} = {} in {}", key, n, settings_path);
    };
    keep("volume", L"volume", 0, 100);
    keep("user-data/glasir/audio-boost", L"boost", 100, 400);
    keep("user-data/glasir/audio-delay", L"delay_ms", 0, 1000);
    if (!values.empty()) save_settings(settings_path, values);
}

// mpv's exit code when the in-game menu asks Glasir to start the picture
// again (after it changed glasir.ini to something that needs the card opened
// in another mode: an 8-bit SDR capture to HDR or Automatic).
constexpr DWORD kRestartCode = 42;
// What the signal turned out to be while Glasir followed it, for the start
// that follows (also kept in glasir.ini for the next time; this does not
// depend on the file being written, or on a --set that says otherwise).
std::optional<bool> g_found_hdr;

// glasir.ini's video.mode. The menu's Picture changes it while playing.
enum class PictureMode { Auto, Sdr, Hdr };

// How a 10-bit capture is shown: Automatic as the signal is found to be;
// HDR with RTX Video HDR likewise, the SDR signal turned into HDR; else as
// the mode says, whatever the picture looks like.
Picture picture_for(PictureMode mode, bool rtx, bool found_hdr) {
    switch (mode) {
    case PictureMode::Auto: return {found_hdr, false};
    case PictureMode::Sdr: return {false, false};
    default: return rtx ? Picture{found_hdr, !found_hdr} : Picture{true, false};
    }
}

// F9 while playing: the frame generation add-on saves the frames on screen,
// then asks for the rest of the report with "script-message
// glasir-bug-report <folder> <why>" (no folder: why there are no frames).
// One report is made at a time; an F9 meanwhile waits for it (the latest).
class InGameReports {
public:
    InGameReports(const std::wstring &dir, const std::wstring &settings, const std::wstring &pipe, MpvProcess &mpv)
        : dir_(dir), settings_(settings), pipe_(pipe), mpv_(mpv) {}
    InGameReports(const InGameReports &) = delete;
    InGameReports &operator=(const InGameReports &) = delete;
    ~InGameReports() {
        if (thread_.joinable()) thread_.join();
    }

    // On MpvWatch's thread.
    void on_message(const std::vector<std::string> &words) {
        if (words.front() != "glasir-bug-report") return;
        Request r{words.size() > 1 ? widen(words[1]) : L"", words.size() > 2 ? words[2] : ""};
        std::lock_guard lock(m_);
        if (running_) {
            pending_ = r;
            return;
        }
        if (thread_.joinable()) thread_.join();  // the last one, finished
        running_ = true;
        try {
            thread_ = std::thread([this, r] { work(r); });
        } catch (...) {
            running_ = false;
        }
    }

private:
    struct Request {
        std::wstring frames;
        std::string why;
    };

    void work(Request r) {
        for (;;) {
            make(r);
            std::lock_guard lock(m_);
            if (!pending_) {
                running_ = false;
                return;
            }
            r = *pending_;
            pending_.reset();
        }
    }

    void make(const Request &r) {
        BugReportInput in;
        in.app_dir = dir_;
        in.settings_path = settings_;
        in.frames = r.frames;
        in.how = L"F9 while playing";
        std::wstring zip, error;
        std::wstring text;
        if (!save_bug_report(in, zip, error)) {
            log(L"bug report: {}", error);
            text = L"Could not save a bug report: " + error;
        } else {
            text = L"Bug report saved in \"Bug reports\"";
            if (r.frames.empty())
                text += r.why == "off"           ? L" (no frames: frame generation is off)"
                        : r.why == "unavailable" ? L" (no frames: frame generation is not running)"
                                                 : L" (the frames could not be saved)";
        }
        // A toast from the overlay (after DLSS 5 and frame generation), not mpv's OSD.
        mpv_.send(pipe_, ipc_command({L"script-message", L"glasir-toast", text, L"4000"}), 2000);
    }

    std::wstring dir_, settings_, pipe_;
    MpvProcess &mpv_;
    std::mutex m_;
    bool running_ = false;
    std::optional<Request> pending_;
    std::thread thread_;
};

// Plays until mpv closes. Returns the exit code; errors go to report().
// Sets stuck if the capture thread would not end (see wWinMain), restart if
// the menu asked to start again (again: this is such a start).
int run(const Args &args, bool again, bool &stuck, bool &restart) {
    const std::wstring dir = app_dir();
    if (again) log(L"starting the picture again");

    // One Glasir at a time, before anything is read or written: one still
    // closing writes glasir.ini and ReShade.ini as it goes, and setup may
    // replace mpv.exe.
    HANDLE quit_event = CreateEventW(nullptr, TRUE, FALSE, kQuitEvent);
    Instance instance(quit_event);
    struct CloseEvent {
        HANDLE h;
        ~CloseEvent() { CloseHandle(h); }
    } close_event{quit_event};
    if (!instance.take()) {
        report(L"Another Glasir is running and did not close.");
        return 1;
    }

    const std::wstring engine = dir + L"\\engine";
    switch (setup(args, dir, engine)) {
    case SetupResult::Play: break;
    case SetupResult::Stop: return 0;
    case SetupResult::Failed: return 1;
    }
    for (const auto &line : installed_versions(engine)) log(L"{}", line);
    for (Component c : {Component::Mpv, Component::ReShade, Component::Dlss5, Component::Vfg})
        if (component_installed(engine, c) && !component_as_tested(engine, c))
            log(L"{} is not the version this Glasir was tested with (Update in Glasir's window installs it)",
                component_name(c));

    Settings s;
    std::wstring error;
    const std::wstring settings_path = args.settings_path.empty() ? dir + L"\\glasir.ini" : args.settings_path;
    if (!load_settings(settings_path, s, error)) {
        report(error);
        return 2;
    }
    for (const auto &o : args.overrides)
        if (!apply_override(s, o, error)) {
            report(error);
            return 2;
        }
    if (g_found_hdr) s.auto_found_hdr = *g_found_hdr;
    // As glasir.ini says; open_source makes s.hdr what the frames are shown as.
    PictureMode mode = s.hdr_auto ? PictureMode::Auto : s.hdr ? PictureMode::Hdr : PictureMode::Sdr;

    MpvPlan plan;
    plan.mpv_exe = engine + L"\\mpv.exe";
    plan.config_dir = engine + L"\\portable_config";
    plan.screenshot_dir = dir + L"\\Screenshots";
    plan.ipc_pipe = kIpcPipe;
    if (s.mpv_log) plan.log_file = dir + L"\\logs\\mpv.log";
    if (!file_exists(plan.mpv_exe)) {
        report(L"mpv.exe is missing from " + engine +
               L". Start Glasir without --no-dialogs, or run \"Glasir.exe --setup\", to download it.");
        return 1;
    }
    CreateDirectoryW(plan.screenshot_dir.c_str(), nullptr);
    const GpuInfo gpu = display_gpu();
    plan.vsr = vsr_kind(gpu);
    if (reshade_installed(engine)) {
        // DLSS 5: auto runs it on an RTX 30, 40 or 50 GPU; on tries any GPU.
        const bool installed = dlss5_installed(engine);
        const bool loaded = installed && (s.dlss5 == Settings::Dlss5::On || dlss5_gpu(gpu));
        const bool dlss5 = loaded && s.dlss5 != Settings::Dlss5::Off;
        write_reshade_config(engine, s, loaded, dlss5);
        log(L"ReShade: on; frame generation {}{}", s.framegen ? L"on" : L"off",
            s.framegen && !s.dedup ? L" but needs dedup = 1: it stays off" : L"");
        if (s.framegen && s.framegen_nvidia)
            log(L"frame generation by NVIDIA Video Frame Generation{}",
                !component_installed(engine, Component::Vfg)
                    ? L": not downloaded (Glasir's window offers it), Glasir's own makes the frames"
                    : !vfg_gpu(gpu) ? L": it needs an RTX 40 or 50 series GPU, Glasir's own makes the frames" : L"");
        std::wstring why;
        if (!installed)
            why = L" (not downloaded; \"Glasir.exe --setup\" offers it)";
        else if (loaded && !dlss5)
            why = L" (glasir.ini; the menu can switch it on)";
        else if (!dlss5)
            why = L" (needs an RTX 30, 40 or 50 series GPU; [dlss5] enabled = on tries anyway)";
        log(L"DLSS 5: {}{}; GPU: {}", dlss5 ? L"on" : L"off", why, gpu.found ? gpu.name : L"unknown");
    } else {
        log(L"ReShade is not in {}: no frame generation or DLSS 5", engine);
    }

    // Declared in this order so they end in reverse: the watcher before mpv,
    // mpv before the card, the card before the one-instance lock.
    Source src;
    if (!open_source(args, s, src)) return 1;
    plan.raw = src.raw;
    plan.layout = src.layout;
    plan.fps = src.fps;
    plan.dshow_device = src.dshow_device;
    plan.dshow_mode = src.dshow_mode;
    plan.audio_log = dir + L"\\logs\\audio.log";
    if (s.audio) choose_audio(s, src, plan);

    // A 10-bit capture carries SDR or HDR alike, so what it is shown as can
    // change while playing: with the console's signal, or in the menu.
    const bool live = src.raw && src.layout.format == PixelFormat::P010;
    // RTX Video HDR: NVIDIA RTX only (the video processor that also does VSR).
    const bool rtx_offered = plan.vsr == L"nvidia";
    bool rtx = s.rtx_hdr && rtx_offered;
    if (live && mode == PictureMode::Hdr && rtx) {
        // HDR on the screen whatever the console sends. It starts as the
        // signal was the last time, as Automatic does.
        const Picture p = picture_for(mode, rtx, s.auto_found_hdr);
        s.hdr = p.hdr;
        plan.rtx_hdr = p.rtx_hdr;
    }

    // The in-game menu keeps what it changes in the same file.
    SetEnvironmentVariableW(L"GLASIR_SETTINGS", settings_path.c_str());
    SetEnvironmentVariableW(L"GLASIR_VIDEO_MODE", s.hdr ? L"hdr" : L"sdr");  // the signal, as shown to start with
    SetEnvironmentVariableW(L"GLASIR_PICTURE",
                            mode == PictureMode::Auto ? L"auto" : mode == PictureMode::Hdr ? L"hdr" : L"sdr");
    SetEnvironmentVariableW(L"GLASIR_PICTURE_LIVE", live ? L"1" : L"0");
    SetEnvironmentVariableW(L"GLASIR_RTX_HDR", !rtx_offered ? L"none" : s.rtx_hdr ? L"on" : L"off");
    SetEnvironmentVariableW(L"GLASIR_HDR_AVAILABLE", src.hdr_available ? L"1" : L"0");
    SetEnvironmentVariableW(L"GLASIR_WINDOW_MODE",
                            s.fullscreen ? L"fullscreen" : s.borderless ? L"borderless" : L"windowed");
    MpvProcess mpv;
    if (!mpv.start(join_command_line(plan.mpv_exe, mpv_arguments(plan, s)), plan.raw, error)) {
        report(error);
        return 1;
    }
    // mpv's window: full screen or a window (the menu switches it), and
    // full screen without the black bars (backdrop.h).
    Backdrop backdrop;  // after changes stops: it calls this
    {
        WindowSetup w;
        w.borderless = s.borderless;
        w.fullscreen = s.fullscreen;
        w.skip_bars = s.skip_black_bars;
        w.width = plan.raw ? plan.layout.width : (plan.dshow_mode ? plan.dshow_mode->width : s.width);
        w.height = plan.raw ? plan.layout.height : (plan.dshow_mode ? plan.dshow_mode->height : s.height);
        w.mpv_command = [&mpv, pipe = plan.ipc_pipe](const std::vector<std::wstring> &words) {
            mpv.send(pipe, ipc_command(words), 1000);
        };
        backdrop.start(mpv.pid(), std::move(w));
    }
    int rc = 0;
    bool quit_asked = false;      // another Glasir asked this one to close: no restart, whatever else asks
    const bool found_before = s.auto_found_hdr;
    // The menu's Picture and its RTX HDR switch, for the loop that passes
    // the frames on (-1: nothing asked).
    std::atomic<int> asked_mode{-1}, asked_rtx{-1};
    InGameReports reports(dir, settings_path, plan.ipc_pipe, mpv);  // after changes stops: it calls this
    MpvWatch changes;
    changes.on_message = [&reports, &backdrop, &asked_mode, &asked_rtx](const std::vector<std::string> &words) {
        if (words.front() == "glasir-window" && words.size() > 1 && words[1] == "again")
            backdrop.place_again();  // mpv left its own full screen (glasir_window.lua)
        else if (words.front() == "glasir-window" && words.size() > 1)
            backdrop.set_borderless(words[1] == "borderless");
        else if (words.front() == "glasir-picture" && words.size() > 1)
            asked_mode = int(words[1] == "auto" ? PictureMode::Auto : words[1] == "hdr" ? PictureMode::Hdr : PictureMode::Sdr);
        else if (words.front() == "glasir-rtx-hdr" && words.size() > 1)
            asked_rtx = words[1] == "on" ? 1 : 0;
        else
            reports.on_message(words);
    };
    {
        QuitWatcher watcher(quit_event, mpv);
        const std::wstring target = mpv_load_target(plan);
        log(L"mpv is loading {}", target);
        bool loaded = false;
        std::wstring why;
        if (plan.raw) {
            // Raw frames: mpv opens its standard input at once; the frames
            // that follow tell whether it plays.
            loaded = mpv.send(plan.ipc_pipe, ipc_command({L"loadfile", target, L"replace"}), 15000);
        } else {
            // DirectShow: mpv opens the card itself and says whether it could.
            const auto result = mpv.load(plan.ipc_pipe, target, 15000, why);
            loaded = result == MpvProcess::LoadResult::Loaded;
            if (loaded) log(L"mpv opened the card");
            if (result == MpvProcess::LoadResult::NoAnswer && !mpv.exited())
                why = L"no answer in 15 s";
        }
        if (!loaded && !watcher.requested) {
            if (!why.empty())
                report(L"mpv could not open the capture card \"" + plan.dshow_device + L"\" (" + why + L"). " +
                       kAnotherApp);
            else
                report(mpv.exited() ? std::format(L"mpv closed while starting (exit code {}). Set debug.mpv_log = 1 "
                                                  L"in glasir.ini and look at logs\\mpv.log.",
                                                  mpv.exit_code())
                                    : std::wstring(L"mpv did not answer while starting."));
            rc = 1;
        }

        // Follows what the user changes while playing.
        if (loaded && !changes.start(mpv, plan.ipc_pipe,
                                     {"volume", "user-data/glasir/audio-boost", "user-data/glasir/audio-delay"}))
            log(L"could not follow mpv's volume: changes while playing are not kept");

        if (!loaded) {
            mpv.stop(plan.ipc_pipe, 1000);
        } else if (!plan.raw) {
            // mpv reads the card itself; Glasir stays to keep one instance.
            mpv.exited(INFINITE);
        } else {
            const size_t frame_bytes = src.layout.bytes();
            log(L"passing {} {}x{} @ {:.3f} fps from \"{}\" to mpv", format_name(src.layout.format),
                src.layout.width, src.layout.height, src.fps, src.device_name);

            // What the capture thread uses, kept alive by the thread itself:
            // if a driver read never returns, the thread is left behind.
            struct Pump {
                explicit Pump(size_t bytes) : slot(bytes) {}
                FrameSlot slot;
                std::atomic<UINT64> frames{0};
                std::atomic<HRESULT> status{S_OK};
                std::unique_ptr<RepeatFilter> repeats;
                std::unique_ptr<SignalMonitor> signal;
                FrameLayout layout;
                ComPtr<IMFSourceReader> reader;
                HANDLE raw_input = nullptr;
            };
            auto pump = std::make_shared<Pump>(frame_bytes);
            pump->layout = src.layout;
            pump->reader = src.reader;
            pump->raw_input = src.raw_input;
            HANDLE link_map = nullptr;
            GlasirFrameGenLink *link = nullptr;
            if (s.dedup) {
                pump->repeats = std::make_unique<RepeatFilter>(src.layout.format, src.layout.width,
                                                               src.layout.height, s.dedup_threshold,
                                                               s.dedup_keepalive_ms);
                link = create_link(mpv.pid(), link_map);
                pump->slot.set_link(link);
                log(L"dedup on: threshold {}, keepalive {} ms, frame generation link {}", s.dedup_threshold,
                    s.dedup_keepalive_ms, link ? L"open" : L"unavailable");
            }
            Picture shown{s.hdr, plan.rtx_hdr};
            auto follows = [&] { return mode == PictureMode::Auto || (mode == PictureMode::Hdr && rtx); };
            auto picture_text = [](const Picture &p) {
                return p.hdr ? L"HDR" : p.rtx_hdr ? L"SDR in HDR (NVIDIA RTX Video HDR)" : L"SDR";
            };
            if (live) {
                // The signal is watched in every mode: it is there to follow
                // as soon as the menu asks for that.
                pump->signal = std::make_unique<SignalMonitor>(s.hdr ? Signal::Hdr : Signal::Sdr, src.layout.width,
                                                               src.layout.height);
                log(L"picture: shown as {}{}", picture_text(shown),
                    follows() ? L" until the picture says otherwise" : L"");
            }
            std::thread reader([pump] {
                const FrameSinks sinks{pump->repeats.get(), pump->signal.get()};
                if (pump->raw_input)
                    raw_loop(pump->raw_input, pump->layout, pump->slot, sinks, pump->frames, pump->status);
                else
                    capture_loop(pump->reader.Get(), pump->layout, pump->slot, sinks, pump->frames, pump->status);
            });
            FrameSlot &slot = pump->slot;

            UINT64 sent = 0;
            bool stalled = false;
            BehindLog behind;
            ULONGLONG switch_after = 0;  // a switch mpv did not take is tried again then
            bool by_menu = false;        // the switch to make is the menu's (not said in a toast)
            for (;;) {
                const std::vector<BYTE> *frame = slot.take(1000);
                if (!frame) {
                    if (slot.stopped() || mpv.exited()) break;
                    if (!stalled) log(L"no frames from the source");
                    stalled = true;
                    continue;
                }
                stalled = false;
                if (!mpv.write_frame(frame->data(), frame_bytes)) break;
                ++sent;
                behind.check(slot);
                if (!pump->signal) continue;
                // The console went from SDR to HDR or back (or the first
                // guess was wrong), or the menu asks for another picture:
                // mpv shows the frames as that from here on. The capture,
                // mpv, DLSS 5 and frame generation keep running.
                if (const int m = asked_mode.exchange(-1); m >= 0) mode = PictureMode(m), by_menu = true;
                if (const int r = asked_rtx.exchange(-1); r >= 0) rtx = r == 1 && rtx_offered, by_menu = true;
                const bool found_hdr = pump->signal->found.load() == Signal::Hdr;
                // glasir.ini is written once mpv is gone (below): the menu
                // in mpv may be writing it now.
                if (follows()) g_found_hdr = found_hdr;
                const Picture want = picture_for(mode, rtx, found_hdr);
                if (want == shown) {
                    by_menu = false;
                    continue;
                }
                if (GetTickCount64() < switch_after) continue;
                std::string commands = picture_commands(plan, s, want);
                // The menu shows its own choice; a change of the console's is said.
                if (!by_menu)
                    commands += ipc_command(
                        {L"script-message", L"glasir-toast",
                         want.hdr       ? L"HDR signal: showing HDR"
                         : want.rtx_hdr ? L"SDR signal: showing it in HDR (RTX Video HDR)"
                                        : L"SDR signal: showing SDR",
                         L"4000"});
                if (!mpv.send(plan.ipc_pipe, commands, 500)) {
                    switch_after = GetTickCount64() + 1000;
                    continue;
                }
                log(L"picture: {} now ({})", picture_text(want),
                    by_menu ? L"the menu" : found_hdr ? L"the signal is HDR" : L"the signal is SDR");
                shown = want;
                by_menu = false;
            }
            log(L"{} frames sent to mpv", sent);

            const bool mpv_gone = mpv.exited();
            mpv.close_stdin();
            mpv.stop(plan.ipc_pipe, src.raw_input ? 1000 : 2000);

            // Stop reading. A raw ReadFile is cancelled until the thread is
            // out. A card read is ended by flushing the reader (that returns
            // a waiting ReadSample at once; shutting the source down does
            // not while frames flow). The flush runs on its own thread so a
            // driver that never answers cannot hold Glasir; shutting the
            // source down is the last try.
            slot.stop();
            HANDLE thread = reader.native_handle();
            if (src.raw_input) {
                for (int i = 0; i < 60 && WaitForSingleObject(thread, 50) == WAIT_TIMEOUT; ++i)
                    CancelSynchronousIo(thread);
            } else if (src.reader) {
                std::thread([r = src.reader] {
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    r->Flush(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS));
                    if (SUCCEEDED(com)) CoUninitialize();
                }).detach();
                if (WaitForSingleObject(thread, 3000) == WAIT_TIMEOUT && src.source) {
                    log(L"the capture thread did not stop after a flush: shutting the source down");
                    src.source->Shutdown();
                }
            }
            if (WaitForSingleObject(thread, 2000) == WAIT_TIMEOUT) {
                // The driver will not give the read back. Leave the thread
                // (it holds what it uses); wWinMain ends the process.
                log(L"the capture thread did not end; Glasir ends without waiting for it");
                reader.detach();
                stuck = true;
            } else {
                reader.join();
            }
            log(L"{} frames captured", pump->frames.load());
            if (link) {
                log(L"frame generation link: {} new frames, {} dropped while mpv was busy",
                    static_cast<long long>(link->published), static_cast<long long>(link->dropped));
                if (!stuck) UnmapViewOfFile(link);  // the thread may still write to it
            }
            if (link_map && !stuck) CloseHandle(link_map);

            const HRESULT ended = pump->status.load();
            if (!mpv_gone && !watcher.requested && ended != S_OK) {
                if (src.raw_input) {
                    log(L"raw input finished");
                } else {
                    rc = 1;
                    report(capture_end_error(ended, src.device_name));
                }
            }
        }
    }
    // The quit event stays set until the next Glasir takes the lock.
    quit_asked = WaitForSingleObject(quit_event, 0) == WAIT_OBJECT_0;
    log(L"mpv exited (code {})", mpv.exit_code());
    changes.stop();
    keep_changes(changes, settings_path);
    if (g_found_hdr && *g_found_hdr != found_before)
        save_settings(settings_path, {{L"video", L"auto_found", *g_found_hdr ? L"hdr" : L"sdr"}});
    if (quit_asked && mpv.exit_code() == kRestartCode)
        log(L"not starting the picture again: another Glasir asked this one to close");
    restart = mpv.exit_code() == kRestartCode && rc == 0 && !quit_asked;
    return rc;
}

std::wstring settings_file(const Args &args) {
    return args.settings_path.empty() ? app_dir() + L"\\glasir.ini" : args.settings_path;
}

bool launcher_wanted(const Args &args) {
    if (!args.dialogs || args.setup || args.play || !args.input_raw.empty()) return false;
    Settings s;
    std::wstring error;
    load_settings(settings_file(args), s, error);  // an error is shown when playing
    return s.show_launcher || (GetAsyncKeyState(VK_SHIFT) & 0x8000);
}

// glasir.log becomes glasir-previous.log; if it cannot be renamed (open in
// an editor), it is copied, so the last session is kept either way.
void keep_previous_log(const std::wstring &log_path, const std::wstring &previous) {
    if (MoveFileExW(log_path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
    std::string text;
    if (!read_shared(log_path, text)) return;
    HANDLE f = CreateFileW(previous.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text.data(), DWORD(text.size()), &written, nullptr);
    CloseHandle(f);
}

// Glasir.exe's arguments, without the program: for starting it again.
std::vector<std::wstring> own_arguments() {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> out;
    for (int i = 1; i < argc; ++i) out.push_back(argv[i]);
    LocalFree(argv);
    return out;
}

// Set once an update is installed: the new Glasir.exe is started with this
// command line as the old one ends (wWinMain).
std::wstring g_start_after;

// Installs a newer Glasir (with the one-instance lock: a Glasir playing
// closes first). zip: from disk instead of the release's download.
bool install_glasir(const Args &args, const Release &release, const std::wstring &zip, std::wstring &error) {
    const std::wstring dir = app_dir();
    HANDLE quit_event = CreateEventW(nullptr, TRUE, FALSE, kQuitEvent);
    bool ok = false;
    {
        Instance instance(quit_event);
        if (!instance.take()) {
            error = L"another Glasir is running and did not close";
        } else {
            ok = run_with_progress(
                L"Glasir update",
                [&](SetupProgress &progress, const std::atomic<bool> &cancel, std::wstring &e) {
                    return zip.empty() ? install_update(dir, release, progress, cancel, e)
                                       : install_update_zip(dir, zip, release.version, progress, cancel, e);
                },
                args.dialogs, error);
        }
    }
    CloseHandle(quit_event);
    if (!ok) log(L"update: not installed: {}", error);
    return ok;
}

// --update and --update-from.
int update_command(const Args &args) {
    Release release;
    std::wstring error;
    int now[3];
    current_version(now);
    if (!args.update_from.empty()) {
        // The version is in the release zip's name: Glasir-x.y.z.zip.
        const std::wstring name = args.update_from.substr(args.update_from.find_last_of(L"\\/") + 1);
        const std::wstring stem =
            name.size() > 11 && name.rfind(L"Glasir-", 0) == 0 ? name.substr(7, name.size() - 11) : L"";
        if (!parse_version(stem, release.version)) {
            print(L"--update-from needs a Glasir-x.y.z.zip.\n", args.dialogs);
            return 2;
        }
    } else if (!latest_release(update_repo(), release, error)) {
        print(L"Could not look for an update: " + sentence(error) + L"\n", args.dialogs);
        return 1;
    } else if (!newer(release.version, now)) {
        print(L"Glasir " + version_text(now) + L" is the newest.\n", args.dialogs);
        return 0;
    }
    if (!install_glasir(args, release, args.update_from, error)) {
        if (error != L"cancelled")
            print(L"Glasir could not update: " + sentence(error) + L" The version you have is unchanged.\n",
                  args.dialogs);
        return 1;
    }
    print(L"Glasir " + version_text(release.version) + L" is installed. Start Glasir again.\n", args.dialogs);
    return 0;
}

// A bug report without frames (the launcher's link, --bug-report).
bool bug_report_now(const Args &args, std::wstring &zip, std::wstring &error, const wchar_t *how) {
    BugReportInput in;
    in.app_dir = app_dir();
    in.settings_path = settings_file(args);
    in.how = how;
    in.extra = L"Capture cards:\n" + device_list_text();
    return save_bug_report(in, zip, error);
}

void show_in_explorer(const std::wstring &file) {
    const std::wstring select = L"/select,\"" + file + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
}

// The launcher's download of NVIDIA Video Frame Generation: NVIDIA's licences
// first, and nothing is downloaded unless they are accepted.
void setup_vfg(const std::wstring &engine) {
    if (!ask_vfg_licence()) {
        log(L"setup: NVIDIA Video Frame Generation: its licences were not accepted");
        return;
    }
    log(L"setup: NVIDIA Video Frame Generation: its licences were accepted in the window");
    std::wstring error;
    if (!run_setup(engine, {Component::Vfg}, Versions::Tested, true, error) && error != L"cancelled")
        show(L"Glasir could not download NVIDIA Video Frame Generation: " + sentence(error) +
                 L"\n\nGlasir's own frame generation is used until then. logs\\glasir.log has the details.",
             MB_ICONERROR);
}

// The launcher until Play (true) or the window is closed (false). Its
// Download/Update buttons run setup, as --setup does, and come back; its
// update offer installs the newer Glasir, which then starts in its place.
bool launcher(const Args &args, UpdateCheck &update) {
    const std::wstring dir = app_dir();
    {
        Settings s;
        std::wstring error;
        load_settings(settings_file(args), s, error);
        if (s.update_check) update.start();
    }
    LauncherExtras extras;
    extras.update = &update;
    extras.bug_report = [&args](std::wstring &zip, std::wstring &error) {
        return bug_report_now(args, zip, error, L"from the launcher");
    };
    for (;;) {
        const LauncherAction action = run_launcher(settings_file(args), dir + L"\\engine", extras);
        if (action == LauncherAction::Play || action == LauncherAction::Quit) {
            log(L"launcher: {}", action == LauncherAction::Play ? L"play" : L"closed");
            return action == LauncherAction::Play;
        }
        if (action == LauncherAction::SelfUpdate) {
            std::wstring error;
            if (install_glasir(args, update.release(), L"", error)) {
                g_start_after = join_command_line(dir + L"\\Glasir.exe", own_arguments());
                return false;
            }
            if (error != L"cancelled")
                show(L"Glasir could not update: " + sentence(error) +
                         L"\n\nThe version you have is unchanged. logs\\glasir.log has the details.",
                     MB_ICONERROR);
            continue;
        }
        Args setup_args = args;
        setup_args.setup = setup_args.from_launcher = true;
        HANDLE quit_event = CreateEventW(nullptr, TRUE, FALSE, kQuitEvent);
        {
            Instance instance(quit_event);  // a Glasir playing closes first: setup replaces its files
            if (instance.take()) {
                if (action == LauncherAction::SetupVfg) setup_vfg(dir + L"\\engine");
                else setup(setup_args, dir, dir + L"\\engine");
            }
        }
        CloseHandle(quit_event);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Args args;
    std::wstring error;
    if (!parse_args(args, error)) {
        print(error + L"\n", true);
        return 2;
    }
    if (args.version) {
        print(std::format(L"{} {} ({})\n", widen(GLASIR_NAME), widen(GLASIR_VERSION), widen(GLASIR_GIT_DESCRIBE)),
              args.dialogs);
        return 0;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        if (args.dialogs)
            show(L"Media Foundation could not start. Is this an N edition of Windows without the Media Feature Pack?",
                 MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    int rc = 0;
    bool stuck = false;
    UpdateCheck update;  // the launcher's; ends with Glasir, so Play never waits for it
    if (args.list) {
        print(device_list_text(), args.dialogs);
    } else {
        const std::wstring dir = app_dir();
        CreateDirectoryW((dir + L"\\logs").c_str(), nullptr);
        // The last start's log is kept beside this one (for a bug report).
        const std::wstring log_path = dir + L"\\logs\\glasir.log";
        keep_previous_log(log_path, dir + L"\\logs\\glasir-previous.log");
        log_open(log_path);
        log(L"{} {} ({})", widen(GLASIR_NAME), widen(GLASIR_VERSION), widen(GLASIR_GIT_DESCRIBE));
        finish_update(dir);
        bool play = true;
        if (args.update || !args.update_from.empty()) {
            rc = update_command(args);
            play = false;
        } else if (args.bug_report) {
            std::wstring zip;
            if (bug_report_now(args, zip, error, L"Glasir.exe --bug-report")) {
                if (args.dialogs) show_in_explorer(zip);
                else print(L"Bug report saved: " + zip + L"\n", false);
            } else {
                print(L"Could not save a bug report: " + error + L"\n", args.dialogs);
                rc = 1;
            }
            play = false;
        } else if (launcher_wanted(args)) {
            play = launcher(args, update);
        }
        if (play)
            for (bool again = false, restart = true; restart && !stuck; again = true) {
                restart = false;
                rc = run(args, again, stuck, restart);
            }
    }
    if (!g_error.empty()) {
        if (args.dialogs) show(g_error, MB_ICONERROR);
        else print(L"Glasir: " + g_error + L"\n", false);  // to a console or pipe, if there is one
    }
    if (stuck) {
        // A driver read that never returned: shutting Media Foundation down
        // could wait on it too. Everything else is already released.
        log_close();
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));
    }
    MFShutdown();
    CoUninitialize();
    log_close();
    if (!g_start_after.empty()) {
        // The updated Glasir, in this one's place (it waits for this one's
        // lock, and deletes the old Glasir.exe).
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring command = g_start_after;
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else if (args.dialogs) {
            show(std::format(L"Glasir is updated, but could not start again (error {}). Start it yourself.",
                             GetLastError()),
                 MB_ICONINFORMATION);
        }
    }
    return rc;
}
