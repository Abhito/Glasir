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
//   Glasir.exe [--settings FILE] [--set section.key=value ...] [--no-dialogs]
//   Glasir.exe --input-raw FILE|- [--raw-format nv12|p010|yuy2|uyvy] --set capture.width=W ...
//   Glasir.exe --list
//   Glasir.exe --version
//
// --input-raw reads tightly packed frames from a file or standard input
// instead of the card (capture.width, capture.height and capture.fps must be
// set), for testing the same path without one.

#include <windows.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../common/framegen_link.h"
#include "capture.h"
#include "devices.h"
#include "glasir_version.h"
#include "log.h"
#include "mpv_launch.h"
#include "settings.h"

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
    bool list = false;
    bool version = false;
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

bool file_exists(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
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
    // Media Foundation
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    std::wstring device_name;
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
bool open_source(const Args &args, const Settings &s, Source &src) {
    if (!args.input_raw.empty()) {
        if (!s.width || !s.height || s.fps <= 0) {
            report(L"--input-raw needs capture.width, capture.height and capture.fps.");
            return false;
        }
        if (!parse_raw_format(args.raw_format, s.hdr, src.layout.format)) {
            report(L"Unknown --raw-format " + args.raw_format + L" (nv12, p010, yuy2 or uyvy).");
            return false;
        }
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
        return true;
    }

    ModeRequest request;
    request.hdr = s.hdr;
    request.width = s.width;
    request.height = s.height;
    request.fps = s.fps;
    request.max_auto_height = UINT32(GetSystemMetrics(SM_CYSCREEN));

    // The card list also guides the DirectShow path: which device is the
    // card, and which mode to ask for.
    const std::vector<CaptureDevice> devices = list_devices();
    ModeRequest any = request;
    any.supported_only = false;
    const CaptureDevice *device = nullptr;
    if (s.path != Settings::CapturePath::DirectShow) {
        device = choose_device(devices, s.device, request);
        if (device) {
            if (auto mode = choose_mode(device->modes, request)) {
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

// Plays until mpv closes. Returns the exit code; errors go to report().
// Sets stuck if the capture thread would not end (see wWinMain).
int run(const Args &args, bool &stuck) {
    const std::wstring dir = app_dir();
    CreateDirectoryW((dir + L"\\logs").c_str(), nullptr);
    log_open(dir + L"\\logs\\glasir.log");
    log(L"{} {} ({})", widen(GLASIR_NAME), widen(GLASIR_VERSION), widen(GLASIR_GIT_DESCRIBE));

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

    const std::wstring engine = dir + L"\\engine";
    MpvPlan plan;
    plan.mpv_exe = engine + L"\\mpv.exe";
    plan.config_dir = engine + L"\\portable_config";
    plan.screenshot_dir = dir + L"\\Screenshots";
    plan.ipc_pipe = kIpcPipe;
    if (s.mpv_log) plan.log_file = dir + L"\\logs\\mpv.log";
    if (!file_exists(plan.mpv_exe)) {
        report(L"mpv.exe is missing from " + engine + L". Run the Glasir installer to download it.");
        return 1;
    }
    CreateDirectoryW(plan.screenshot_dir.c_str(), nullptr);

    HANDLE quit_event = CreateEventW(nullptr, TRUE, FALSE, kQuitEvent);
    Instance instance(quit_event);
    if (!instance.take()) {
        report(L"Another Glasir is running and did not close.");
        CloseHandle(quit_event);
        return 1;
    }

    // Declared in this order so they end in reverse: the watcher before mpv,
    // mpv before the card, the card before the one-instance lock.
    Source src;
    if (!open_source(args, s, src)) {
        CloseHandle(quit_event);
        return 1;
    }
    plan.raw = src.raw;
    plan.layout = src.layout;
    plan.fps = src.fps;
    plan.dshow_device = src.dshow_device;
    plan.dshow_mode = src.dshow_mode;

    MpvProcess mpv;
    if (!mpv.start(join_command_line(plan.mpv_exe, mpv_arguments(plan, s)), plan.raw, error)) {
        report(error);
        CloseHandle(quit_event);
        return 1;
    }
    int rc = 0;
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
            std::thread reader([pump] {
                if (pump->raw_input)
                    raw_loop(pump->raw_input, pump->layout, pump->slot, pump->repeats.get(), pump->frames,
                             pump->status);
                else
                    capture_loop(pump->reader.Get(), pump->layout, pump->slot, pump->repeats.get(), pump->frames,
                                 pump->status);
            });
            FrameSlot &slot = pump->slot;

            UINT64 sent = 0;
            bool stalled = false;
            BehindLog behind;
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
    log(L"mpv exited (code {})", mpv.exit_code());
    CloseHandle(quit_event);
    return rc;
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
    if (args.list)
        print(device_list_text(), args.dialogs);
    else
        rc = run(args, stuck);
    if (!g_error.empty() && args.dialogs) show(g_error, MB_ICONERROR);
    if (stuck) {
        // A driver read that never returned: shutting Media Foundation down
        // could wait on it too. Everything else is already released.
        log_close();
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));
    }
    MFShutdown();
    CoUninitialize();
    log_close();
    return rc;
}
