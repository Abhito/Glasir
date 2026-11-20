// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// zz-glasir-framegen.addon64 - frame generation inside mpv for Glasir.
//
// A ReShade add-on that runs last, after the DLSS 5 pass has drawn into mpv's
// back buffer. For every real frame B it keeps a copy and draws the frame in
// between the previous real frame A and B over it, which mpv's own present
// then shows; half a source frame after the GPU finished that frame it puts B
// back and presents it through the original swap chain (FrameGen below has the
// details). A 30 fps source
// reaches the screen at 60, a 60 fps source at 120 - when the display can show
// it.
//
// The frame in between is motion-compensated (fg_interp.h, flow.hlsl; the
// design and measurements are in README.md); Method=blend mixes A and B 50/50.
// Method=vfg has NVIDIA's Video Frame Generation make it instead (fg_vfg.h),
// where its DLLs are installed and the GPU is an RTX 40 or 50; where not, or
// when it fails, the motion-compensated one takes over.
//
// It only makes sense when mpv gets real frames, not the pairs of repeats a 30
// fps game captured at 60 arrives as. Glasir's dedup sends only new
// frames and opens a link to this add-on (framegen_link.h) with when each
// one arrived and how many mpv had to drop; without that link the add-on stays
// off unless RequireDedupLauncher=0.
//
// Settings live in ReShade.ini, section [GLASIR_FRAMEGEN]; F8 toggles it.

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wincodec.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <reshade.hpp>

#include "fg_interp.h"
#include "fg_sr.h"
#include "fg_resample.h"
#include "fg_vfg.h"
#include "framegen_link.h"
#include "nv_low_latency.h"
#include "overlay.h"
#include "reshade_ui.h"

using Microsoft::WRL::ComPtr;

extern "C" __declspec(dllexport) const char *NAME = "Glasir Frame Generation";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Doubles the frame rate of Glasir by presenting a generated frame between each pair of real frames.";

namespace {

constexpr const char *kSection = "GLASIR_FRAMEGEN";

void log_line(reshade::log::level level, const char *fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    reshade::log::message(level, text);
}

// ---- Settings ----------------------------------------------------------------

struct Settings {
    bool enabled = true;
    bool require_dedup = true;  // only run with the link from Glasir's dedup
    fg::Method method = fg::Method::motion;  // motion = motion-compensated, blend = 50/50 mix
    bool fast = false;          // Quality=fast: fewer candidates per pixel, about half the drawing cost
    bool marker = false;        // magenta square on generated frames
    bool log_stats = true;      // a line in ReShade.log every 5 s
    int max_output_hz = 0;      // 0 = the display's refresh rate
    int reset_ms = 100;         // a longer gap between real frames starts over
    int toggle_key = VK_F8;
    int capture_key = VK_F9;    // saves the frames around this moment (Capture below)
    bool dedup_when_off = false;  // 1: mpv gets new frames only while frame generation is off too
    // The switches (the panel has them; switch_setting below for how they are kept).
    bool picture_only = true;     // with black bars around the picture, the generator works on the picture alone
    bool edge_rules = true;       // the strip along the frame's edges that only one frame shows in a camera turn
    bool still_hud = true;        // a HUD over fast scenery is held where it is
    bool history_search = false;  // a test: the search also tries what each block did over the last pair
    // Method=vfg: NVIDIA's Video Frame Generation makes the frame in between (fg_vfg.h); `method` is
    // then what takes over where it cannot run.
    bool vfg = false;
    fg::VfgMode vfg_mode = fg::VfgMode::medium;  // VfgQuality=low|medium|high: its model's size
    // VfgQuality=auto: the add-on picks the model, the largest that keeps up, and picks again as the
    // load changes (fg_vfg_auto.h); vfg_mode is then the one in use now.
    bool vfg_auto = false;
    bool vfg_auto_cut = true;   // VfgAutoCut: its own cut detection (frame A again across a cut)
    std::string vfg_runtime;    // VfgRuntime: the folder with its DLLs; empty: nvidia-vfg beside mpv.exe
    // Multiplier=2|3|4: frames shown for each of the source's - one, two or three of them generated -
    // or as many as the display can show. 3 and 4 are there for whoever wants them, as they are.
    int multiplier = 2;
    // SrQuality=low|medium|high|ultra: the model of NVIDIA's Video Super Resolution (Upscale below;
    // glasir.ini video.scaling = nvidia, scaling_quality).
    fg::SrLevel sr_level = fg::SrLevel::medium;
    // LowLatency=off|on|boost: NVIDIA Reflex's low latency mode on mpv's device (LowLatency below).
    int low_latency = 0;
    // GsyncSeen=1: the NVIDIA driver has run G-SYNC on mpv's window on this PC (written the first time it
    // does, LowLatency::watch): glasir_scaling.lua then has mpv's output made anew when RTX Video Super
    // Resolution is left, which is what brings G-SYNC back.
    bool gsync_seen = false;
    std::string mpv_pipe = "glasir";
};

Settings g_settings;
const Settings kDefaults;

// Every value is read as text and parsed here: ReShade's own bool and int
// readers take neither "true" nor "0x77". A missing key gets the default
// written back so it shows up in ReShade.ini; a value that does not parse is
// left alone and logged.
bool read_text(const char *key, std::string &out) {
    char text[256] = {};
    size_t size = sizeof(text);
    if (!reshade::get_config_value(nullptr, kSection, key, text, &size)) return false;
    out = text;
    while (!out.empty() && isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
    while (!out.empty() && isspace(static_cast<unsigned char>(out.front()))) out.erase(out.begin());
    return true;
}

void bad_setting(const char *key, const std::string &text) {
    log_line(reshade::log::level::warning, "[%s] %s=%s is not a value this add-on understands; using the default",
             kSection, key, text.c_str());
}

bool parse_bool(std::string text, bool &value) {
    for (auto &c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (text == "1" || text == "true" || text == "yes" || text == "on")
        value = true;
    else if (text == "0" || text == "false" || text == "no" || text == "off")
        value = false;
    else
        return false;
    return true;
}

void setting(const char *key, bool &value) {
    std::string text;
    if (!read_text(key, text))
        reshade::set_config_value(nullptr, kSection, key, value ? "1" : "0");
    else if (!parse_bool(text, value))
        bad_setting(key, text);
}

// A switch (PictureOnly, EdgeRules, StillHud, HistorySearch) is in ReShade.ini
// only while it differs from its default: a missing key stays missing, and one
// that says what the default says is taken out - here, and by the panel when a
// switch is put back. So a later version can change a default for everyone
// who did not choose otherwise. (A null value removes the key: ReShade 6.4 on,
// and this add-on needs a newer one to load at all.)
void forget_setting(const char *key) {
    reshade::set_config_value(nullptr, kSection, key, static_cast<const char *>(nullptr));
}

void switch_setting(const char *key, bool &value) {
    const bool standard = value;
    std::string text;
    if (!read_text(key, text)) return;
    if (!parse_bool(text, value))
        bad_setting(key, text);
    else if (value == standard)
        forget_setting(key);
}

void setting(const char *key, int &value) {
    std::string text;
    if (!read_text(key, text)) {
        reshade::set_config_value(nullptr, kSection, key, std::to_string(value).c_str());
        return;
    }
    char *end = nullptr;
    const long parsed = strtol(text.c_str(), &end, 0);  // base 0: "119" and "0x77" alike
    if (text.empty() || *end != '\0')
        bad_setting(key, text);
    else
        value = static_cast<int>(parsed);
}

void setting(const char *key, std::string &value) {
    std::string text;
    if (read_text(key, text))
        value = text;
    else
        reshade::set_config_value(nullptr, kSection, key, value.c_str());
}

void load_settings() {
    Settings &s = g_settings;
    setting("Enabled", s.enabled);
    setting("RequireDedupLauncher", s.require_dedup);
    setting("DebugMarker", s.marker);
    setting("LogStats", s.log_stats);
    setting("MaxOutputHz", s.max_output_hz);
    setting("ResetMs", s.reset_ms);
    setting("Multiplier", s.multiplier);
    s.multiplier = std::clamp(s.multiplier, 2, 4);
    setting("ToggleKey", s.toggle_key);
    setting("MpvPipe", s.mpv_pipe);
    setting("CaptureKey", s.capture_key);
    setting("DedupWhenOff", s.dedup_when_off);
    switch_setting("PictureOnly", s.picture_only);
    switch_setting("EdgeRules", s.edge_rules);
    switch_setting("StillHud", s.still_hud);
    switch_setting("HistorySearch", s.history_search);
    log_line(reshade::log::level::info, "switches: PictureOnly=%d EdgeRules=%d StillHud=%d HistorySearch=%d",
             s.picture_only, s.edge_rules, s.still_hud, s.history_search);
    std::string method = s.method == fg::Method::blend ? "blend" : "motion";
    setting("Method", method);
    for (auto &c : method) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (method == "blend")
        s.method = fg::Method::blend;
    else if (method == "motion")
        s.method = fg::Method::motion;
    else if (method == "vfg")
        s.vfg = true;
    else
        bad_setting("Method", method);
    std::string vfg_quality = "medium";
    setting("VfgQuality", vfg_quality);
    for (auto &c : vfg_quality) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (vfg_quality == "low")
        s.vfg_mode = fg::VfgMode::low;
    else if (vfg_quality == "medium")
        s.vfg_mode = fg::VfgMode::medium;
    else if (vfg_quality == "high")
        s.vfg_mode = fg::VfgMode::high;
    else if (vfg_quality == "auto")
        s.vfg_auto = true;  // from medium
    else
        bad_setting("VfgQuality", vfg_quality);
    std::string seen;
    if (read_text("GsyncSeen", seen)) parse_bool(seen, s.gsync_seen);  // not written until it is seen
    std::string sr_quality = "medium";
    setting("SrQuality", sr_quality);
    for (auto &c : sr_quality) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (sr_quality == "low")
        s.sr_level = fg::SrLevel::low;
    else if (sr_quality == "medium")
        s.sr_level = fg::SrLevel::medium;
    else if (sr_quality == "high")
        s.sr_level = fg::SrLevel::high;
    else if (sr_quality == "ultra")
        s.sr_level = fg::SrLevel::ultra;
    else
        bad_setting("SrQuality", sr_quality);
    std::string low_latency = "off";
    setting("LowLatency", low_latency);
    for (auto &c : low_latency) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (low_latency == "off" || low_latency == "on" || low_latency == "boost")
        s.low_latency = low_latency == "boost" ? 2 : low_latency == "on" ? 1 : 0;
    else
        bad_setting("LowLatency", low_latency);
    setting("VfgAutoCut", s.vfg_auto_cut);
    read_text("VfgRuntime", s.vfg_runtime);  // not written back: empty is the folder beside mpv.exe
    std::string quality = s.fast ? "fast" : "high";
    setting("Quality", quality);
    for (auto &c : quality) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (quality == "high" || quality == "fast")
        s.fast = quality == "fast";
    else
        bad_setting("Quality", quality);
    s.reset_ms = std::clamp(s.reset_ms, 20, 1000);
    // The launcher passes the full \\.\pipe\name form to mpv; take that too.
    const std::string prefix = "\\\\.\\pipe\\";
    if (s.mpv_pipe.compare(0, prefix.size(), prefix) == 0) s.mpv_pipe.erase(0, prefix.size());
}

// Method=vfg: the folder with the DLLs of NVIDIA's Video Frame Generation -
// GLASIR_VFG_RUNTIME (tests), VfgRuntime, or nvidia-vfg beside mpv.exe, where
// Glasir's setup puts them.
std::wstring vfg_runtime_folder() {
    wchar_t text[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"GLASIR_VFG_RUNTIME", text, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return text;
    if (!g_settings.vfg_runtime.empty()) {
        const std::string &utf8 = g_settings.vfg_runtime;
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
        std::wstring wide(size_t(n > 0 ? n : 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), n);
        return wide;
    }
    GetModuleFileNameW(nullptr, text, MAX_PATH);
    std::wstring folder(text);
    folder.erase(folder.find_last_of(L"\\/") + 1);
    return folder + L"nvidia-vfg";
}

// The model in use, and VfgQuality as it is set ("auto" where the add-on picks).
const char *vfg_mode_name() {
    return g_settings.vfg_mode == fg::VfgMode::low ? "low" : g_settings.vfg_mode == fg::VfgMode::high ? "high" : "medium";
}
const char *vfg_quality_name() { return g_settings.vfg_auto ? "auto" : vfg_mode_name(); }

// ---- Time ----------------------------------------------------------------------

double qpc_period() {
    static const double period = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    return period;
}

double qpc_seconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) * qpc_period();
}

// Sleeps on a high-resolution waitable timer until shortly before the target,
// then spins the rest, so the real frame goes out within a few microseconds.
class Sleeper {
public:
    Sleeper() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~Sleeper() {
        if (timer_) CloseHandle(timer_);
    }

    void until(double target) {
        constexpr double kSpin = 0.0007;
        const double wait = target - qpc_seconds() - kSpin;
        if (timer_ && wait > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(wait * 1e7);  // relative, 100 ns units
            if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0))
                WaitForSingleObject(timer_, 1000);
        }
        while (qpc_seconds() < target) YieldProcessor();
    }

private:
    HANDLE timer_ = nullptr;
};

// ---- The link from Glasir --------------------------------------------

class Link {
public:
    // Looks for the block once a second until Glasir has made it
    // (it starts mpv first, then creates the block under mpv's process id).
    void poll(double now) {
        if (view_ || now < next_try_) return;
        next_try_ = now + 1.0;
        wchar_t name[64];
        swprintf(name, 64, GLASIR_FRAMEGEN_LINK_NAME, GetCurrentProcessId());
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        if (!mapping) return;
        auto *view = static_cast<GlasirFrameGenLink *>(
            MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(GlasirFrameGenLink)));
        if (!view || view->magic != GLASIR_FRAMEGEN_LINK_MAGIC || view->version != GLASIR_FRAMEGEN_LINK_VERSION) {
            if (view) UnmapViewOfFile(view);
            CloseHandle(mapping);
            return;
        }
        mapping_ = mapping;
        view_ = view;
        log_line(reshade::log::level::info, "linked to Glasir: new frames only, with their arrival times");
    }

    void close() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr;
        mapping_ = nullptr;
    }

    bool linked() const { return view_ != nullptr; }

    // Asks Glasir for the repeats too (frame generation off) or for
    // new frames only.
    void send_repeats(bool on) {
        const LONG want = on ? 1 : 0;
        if (view_ && view_->send_repeats != want) InterlockedExchange(&view_->send_repeats, want);
    }
    LONG64 dropped() const { return view_ ? view_->dropped : 0; }

    // The mean time between the last new frames from the source, leaving out
    // pauses longer than `gap`. 0 when there are too few to tell. A mean, not a
    // median: a source that delivers in uneven steps (31, 31, 31, 46 ms) has
    // its median well below its real rate, and the real rate is what decides
    // whether twice it fits the display.
    double source_interval(double gap) const {
        double sum = 0;
        int n = 0;
        each_gap(gap, kAll, [&](double d) {
            sum += d;
            ++n;
        });
        return n >= 3 ? sum / n : 0;
    }

    // The shortest of the last few gaps: how soon the next new frame may
    // come, also while the rate is going up (the mean lags) or uneven. 0
    // when there are too few to tell.
    double shortest_recent_gap(double gap) const {
        double least = 1e9;
        int n = 0;
        each_gap(gap, 4, [&](double d) {
            least = std::min(least, d);
            ++n;
        });
        return n >= 2 ? least : 0;
    }

    // New frames so far: changes when the source delivered one.
    LONG64 published() const { return view_ ? view_->published : 0; }

    // Seconds since the source last delivered a new frame.
    double since_last(double now) const {
        if (!view_) return 1e9;
        const LONG64 p = view_->published;
        if (p == 0) return 1e9;
        return now - double(view_->stamps[(p - 1) % GlasirFrameGenLink::kStamps]) * qpc_period();
    }

private:
    static constexpr int kAll = GlasirFrameGenLink::kStamps;

    // Calls f with the gaps between the last `count` pairs of new frames,
    // newest first, in seconds, leaving out pauses longer than `gap`.
    template <class F>
    void each_gap(double gap, int count, F &&f) const {
        if (!view_) return;
        const LONG64 p = view_->published;
        constexpr int kStamps = GlasirFrameGenLink::kStamps;
        // The slot the writer fills next is the oldest; leave it out.
        const int pairs = std::min(count, static_cast<int>(std::min<LONG64>(p, kStamps - 1)) - 1);
        for (int k = 1; k <= pairs; ++k) {
            const LONG64 newer = view_->stamps[(p - k) % kStamps];
            const LONG64 older = view_->stamps[(p - k - 1) % kStamps];
            const double d = double(newer - older) * qpc_period();
            if (d > 0.002 && d <= gap) f(d);
        }
    }

    HANDLE mapping_ = nullptr;
    GlasirFrameGenLink *view_ = nullptr;
    double next_try_ = 0;
};

Link g_link;

// ---- mpv IPC ------------------------------------------------------------------

// Sends one IPC line to mpv, on a thread of its own so the render thread never
// waits on the pipe. The thread holds a reference to this module and drops it
// on exit, so mpv closing meanwhile cannot unload the code under it.
void mpv_send(const std::string &line) {
    struct Job {
        std::wstring pipe;
        std::string line;
    };
    auto *job = new Job;
    job->pipe = L"\\\\.\\pipe\\" + std::wstring(g_settings.mpv_pipe.begin(), g_settings.mpv_pipe.end());
    job->line = line;

    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&mpv_send), &self)) {
        delete job;
        return;
    }
    auto run = [](LPVOID param) -> DWORD {
        auto *job = static_cast<Job *>(param);
        for (int attempt = 0; attempt < 2; ++attempt) {
            HANDLE pipe = CreateFileW(job->pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(pipe, job->line.data(), static_cast<DWORD>(job->line.size()), &written, nullptr);
                CloseHandle(pipe);
                break;
            }
            if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(job->pipe.c_str(), 250)) break;
        }
        delete job;
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&mpv_send), &module);
        FreeLibraryAndExitThread(module, 0);
    };
    HANDLE thread = CreateThread(nullptr, 0, run, job, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete job;
        FreeLibrary(self);
    }
}

// Asks Glasir.exe (an IPC client of mpv) to save a bug report, with the
// frames in folder; mpv passes script messages on to its clients. Without
// frames, why says why not: "off" (frame generation is off), "unavailable"
// (it cannot run here) or "failed" (they could not be saved).
void mpv_bug_report(const std::wstring &folder, const char *why = "") {
    std::string utf8;
    if (!folder.empty()) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, folder.data(), int(folder.size()), nullptr, 0, nullptr, nullptr);
        utf8.resize(size_t(n > 0 ? n : 0));
        WideCharToMultiByte(CP_UTF8, 0, folder.data(), int(folder.size()), utf8.data(), n, nullptr, nullptr);
    }
    std::string quoted;
    for (char c : utf8) {
        if (c == '\\' || c == '"') quoted += '\\';
        quoted += c;
    }
    mpv_send("{\"command\":[\"script-message\",\"glasir-bug-report\",\"" + quoted + "\",\"" + why + "\"]}\n");
}

// ---- Capture ------------------------------------------------------------------

// The capture key saves what frame generation works with at that moment: five
// real frames in a row as the DLSS 5 pass left them (f01.png - f05.png) and
// the frames the add-on drew between them (between_f01_f02.png, ...), into
// Captures\<date-time>\ in the app folder. eval\fg_eval.exe reads such
// a folder like one of its clips: it makes the frame between f01 and f03 and
// compares it with f02, and so on - a score on the game's own pictures.
//
// The copies go to the GPU's staging memory as the frames pass; two frames
// after the last one they are read back (one hitch of a few tens of
// milliseconds) and a thread of its own writes the files. 10-bit HDR (PQ)
// frames are written as 16-bit PNG, everything else as 8-bit: an SDR picture
// in a 10-bit back buffer (mpv picks one on a 10-bit desktop) too, so a 16-bit
// capture always means PQ to the viewer and the eval tools.
class Capture {
public:
    static constexpr int kReals = 5;

    bool busy() const { return active_ || wait_ >= 0; }
    bool active() const { return active_; }

    // Nothing is shown on mpv's OSD here: mpv draws it into the back buffer,
    // and it would be in the frames being captured.
    // pq: the back buffer holds HDR10 (PQ) values, not SDR ones.
    // report: a bug report waits for the frames (the key, not a timed test).
    bool start(ID3D11Device *device, const D3D11_TEXTURE2D_DESC &bb, bool pq, bool report) {
        reset();
        report_ = report;
        if (bb.Format != DXGI_FORMAT_R10G10B10A2_UNORM && bb.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            bb.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            log_line(reshade::log::level::warning, "capture: back buffer format %d is not one it can save", bb.Format);
            return false;
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        swprintf(stamp_, 32, L"%04u%02u%02u-%02u%02u%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond, t.wMilliseconds);
        device_ = device;
        pq_ = pq;
        desc_ = bb;
        desc_.BindFlags = 0;
        desc_.MiscFlags = 0;
        desc_.Usage = D3D11_USAGE_STAGING;
        desc_.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        active_ = true;
        log_line(reshade::log::level::info, "capture: saving the next %d frames", kReals);
        return true;
    }

    // The frame drawn between the last real frame captured and the one now.
    void between(ID3D11DeviceContext *ctx, ID3D11Texture2D *drawn) {
        if (!active_ || reals_ == 0) return;
        char name[48];
        snprintf(name, sizeof(name), "between_f%02d_f%02d", reals_, reals_ + 1);
        add(ctx, drawn, name);
    }

    void real(ID3D11DeviceContext *ctx, ID3D11Texture2D *frame) {
        if (!active_) return;
        char name[16];
        snprintf(name, sizeof(name), "f%02d", ++reals_);
        add(ctx, frame, name);
        if (reals_ == kReals) {
            active_ = false;
            wait_ = 2;
        }
    }

    // Every frame: reads the copies back once the GPU is done with them.
    void tick(ID3D11DeviceContext *ctx) {
        if (wait_ < 0 || active_) return;
        if (wait_ > 0) {
            --wait_;
            return;
        }
        wait_ = -1;
        auto *job = new Job;
        job->expected = shots_.size();
        job->stamp = stamp_;
        job->width = desc_.Width;
        job->height = desc_.Height;
        job->format = desc_.Format;
        job->deep = desc_.Format == DXGI_FORMAT_R10G10B10A2_UNORM && pq_;
        job->report = report_;
        report_ = false;  // the write thread sends it now
        for (auto &shot : shots_) {
            D3D11_MAPPED_SUBRESOURCE map{};
            if (FAILED(ctx->Map(shot.staging.Get(), 0, D3D11_MAP_READ, 0, &map))) continue;
            Image image;
            image.name = shot.name;
            image.pixels.resize(size_t(desc_.Width) * desc_.Height * 4);
            for (UINT y = 0; y < desc_.Height; ++y)
                memcpy(image.pixels.data() + size_t(y) * desc_.Width * 4,
                       static_cast<const BYTE *>(map.pData) + size_t(y) * map.RowPitch, size_t(desc_.Width) * 4);
            ctx->Unmap(shot.staging.Get(), 0);
            job->images.push_back(std::move(image));
        }
        reset();
        write(job);
    }

    // Stops a capture under way; a bug report waiting for it is made without
    // frames (tell = false: mpv is closing, no one to tell).
    void reset(bool tell = true) {
        if (tell && report_ && (active_ || wait_ >= 0)) mpv_bug_report(L"", "failed");
        shots_.clear();
        device_.Reset();
        report_ = false;
        active_ = false;
        reals_ = 0;
        wait_ = -1;
    }

private:
    struct Shot {
        std::string name;
        ComPtr<ID3D11Texture2D> staging;
    };
    struct Image {
        std::string name;
        std::vector<BYTE> pixels;  // 4 bytes a pixel, tightly packed
    };
    struct Job {
        size_t expected = 0;  // images copied; fewer read back is a failure
        std::wstring stamp;
        UINT width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool deep = false;  // 16-bit PNG (10-bit PQ); otherwise 8-bit
        bool report = false;  // a bug report waits for the frames
        std::vector<Image> images;
    };

    void add(ID3D11DeviceContext *ctx, ID3D11Texture2D *source, const char *name) {
        Shot shot;
        shot.name = name;
        if (FAILED(device_->CreateTexture2D(&desc_, nullptr, &shot.staging))) {
            log_line(reshade::log::level::warning, "capture: no memory for %s", name);
            return;
        }
        ctx->CopyResource(shot.staging.Get(), source);
        shots_.push_back(std::move(shot));
    }

    // Writes the files on a thread of its own, which holds a reference to this
    // module so mpv closing meanwhile cannot unload the code under it.
    static void write(Job *job) {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Capture::write), &self)) {
            if (job->report) mpv_bug_report(L"", "failed");
            delete job;
            return;
        }
        auto run = [](LPVOID param) -> DWORD {
            auto *job = static_cast<Job *>(param);
            const std::wstring folder = save(*job);
            const bool report = job->report;
            delete job;
            if (report) {
                if (folder.empty()) glasir::overlay::toast("Frame generation: could not save the frames, see ReShade.log");
                mpv_bug_report(folder, folder.empty() ? "failed" : "");  // Glasir.exe says when it is saved
            } else {
                glasir::overlay::toast(folder.empty() ? "Frame generation: capture failed, see ReShade.log"
                                             : "Frame generation: frames saved to Captures");
            }
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&Capture::write), &module);
            FreeLibraryAndExitThread(module, 0);
        };
        HANDLE thread = CreateThread(nullptr, 0, run, job, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            if (job->report) mpv_bug_report(L"", "failed");
            delete job;
            FreeLibrary(self);
        }
    }

    static std::wstring save(const Job &job) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring folder(exe);
        folder.erase(folder.find_last_of(L"\\/") + 1);
        folder += L"..\\Captures";  // in the app folder (mpv.exe is in engine)
        CreateDirectoryW(folder.c_str(), nullptr);
        // Its own folder, named when the key was pressed; never one in use.
        const std::wstring base = folder + L"\\" + job.stamp;
        folder = base;
        for (int n = 2; !CreateDirectoryW(folder.c_str(), nullptr); ++n) {
            if (GetLastError() != ERROR_ALREADY_EXISTS || n > 99) {
                log_line(reshade::log::level::error, "capture: could not create its folder");
                return {};
            }
            folder = base + L"-" + std::to_wstring(n);
        }
        if (job.images.empty() || job.images.size() < job.expected) {
            log_line(reshade::log::level::error, "capture: only %zu of %zu frames could be read back", job.images.size(),
                     job.expected);
            return {};
        }

        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ComPtr<IWICImagingFactory> wic;
        bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
        for (const auto &image : job.images) {
            if (!ok) break;
            const std::wstring path = folder + L"\\" + std::wstring(image.name.begin(), image.name.end()) + L".png";
            ok = png(wic.Get(), path, job, image.pixels);
        }
        wic.Reset();
        if (com) CoUninitialize();
        if (!ok) {
            log_line(reshade::log::level::error, "capture: writing the PNG files failed");
            return {};
        }
        log_line(reshade::log::level::info, "capture: %zu frames saved (%ux%u, format %d, %s)", job.images.size(),
                 job.width, job.height, job.format, job.deep ? "16-bit PQ" : "8-bit SDR");
        return folder;
    }

    static bool png(IWICImagingFactory *wic, const std::wstring &path, const Job &job, const std::vector<BYTE> &pixels) {
        // 10-bit PQ as 16-bit RGBA, the rest as BGRA (the order WIC's PNG
        // encoder takes); 10-bit SDR is rounded to 8 bits.
        const bool deep = job.deep;
        const size_t n = size_t(job.width) * job.height;
        std::vector<BYTE> out(n * (deep ? 8 : 4));
        for (size_t i = 0; i < n; ++i) {
            const BYTE *px = pixels.data() + i * 4;
            if (job.format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                UINT32 v;
                memcpy(&v, px, 4);
                UINT16 c[4] = {UINT16(v & 1023), UINT16((v >> 10) & 1023), UINT16((v >> 20) & 1023), UINT16(v >> 30)};
                if (deep) {
                    for (int k = 0; k < 3; ++k) c[k] = UINT16((c[k] << 6) | (c[k] >> 4));
                    c[3] = UINT16(c[3] * 21845);
                    memcpy(out.data() + i * 8, c, 8);
                } else {
                    BYTE *o = out.data() + i * 4;
                    for (int k = 0; k < 3; ++k) o[2 - k] = BYTE((c[k] * 255u + 511u) / 1023u);
                    o[3] = BYTE(c[3] * 85u);
                }
            } else if (job.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                out[i * 4 + 0] = px[2], out[i * 4 + 1] = px[1], out[i * 4 + 2] = px[0], out[i * 4 + 3] = px[3];
            } else {
                memcpy(out.data() + i * 4, px, 4);
            }
        }
        WICPixelFormatGUID format = deep ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppBGRA;
        const WICPixelFormatGUID wanted = format;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        return SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
               SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
               SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
               SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
               SUCCEEDED(frame->SetSize(job.width, job.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
               IsEqualGUID(format, wanted) &&
               SUCCEEDED(frame->WritePixels(job.height, job.width * (deep ? 8 : 4), UINT(out.size()), out.data())) &&
               SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }

    ComPtr<ID3D11Device> device_;
    D3D11_TEXTURE2D_DESC desc_{};
    bool pq_ = false;
    bool report_ = false;
    wchar_t stamp_[32] = {};
    std::vector<Shot> shots_;
    bool active_ = false;
    int reals_ = 0;
    int wait_ = -1;  // frames until the copies are read back; -1 = nothing to read
};

// ---- Display ------------------------------------------------------------------

const char *color_space_name(reshade::api::color_space cs) {
    switch (cs) {
    case reshade::api::color_space::srgb: return "sRGB (SDR)";
    case reshade::api::color_space::scrgb: return "scRGB";
    case reshade::api::color_space::hdr10_pq: return "HDR10 (PQ)";
    case reshade::api::color_space::hdr10_hlg: return "HDR10 (HLG)";
    default: return "unknown";
    }
}

// The refresh rate of the screen the swap chain is on, 0 when unknown.
int display_hz(IDXGISwapChain *swapchain) {
    ComPtr<IDXGIOutput> output;
    if (FAILED(swapchain->GetContainingOutput(&output))) return 0;
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) return 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    return mode.dmDisplayFrequency > 1 ? static_cast<int>(mode.dmDisplayFrequency) : 0;
}

// Presents still queued for the screen, or -1 when the swap chain cannot say:
// the call fails, or succeeds with the statistics left at zero (mpv's own
// d3d11 code has seen that), or gives a count no queue could hold.
int queued_presents(IDXGISwapChain *swapchain) {
    UINT last = 0;
    DXGI_FRAME_STATISTICS stats{};
    if (FAILED(swapchain->GetLastPresentCount(&last)) || FAILED(swapchain->GetFrameStatistics(&stats))) return -1;
    if (stats.PresentCount == 0) return -1;
    const UINT queued = last - stats.PresentCount;
    return queued <= 8 ? static_cast<int>(queued) : -1;
}

// ---- Trace --------------------------------------------------------------------

// GLASIR_FRAMEGEN_TRACE=<file>: one CSV line per real frame, for the whole run.
FILE *g_trace = nullptr;

void trace_open() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    char path[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableA("GLASIR_FRAMEGEN_TRACE", path, sizeof(path));
    if (len > 0 && len < sizeof(path) && fopen_s(&g_trace, path, "w") == 0)
        fprintf(g_trace,
                "time_s,dt_ms,interval_ms,source_interval_ms,queued,event,source_age_ms,effects_ms,drawn_ms,real_ms\n");
}

// One line. The hold thread writes the lines of held frames (once they went
// out), mpv's thread the rest; the C runtime keeps each line whole.
struct TraceLine {
    double now = 0, dt = 0, interval = 0, source_interval = 0;
    int queued = -1;
    const char *event = "";
    double source_age = -1;  // since the source delivered its latest frame, -1 without the link
    double effects = -1;     // from mpv's present to reshade_present
    double drawn = -1;       // when the frame in between was done on the GPU, after now
    double real = -1;        // when the real frame went out, after now
};

void write_trace(const TraceLine &l) {
    const auto ms = [](double s) { return s < 0 ? -1.0 : s * 1000.0; };
    if (g_trace)
        fprintf(g_trace, "%.6f,%.3f,%.3f,%.3f,%d,%s,%.3f,%.3f,%.3f,%.3f\n", l.now, l.dt * 1000.0,
                l.interval * 1000.0, l.source_interval * 1000.0, l.queued, l.event, ms(l.source_age),
                ms(l.effects), ms(l.drawn), ms(l.real));
}

// ---- Holding the real frame on the GPU ----------------------------------------

// A held real frame: its fence value, when to let it go, and its trace line.
struct HoldJob {
    uint64_t n = 0;
    double half = 0;    // this long after the frame before it is done (a step: half an interval at 2x)
    double latest = 0;  // the real frame no later than this
    int left = 1;       // frames still to let go for this real frame, this one included
    TraceLine line;
};

// Lets the real frame go out at its time without mpv's render thread waiting
// for it. After mpv's present of the frame in between, mpv's thread queues on
// the GPU a signal that the frame in between is done (fence drawn_), the copy
// of the real frame into the back buffer, a wait for fence release_ and the
// present of the real frame, and goes on. This class's thread sees drawn_
// reach the frame, sleeps until half an interval later and moves release_ on;
// the GPU continues behind it and the real frame is shown.
//
// D3D11 cannot move a fence from the CPU, so release_ is a Direct3D 12 fence
// on the same GPU, opened in D3D11. Every wait queued must be released, or
// mpv's GPU work stops behind it: the thread lets a frame go after 200 ms even
// if drawn_ never gets there, and stop() lets all of them go.
class Hold {
public:
    bool start(ID3D11Device *device, ID3D11DeviceContext *context, std::string &error);
    void stop();
    // False once a release failed: new frames are then timed from mpv's present.
    bool active() const { return thread_ != nullptr && !signal_failed_; }
    // Lets every frame held so far go now (mpv's thread: a resize is coming,
    // which waits for the held present).
    void release_all() {
        if (thread_) release_up_to(issued_);
    }

    // On mpv's thread, in this order around the real frame's copy and present.
    uint64_t mark_drawn() {
        ctx_->Signal(drawn_.Get(), ++issued_);
        return issued_;
    }
    void wait_release(uint64_t n) { ctx_->Wait(release11_.Get(), n); }
    void let_go(const HoldJob &job);

    // Each frame counts once: drawn (the frame in between was seen done, and
    // the thread timed it: held_sum is from then to the release; early = let
    // go at `latest`, before its half; overdue = `latest` had passed when it
    // was done), not_drawn (no done signal: let go at once), at_once (let go
    // with a later frame: the queue was full, or a resize). timed: frames
    // whose release the thread timed, for late_sum.
    struct Stats {
        unsigned frames = 0, drawn = 0, early = 0, overdue = 0, not_drawn = 0, at_once = 0, timed = 0;
        double drawn_sum = 0, held_sum = 0, late_sum = 0, late_max = 0;
    };
    Stats take_stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        const Stats s = stats_;
        stats_ = Stats{};
        return s;
    }

private:
    static DWORD WINAPI run(void *self);
    void loop();
    bool release_up_to(uint64_t n);
    bool release_now(uint64_t n);
    bool released(uint64_t n) {
        std::lock_guard<std::mutex> lock(signal_mutex_);
        return released_ >= n;
    }
    void reset();

    ComPtr<ID3D11DeviceContext4> ctx_;
    ComPtr<ID3D11Fence> drawn_, release11_;
    ComPtr<ID3D12Device> device12_;
    ComPtr<ID3D12Fence> release_;
    HANDLE thread_ = nullptr, drawn_event_ = nullptr;
    HMODULE module_ = nullptr;  // held by the thread while it runs
    HMODULE d3d12_ = nullptr;
    bool stuck_ = false;        // a thread that did not end: its objects stay as they are
    uint64_t issued_ = 0;       // mpv's thread only

    std::mutex mutex_;         // the jobs and the stats
    std::mutex signal_mutex_;  // released_, and the fence moving in order
    std::condition_variable wake_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> signal_failed_{false};
    static constexpr int kJobs = 8;
    HoldJob jobs_[kJobs];
    int first_ = 0, count_ = 0;
    uint64_t released_ = 0;
    Stats stats_;
};

bool Hold::start(ID3D11Device *device, ID3D11DeviceContext *context, std::string &error) {
    if (thread_ || stuck_) return thread_ != nullptr;
    ComPtr<ID3D11Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) || FAILED(context->QueryInterface(IID_PPV_ARGS(&ctx_)))) {
        error = "no Direct3D 11.4 fences";
        return reset(), false;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter))) {
        error = "no DXGI adapter";
        return reset(), false;
    }
    d3d12_ = LoadLibraryW(L"d3d12.dll");
    const auto create =
        d3d12_ ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12_, "D3D12CreateDevice")) : nullptr;
    if (!create || FAILED(create(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12_)))) {
        error = "no Direct3D 12 device on this GPU";
        return reset(), false;
    }
    HANDLE shared = nullptr;
    HRESULT hr = device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&release_));
    if (SUCCEEDED(hr)) hr = device12_->CreateSharedHandle(release_.Get(), nullptr, GENERIC_ALL, nullptr, &shared);
    if (SUCCEEDED(hr)) hr = device5->OpenSharedFence(shared, IID_PPV_ARGS(&release11_));
    if (shared) CloseHandle(shared);
    if (SUCCEEDED(hr)) hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&drawn_));
    if (FAILED(hr)) {
        char text[48];
        snprintf(text, sizeof(text), "fences: 0x%08lX", static_cast<unsigned long>(hr));
        error = text;
        return reset(), false;
    }
    drawn_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!drawn_event_ || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                             reinterpret_cast<LPCWSTR>(&Hold::run), &module_)) {
        error = "no event";
        return reset(), false;
    }
    stopping_ = false;
    thread_ = CreateThread(nullptr, 0, run, this, 0, nullptr);
    if (!thread_) {
        FreeLibrary(module_);
        error = "no thread";
        return reset(), false;
    }
    // Its wake-ups are the real frames' times on screen.
    SetThreadPriority(thread_, THREAD_PRIORITY_TIME_CRITICAL);
    return true;
}

void Hold::reset() {
    ctx_.Reset();
    drawn_.Reset();
    release11_.Reset();
    release_.Reset();
    device12_.Reset();
    if (d3d12_) FreeLibrary(d3d12_);
    d3d12_ = nullptr;
    if (drawn_event_) CloseHandle(drawn_event_);
    drawn_event_ = nullptr;
    module_ = nullptr;
    signal_failed_ = false;
    issued_ = released_ = 0;
    first_ = count_ = 0;
    stats_ = Stats{};
}

void Hold::stop() {
    if (!thread_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        count_ = 0;
    }
    wake_.notify_all();
    release_now(issued_);  // nothing stays held
    SetEvent(drawn_event_);
    const bool ended = WaitForSingleObject(thread_, 2000) == WAIT_OBJECT_0;
    CloseHandle(thread_);
    thread_ = nullptr;
    if (!ended) {
        stuck_ = true;
        log_line(reshade::log::level::error, "the hold thread did not end; the real frame is now timed from mpv's present");
        return;
    }
    reset();
}

void Hold::let_go(const HoldJob &job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ < kJobs) {
            jobs_[(first_ + count_++) % kJobs] = job;
            wake_.notify_one();
            return;
        }
        ++stats_.frames;
        ++stats_.at_once;
    }
    // The thread is that far behind: let this one go now, and those queued before it.
    const bool out = release_now(job.n);
    TraceLine line = job.line;
    line.event = "generated-at-once";
    line.real = out ? qpc_seconds() - line.now : -1;
    write_trace(line);
}

// Moves the release fence on to n (never back). False if it would not move:
// the value stays unreleased, so the next call tries again, and new frames
// stop being held. Its own lock, not the jobs' and stats', so a Signal that
// takes long holds up only other releases.
bool Hold::release_up_to(uint64_t n) {
    std::lock_guard<std::mutex> lock(signal_mutex_);
    if (n <= released_) return true;
    const HRESULT hr = release_->Signal(n);
    if (FAILED(hr)) {
        if (!signal_failed_.exchange(true))
            log_line(reshade::log::level::error,
                     "letting a held real frame go failed (0x%08lX); the real frame is now timed from mpv's present",
                     static_cast<unsigned long>(hr));
        return false;
    }
    released_ = n;
    return true;
}

// release_up_to, tried again for up to 200 ms: the GPU waits behind the value.
bool Hold::release_now(uint64_t n) {
    for (int tries = 0;; ++tries) {
        if (release_up_to(n)) return true;
        if (tries == 200) return false;
        Sleep(1);
    }
}

DWORD WINAPI Hold::run(void *self) {
    auto *hold = static_cast<Hold *>(self);
    const HMODULE module = hold->module_;
    hold->loop();
    FreeLibraryAndExitThread(module, 0);
}

void Hold::loop() {
    Sleeper sleeper;
    for (;;) {
        HoldJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || count_ > 0; });
            if (stopping_) return;
            job = jobs_[first_];
            first_ = (first_ + 1) % kJobs;
            --count_;
        }
        const double give_up = qpc_seconds() + 0.2;
        while (drawn_->GetCompletedValue() < job.n && !stopping_ && !released(job.n)) {
            const double left = give_up - qpc_seconds();
            if (left <= 0) break;
            if (FAILED(drawn_->SetEventOnCompletion(job.n, drawn_event_))) break;
            WaitForSingleObject(drawn_event_, static_cast<DWORD>(left * 1000.0) + 1);
        }
        // UINT64_MAX: the device was removed. Nothing was drawn; let it go now.
        const UINT64 completed = drawn_->GetCompletedValue();
        const bool drawn = completed >= job.n && completed != UINT64_MAX;
        const double drawn_at = qpc_seconds();
        // A step after the frame before was done - or, with less time than that for each frame
        // still to go before `latest`, an equal share of what is left.
        const double step = std::min(job.half, std::max(0.0, job.latest - drawn_at) / std::max(1, job.left));
        const double target = drawn && !stopping_ ? drawn_at + step : drawn_at;
        if (!released(job.n)) sleeper.until(target);
        // Let go meanwhile with a later frame (queue full, a resize): not timed
        // here. Otherwise let go now - also when stopping, in case stop()'s
        // own release failed.
        const bool gone = released(job.n);
        const bool out = gone || release_now(job.n);
        const double out_at = qpc_seconds();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.frames;
            if (gone) {
                ++stats_.at_once;
            } else if (!drawn) {
                ++stats_.not_drawn;
            } else {
                ++stats_.drawn;
                stats_.drawn_sum += drawn_at - job.line.now;
                stats_.held_sum += out_at - drawn_at;
                if (step < job.half) ++(job.latest > drawn_at ? stats_.early : stats_.overdue);
            }
            if (!gone && out) {
                ++stats_.timed;
                stats_.late_sum += out_at - target;
                stats_.late_max = std::max(stats_.late_max, out_at - target);
            }
        }
        job.line.drawn = drawn ? drawn_at - job.line.now : -1;
        job.line.real = !gone && out ? out_at - job.line.now : -1;
        write_trace(job.line);
        if (stopping_) return;
    }
}

// ---- The generator ------------------------------------------------------------

// The pipeline state fg::Interpolator touches, saved before and put back
// after, so mpv and the other add-ons find everything as they left it. ReShade
// does the same rather than SwapDeviceContextState, which exposes an
// ID3D10Device and breaks overlays such as RTSS. The slot counts cover what
// fg_interp.h binds (it stays within them).
class StateBackup {
public:
    static constexpr UINT kSrvs = 8, kUavs = 8, kSamplers = 4, kBuffers = 2;

    explicit StateBackup(ID3D11DeviceContext *ctx) : ctx_(ctx) {
        ctx->IAGetPrimitiveTopology(&topology_);
        ctx->IAGetInputLayout(&layout_);
        ctx->VSGetShader(&vs_, nullptr, nullptr);
        ctx->HSGetShader(&hs_, nullptr, nullptr);
        ctx->DSGetShader(&ds_, nullptr, nullptr);
        ctx->GSGetShader(&gs_, nullptr, nullptr);
        ctx->PSGetShader(&ps_, nullptr, nullptr);
        ctx->PSGetShaderResources(0, kSrvs, ps_srvs_);
        ctx->PSGetSamplers(0, kSamplers, ps_samplers_);
        ctx->PSGetConstantBuffers(0, kBuffers, ps_cbs_);
        ctx->CSGetShader(&cs_, nullptr, nullptr);
        ctx->CSGetShaderResources(0, kSrvs, cs_srvs_);
        ctx->CSGetUnorderedAccessViews(0, kUavs, cs_uavs_);
        ctx->CSGetSamplers(0, kSamplers, cs_samplers_);
        ctx->CSGetConstantBuffers(0, kBuffers, cs_cbs_);
        ctx->RSGetState(&rs_);
        viewport_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&viewport_count_, viewports_);
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, &dsv_);
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (rtvs_[i]) rtv_count_ = i + 1;
        ctx->OMGetBlendState(&blend_, blend_factor_, &sample_mask_);
        ctx->OMGetDepthStencilState(&depth_, &stencil_ref_);
    }

    ~StateBackup() {
        ID3D11DeviceContext *ctx = ctx_;
        // Outputs first, so a restored shader resource that is also one of
        // our outputs is not bound twice for a moment. Only as many render
        // target slots as were in use, so unordered access views bound past
        // them stay.
        ctx->OMSetRenderTargets(rtv_count_, rtvs_, dsv_);
        ctx->CSSetUnorderedAccessViews(0, kUavs, cs_uavs_, nullptr);
        ctx->OMSetBlendState(blend_, blend_factor_, sample_mask_);
        ctx->OMSetDepthStencilState(depth_, stencil_ref_);
        ctx->RSSetState(rs_);
        ctx->RSSetViewports(viewport_count_, viewports_);
        ctx->CSSetConstantBuffers(0, kBuffers, cs_cbs_);
        ctx->CSSetSamplers(0, kSamplers, cs_samplers_);
        ctx->CSSetShaderResources(0, kSrvs, cs_srvs_);
        ctx->CSSetShader(cs_, nullptr, 0);
        ctx->PSSetConstantBuffers(0, kBuffers, ps_cbs_);
        ctx->PSSetSamplers(0, kSamplers, ps_samplers_);
        ctx->PSSetShaderResources(0, kSrvs, ps_srvs_);
        ctx->PSSetShader(ps_, nullptr, 0);
        ctx->GSSetShader(gs_, nullptr, 0);
        ctx->DSSetShader(ds_, nullptr, 0);
        ctx->HSSetShader(hs_, nullptr, 0);
        ctx->VSSetShader(vs_, nullptr, 0);
        ctx->IASetInputLayout(layout_);
        ctx->IASetPrimitiveTopology(topology_);
        // The Get* calls above added a reference to everything they returned.
        auto drop = [](IUnknown *p) {
            if (p) p->Release();
        };
        drop(layout_), drop(vs_), drop(hs_), drop(ds_), drop(gs_), drop(ps_), drop(cs_), drop(rs_);
        drop(dsv_), drop(blend_), drop(depth_);
        for (auto *p : ps_srvs_) drop(p);
        for (auto *p : ps_samplers_) drop(p);
        for (auto *p : ps_cbs_) drop(p);
        for (auto *p : cs_srvs_) drop(p);
        for (auto *p : cs_uavs_) drop(p);
        for (auto *p : cs_samplers_) drop(p);
        for (auto *p : cs_cbs_) drop(p);
        for (auto *p : rtvs_) drop(p);
    }

    StateBackup(const StateBackup &) = delete;
    StateBackup &operator=(const StateBackup &) = delete;

private:
    ID3D11DeviceContext *ctx_;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout *layout_ = nullptr;
    ID3D11VertexShader *vs_ = nullptr;
    ID3D11HullShader *hs_ = nullptr;
    ID3D11DomainShader *ds_ = nullptr;
    ID3D11GeometryShader *gs_ = nullptr;
    ID3D11PixelShader *ps_ = nullptr;
    ID3D11ShaderResourceView *ps_srvs_[kSrvs] = {};
    ID3D11SamplerState *ps_samplers_[kSamplers] = {};
    ID3D11Buffer *ps_cbs_[kBuffers] = {};
    ID3D11ComputeShader *cs_ = nullptr;
    ID3D11ShaderResourceView *cs_srvs_[kSrvs] = {};
    ID3D11UnorderedAccessView *cs_uavs_[kUavs] = {};
    ID3D11SamplerState *cs_samplers_[kSamplers] = {};
    ID3D11Buffer *cs_cbs_[kBuffers] = {};
    ID3D11RasterizerState *rs_ = nullptr;
    UINT viewport_count_ = 0;
    D3D11_VIEWPORT viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11RenderTargetView *rtvs_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    UINT rtv_count_ = 0;
    ID3D11DepthStencilView *dsv_ = nullptr;
    ID3D11BlendState *blend_ = nullptr;
    FLOAT blend_factor_[4] = {};
    UINT sample_mask_ = 0;
    ID3D11DepthStencilState *depth_ = nullptr;
    UINT stencil_ref_ = 0;
};

// ---- NVIDIA Reflex -------------------------------------------------------------
//
// LowLatency=on|boost: NVIDIA Reflex's low latency mode on mpv's device
// (nv_low_latency.h; an NVIDIA GPU's driver only). This add-on is where it
// can be asked for: it is in mpv's process, on the thread of mpv's presents.
//
// The driver is told the mode once, and called once a frame after mpv's
// present has returned and the frames this add-on presents after it are
// queued - where mpv goes back to wait for the next frame, so where the next
// frame's work starts. If frames are queueing before the GPU, the driver
// waits there, so that the next one is started later and shown as early.
// mpv's work on a frame cannot be marked for the driver (it is done before
// the present this add-on sees): the frame is marked from mpv's present on,
// which is what the driver's timings in the log are counted from.
//
// GLASIR_LATENCY_LOG=1 marks the frames and logs those timings with the mode
// off too, to compare.
class LowLatency {
public:
    // mpv's device is known. On mpv's render thread, like everything here.
    void start(ID3D11Device *device) {
        device_ = device;
        char text[8] = {};
        const DWORD len = GetEnvironmentVariableA("GLASIR_LATENCY_LOG", text, sizeof(text));
        measure_ = len > 0 && len < sizeof(text) && strcmp(text, "0") != 0;
        offered_ = nv_.offered(device);
        active_ = false;
        stage_ = 0;
        gsync_ = composed_ = -1;
        next_watch_ = 0;
        if (!offered_) {
            if (g_settings.low_latency > 0)
                log_line(reshade::log::level::warning,
                         "LowLatency=%s: NVIDIA Reflex is not available on mpv's device (no NVIDIA driver, or mpv "
                         "is on another GPU)",
                         name());
            return;
        }
        if (g_settings.low_latency > 0)
            apply();
        else if (measure_)
            log_line(reshade::log::level::info, "NVIDIA Reflex: off; GLASIR_LATENCY_LOG: the driver's timings are logged");
    }

    // The device is going away.
    void stop() {
        device_ = nullptr;
        offered_ = active_ = false;
        stage_ = 0;
    }

    // For the menu: 0 off, 1 on, 2 on with boost; -1 where the driver has no such thing.
    int mode() const { return offered_ ? g_settings.low_latency : -1; }
    // For the menu's top line: G-SYNC is following mpv's frames (1), is not (0), or the driver does not say (-1).
    int gsync() const { return offered_ ? gsync_ : -1; }
    // Windows composes mpv's window (1), flips its frames straight to the screen (0), or the driver does not say (-1).
    int composed() const { return offered_ ? composed_ : -1; }
    void set(int mode) {
        g_settings.low_latency = std::clamp(mode, 0, 2);
        reshade::set_config_value(nullptr, kSection, "LowLatency", name());
        if (offered_) apply();
    }

    // mpv called Present: the frame starts here, as far as the driver is told.
    void frame_start() {
        if (!marking()) return;
        // How long a new frame takes from the card to here (mpv's reading,
        // scaling and drawing of it), for the log: the part before the
        // driver's own timings start.
        if (g_link.linked()) {
            const LONG64 published = g_link.published();
            const double age = g_link.since_last(qpc_seconds());
            if (published != published_ && age >= 0 && age < 0.1) {
                arrival_sum_ += age;
                ++arrival_n_;
            }
            published_ = published;
        }
        ++frame_;
        nv_.mark(device_, glasir::NvLowLatency::SimulationStart, frame_);
        nv_.mark(device_, glasir::NvLowLatency::SimulationEnd, frame_);
        nv_.mark(device_, glasir::NvLowLatency::RenderSubmitStart, frame_);
        stage_ = 1;
    }
    // ReShade's effects and this add-on have drawn: the present itself is next.
    void before_present() {
        if (stage_ != 1) return;
        nv_.mark(device_, glasir::NvLowLatency::RenderSubmitEnd, frame_);
        nv_.mark(device_, glasir::NvLowLatency::PresentStart, frame_);
        stage_ = 2;
    }
    // mpv's present has returned.
    void presented() {
        if (stage_ == 2) nv_.mark(device_, glasir::NvLowLatency::PresentEnd, frame_);
        stage_ = 0;
    }
    // The frame's last present is queued: the driver's wait, and a line in
    // the log every 5 s. timings: the driver's timings may be logged (not
    // while NVIDIA's frame generation runs: its licence keeps performance
    // data of it out of what users send to others, and this log is sent).
    void frame_end(bool timings) {
        if (!device_ || !offered_) return;
        watch(qpc_seconds());
        if (active_) {
            const double before = qpc_seconds();
            nv_.sleep(device_);
            const double waited = qpc_seconds() - before;
            waited_sum_ += waited;
            waited_max_ = std::max(waited_max_, waited);
            ++waited_n_;
        }
        if (!marking() || !g_settings.log_stats) return;
        const double now = qpc_seconds();
        if (next_log_ == 0) next_log_ = now + 5.0;
        if (now < next_log_) return;
        next_log_ = now + 5.0;
        glasir::NvLowLatency::Status status;
        const bool known = nv_.status(device_, status);
        char timed[160] = "";
        glasir::NvLowLatency::Report report;
        if (timings && nv_.report(device_, report))
            snprintf(timed, sizeof(timed),
                     " | the driver's timings of %d frames: %.2f ms in the queue, %.2f ms on the GPU, %.2f ms from "
                     "mpv's present to drawn",
                     report.frames, report.queue_ms, report.gpu_ms, report.total_ms);
        char arrival[96] = "";
        if (arrival_n_ > 0)
            snprintf(arrival, sizeof(arrival), " | a new frame is at mpv's present %.2f ms after it arrives",
                     arrival_sum_ / arrival_n_ * 1000.0);
        // G-SYNC as the driver has it for mpv's window now (it only says
        // while the window is in front).
        char sync[64] = "";
        if (known)
            snprintf(sync, sizeof(sync), ", G-SYNC %s, %s", status.vrr ? "on" : "off",
                     status.direct_flip ? "flipped straight to the screen" : "composed");
        const double n = waited_n_ ? double(waited_n_) : 1.0;
        log_line(reshade::log::level::info,
                 "NVIDIA Reflex %s: the driver says low latency mode is %s%s | it waited %.3f ms avg, %.3f max after "
                 "each of %u frames%s%s",
                 name(), !known ? "unknown" : status.low_latency ? "on" : "off", sync, waited_sum_ / n * 1000.0,
                 waited_max_ * 1000.0, waited_n_, arrival, timed);
        waited_sum_ = waited_max_ = arrival_sum_ = 0;
        waited_n_ = arrival_n_ = 0;
    }

private:
    // Once a second: G-SYNC as the driver has it for mpv's window (it only says while the window is in
    // front). The first time it runs is kept (GsyncSeen), and mpv's scaling script is told that it runs
    // on this PC: the driver keeps G-SYNC off on a device RTX Video Super Resolution has run on, and the
    // script makes mpv's output anew when that scaling is left - only where there is G-SYNC to get back.
    void watch(double now) {
        if (now < next_watch_) return;
        next_watch_ = now + 1.0;
        glasir::NvLowLatency::Status status;
        if (!nv_.status(device_, status)) {
            gsync_ = composed_ = -1;
            return;
        }
        composed_ = status.direct_flip ? 0 : 1;
        if (status.vrr != (gsync_ == 1))
            log_line(reshade::log::level::info, "G-SYNC %s for mpv's window (the NVIDIA driver says)", status.vrr ? "on" : "off");
        gsync_ = status.vrr ? 1 : 0;
        if (status.vrr && !g_settings.gsync_seen) {
            g_settings.gsync_seen = true;
            reshade::set_config_value(nullptr, kSection, "GsyncSeen", "1");
        }
        if (g_settings.gsync_seen && !told_ && glasir::overlay::mpv_connected()) {
            glasir::overlay::mpv_command({"set", "user-data/glasir/gsync", "yes"});
            told_ = true;
        }
    }
    static const char *name() { return g_settings.low_latency >= 2 ? "boost" : g_settings.low_latency == 1 ? "on" : "off"; }
    bool marking() const { return device_ && offered_ && (active_ || measure_); }
    void apply() {
        const int mode = g_settings.low_latency;
        const int status = nv_.set_mode(device_, mode > 0, mode == 2);
        if (status != 0)
            log_line(reshade::log::level::warning,
                     "NVIDIA Reflex: the driver did not take low latency mode %s (NVAPI status %d)", name(), status);
        else
            log_line(reshade::log::level::info, "NVIDIA Reflex: low latency mode %s",
                     mode == 2 ? "on, with boost" : mode == 1 ? "on" : "off");
        active_ = status == 0 && mode > 0;
        next_log_ = 0;
        waited_sum_ = waited_max_ = arrival_sum_ = 0;
        waited_n_ = arrival_n_ = 0;
    }

    glasir::NvLowLatency nv_;
    ID3D11Device *device_ = nullptr;  // mpv's, which FrameGen holds
    bool offered_ = false;            // the driver has the mode for this device
    bool active_ = false;             // it was asked for and taken
    bool measure_ = false;            // GLASIR_LATENCY_LOG
    uint64_t frame_ = 0;
    int stage_ = 0;  // of the frame being presented: 1 = started, 2 = everything drawn
    double next_log_ = 0, waited_sum_ = 0, waited_max_ = 0;
    unsigned waited_n_ = 0;
    LONG64 published_ = -1;  // the link's new-frame count at the last present
    double next_watch_ = 0;
    int gsync_ = -1;     // as last read: 1 on, 0 off, -1 the driver did not say
    int composed_ = -1;  // likewise: 1 Windows composes the window, 0 its frames go straight to the screen
    bool told_ = false;  // mpv has user-data/glasir/gsync (it keeps it for as long as it runs)
    double arrival_sum_ = 0;
    unsigned arrival_n_ = 0;
};

LowLatency g_latency;

// When NVIDIA's software (Video Frame Generation, Video Super Resolution) last
// drew, and mpv's presents since. The driver's timings in the log are of the
// last 64 frames, and its licence keeps timings of its software out of what
// users send to others: they are left out until 64 frames and 5 s have gone
// by without it (a still picture presents only a few frames a second).
double g_nvidia_ran_at = -1e9;
unsigned g_presents_since_nvidia = ~0u;
void nvidia_ran() {
    g_nvidia_ran_at = qpc_seconds();
    g_presents_since_nvidia = 0;
}
bool nvidia_ran_lately() { return g_presents_since_nvidia < 64 || qpc_seconds() - g_nvidia_ran_at < 5.0; }

// ---- Direct3D 12 devices kept until mpv ends -------------------------------------
//
// mpv crashed in its last moments where NVIDIA Super Resolution (below) and
// DLSS 5 had both run: exit code 0xC0000005, and with an 8-bit capture no new
// start for the menu's Picture change (found 2026-11-19, the day 0.4.0 came
// out; src/framegen/README.md, NVIDIA Super Resolution, has how).
//
// Both use NVIDIA's NGX, of which a process has one: DLSS 5 through
// Direct3D 12, on a device its feeder makes; the super resolution through the
// Video Effects SDK. Where the SDK was the first to start NGX - at every start
// with scaling = nvidia, and when DLSS 5 is switched on later - one of NGX's
// model DLLs (NGX\models\dlssg) gives that device back once more than it took
// it, in its clean-up as the process ends. The device is long gone by then:
// the DLSS 5 add-ons let go of it when mpv's picture closes. With DLSS 5
// first there is no such release, nor with NVIDIA Video Frame Generation,
// which does not start NGX.
//
// None of that code is Glasir's. So from the moment the SDK is asked for the
// super resolution, every Direct3D 12 device made in mpv gets two references
// that are never given back: NGX's extra release then finds a device that is
// there, and (with two) does not end it either - nothing is taken apart while
// the process is. The devices go with the process. ReShade, which counts the
// references of the devices it wraps, says so in its log when the DLSS 5
// add-ons let go of theirs ("Reference count for ID3D12Device ... is
// inconsistent (2)"): that line is this.
class KeptDevices {
public:
    // ReShade's init_device and destroy_device, for Direct3D 12 devices.
    void made(IUnknown *device) {
        std::lock_guard lock(mutex_);
        if (keep_)
            hold(device);
        else
            known_.push_back(device);
    }
    void going(IUnknown *device) {
        std::lock_guard lock(mutex_);
        known_.erase(std::remove(known_.begin(), known_.end(), device), known_.end());
    }
    // The Video Effects SDK is about to start NGX: from here on, and for the devices there are.
    void keep() {
        std::lock_guard lock(mutex_);
        if (keep_) return;
        keep_ = true;
        for (IUnknown *device : known_) hold(device);
        known_.clear();
    }

private:
    void hold(IUnknown *device) {
        device->AddRef();
        device->AddRef();
        log_line(reshade::log::level::info,
                 "a Direct3D 12 device in mpv is kept until mpv ends: NVIDIA's NGX gives it back once too often then, "
                 "after its Video Super Resolution has run");
    }

    std::mutex mutex_;
    std::vector<IUnknown *> known_;  // made, still there, not held
    bool keep_ = false;
};

KeptDevices g_kept_devices;

// ---- NVIDIA Super Resolution ---------------------------------------------------
//
// video.scaling = nvidia: the capture is made as large as its place in the
// window by NVIDIA's Video Super Resolution (fg_sr.h), here, before ReShade's
// effects (DLSS 5) and frame generation - not by mpv.
//
// mpv has no place for it: its filters cannot run the SDK, and its own way to
// NVIDIA's super resolution (the GPU's video processor, video.scaling = vsr)
// has no choice of model and costs G-SYNC. So glasir_scaling.lua has mpv
// draw the picture as large as it comes, in the middle of the window
// (video-unscaled), and says so (user-data/glasir/sr). At mpv's present the
// back buffer then holds that small picture: it is copied out, made as large
// as fits the window, and drawn over the middle. Everything after that - the
// effects, the frame in between, the menu, a screenshot - finds a picture as
// large as mpv would have made it, and is told where it is
// (overlay::set_scaled_picture).
//
// "Scale to" (glasir.ini video.scale_to, user-data/glasir/target) is the size
// the effect makes the picture, whatever the window's: its picture then goes
// into a texture of that size, and fg_resample.h takes it from there to its
// place - down from a larger one (supersampling), up from a smaller one. So
// too where its place is more than the four times the effect goes to.
//
// The script is told whether this can be done at all ("script-message
// glasir-sr ready" or "unavailable"): until it hears "ready", and where it
// cannot be, FSR 1 scales in mpv as before. The effect is made (its model
// loaded) while FSR 1 still scales, from the sizes mpv reports, and "ready"
// is said only once it is there: no small picture while it loads, and none
// where it cannot run. A failure is for those sizes and that format: another
// size (the window resized, another Scale to) tries again.
//
// NVIDIA's licence keeps results of its software out of what users send to
// others: nothing here logs how long a frame took.
const char *sr_quality_name(fg::SrLevel level) {
    return level == fg::SrLevel::low ? "low" : level == fg::SrLevel::high ? "high" : level == fg::SrLevel::ultra ? "ultra" : "medium";
}

class Upscale {
    // The sizes and format one effect and its textures are made for.
    struct Key {
        UINT in_w = 0, in_h = 0, sr_w = 0, sr_h = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool direct = false;
        bool operator==(const Key &o) const {
            return in_w == o.in_w && in_h == o.in_h && sr_w == o.sr_w && sr_h == o.sr_h && format == o.format &&
                   direct == o.direct;
        }
    };

public:
    // mpv's present on its swap chain, before ReShade's effects.
    void on_present(reshade::api::swapchain *api_swapchain) {
        const glasir::overlay::Shown shown = glasir::overlay::shown();
        if (!shown.connected) return;
        if (shown.scaling != "nvidia") {
            if (wanted_) {  // the scaling was switched: its memory is the next one's
                wanted_ = false;
                told_ = 0;
                failed_ = Key{};
                mismatch_ = 0;
                sr_.release();
                let_go();
                glasir::overlay::set_scaled_picture(nullptr);
                running_ = false;
            }
            return;
        }
        wanted_ = true;
        reshade::api::device *api_device = api_swapchain->get_device();
        if (api_device->get_api() != reshade::api::device_api::d3d11) return tell(false, "mpv does not draw with Direct3D 11");
        auto *swapchain = reinterpret_cast<IDXGISwapChain *>(api_swapchain->get_native());
        auto *device = reinterpret_cast<ID3D11Device *>(api_device->get_native());
        if (!swapchain || !device) return;
        if (device != device_) {  // another device than the one all this was made on
            if (device_) release();
            device_ = device;
        }
        if (!installed_checked_) {
            installed_checked_ = true;
            installed_ = fg::Sr::runtime_present(vfg_runtime_folder());
        }
        if (!installed_) return tell(false, "it is not downloaded");

        running_ = false;
        glasir::overlay::set_scaled_picture(nullptr);
        if (shown.video_w < 16 || shown.video_h < 16) return;
        ComPtr<ID3D11Texture2D> back_buffer;
        D3D11_TEXTURE2D_DESC bb{};
        if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return;
        back_buffer->GetDesc(&bb);
        if (bb.SampleDesc.Count != 1) return;
        // The picture as it comes, from mpv; drawn as it comes it must fit the window whole (else the
        // script leaves the scaling to mpv).
        const UINT in_w = UINT(shown.video_w), in_h = UINT(shown.video_h);
        if (in_w > bb.Width || in_h > bb.Height) return;
        // Its place: as large as fits the window.
        const double fit = std::min(double(bb.Width) / in_w, double(bb.Height) / in_h);
        const UINT out_w = std::min(bb.Width, UINT(std::lround(in_w * fit)));
        const UINT out_h = std::min(bb.Height, UINT(std::lround(in_h * fit)));
        const UINT x = (bb.Width - out_w) / 2, y = (bb.Height - out_h) / 2;
        // What the effect makes: the target's size (as glasir_scaling.lua counts it), or its place; up
        // to the four times it goes to.
        UINT sr_w = out_w, sr_h = out_h;
        if (shown.target > 0) {
            sr_h = UINT(in_h * std::min(double(shown.target) / in_h, 4.0) / 2) * 2;
            sr_w = UINT(in_w * (double(sr_h) / in_h) / 2 + 0.5) * 2;
        } else if (fit > 4.0) {
            sr_w = in_w * 4;
            sr_h = in_h * 4;
        }
        // Nothing to make larger: the script has mpv scale again in a moment.
        if (sr_h <= in_h * 1.001) return;
        const bool direct = sr_w == out_w && sr_h == out_h;
        const Key key{in_w, in_h, sr_w, sr_h, bb.Format, direct};
        if (key == failed_) return;  // said when it failed; another size tries again
        bool ten_bit = false;
        if (!fg::Sr::takes(bb.Format, ten_bit)) return fail(key, "it does not take this picture format");

        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        std::string error;
        if (!sr_.ready() || sr_.in_width() != in_w || sr_.in_height() != in_h || sr_.out_width() != sr_w ||
            sr_.out_height() != sr_h || sr_.ten_bit() != ten_bit || format_ != bb.Format || direct != !made_) {
            let_go();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = in_w;
            d.Height = in_h;
            d.MipLevels = d.ArraySize = 1;
            d.Format = bb.Format;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&d, nullptr, &copy_)) ||
                FAILED(device->CreateShaderResourceView(copy_.Get(), nullptr, &copy_view_)))
                return fail(key, "no texture for the picture");
            if (!direct) {
                // The effect's picture at its own size, before it is taken to its place.
                d.Width = sr_w;
                d.Height = sr_h;
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                if (FAILED(device->CreateTexture2D(&d, nullptr, &made_)) ||
                    FAILED(device->CreateShaderResourceView(made_.Get(), nullptr, &made_view_)) ||
                    FAILED(device->CreateRenderTargetView(made_.Get(), nullptr, &made_target_)))
                    return fail(key, "no texture for the larger picture");
                if (!to_place_.ready() && !to_place_.create(device, error)) return fail(key, error.c_str());
            }
            g_kept_devices.keep();  // before the SDK starts NGX (KeptDevices, above)
            if (!sr_.create(device, in_w, in_h, sr_w, sr_h, ten_bit, vfg_runtime_folder(), g_settings.sr_level, error))
                return fail(key, error.c_str());
            format_ = bb.Format;
            char then[64] = "";
            if (!direct) snprintf(then, sizeof(then), ", then %ux%u for its place", out_w, out_h);
            log_line(reshade::log::level::info,
                     "NVIDIA Video Super Resolution %s: the %ux%u picture is made %ux%u%s (quality %s, %d-bit)",
                     sr_.version().c_str(), in_w, in_h, sr_w, sr_h, then, sr_quality_name(g_settings.sr_level),
                     ten_bit ? 10 : 8);
        }
        // The effect is there: the script may have mpv draw the picture as it comes.
        tell(true, nullptr);
        if (!shown.unscaled) {  // mpv still scales it (until the script has heard)
            mismatch_ = 0;
            return;
        }
        // Where mpv drew it, as it comes: its box, a pixel or two off at most (mpv's margins are
        // rounded); else mpv is not there yet, or no longer.
        const D3D11_BOX box = glasir::overlay::mpv_picture_box(bb.Width, bb.Height);
        const int dw = int(box.right - box.left) - int(in_w), dh = int(box.bottom - box.top) - int(in_h);
        if (std::abs(dw) > 2 || std::abs(dh) > 2) {
            // For two seconds on end: mpv draws it somewhere else than expected, and keeps doing so.
            if (++mismatch_ >= 120) fail(key, "mpv's picture is not where it was expected");
            return;
        }
        mismatch_ = 0;
        const UINT left = std::min(box.left, bb.Width - in_w), top = std::min(box.top, bb.Height - in_h);
        const D3D11_BOX from{left, top, 0, left + in_w, top + in_h, 1};
        ComPtr<ID3D11RenderTargetView> target;
        if (FAILED(device->CreateRenderTargetView(back_buffer.Get(), nullptr, &target)))
            return fail(key, "no view of mpv's frame");
        ctx->CopySubresourceRegion(copy_.Get(), 0, 0, 0, 0, back_buffer.Get(), 0, &from);
        {
            StateBackup backup(ctx.Get());
            if (direct) {
                if (!sr_.draw(ctx.Get(), copy_view_.Get(), target.Get(), x, y, error)) return fail(key, error.c_str());
            } else {
                if (!sr_.draw(ctx.Get(), copy_view_.Get(), made_target_.Get(), 0, 0, error))
                    return fail(key, error.c_str());
                to_place_.draw(ctx.Get(), made_view_.Get(), sr_w, sr_h, target.Get(), x, y, out_w, out_h);
            }
        }
        const D3D11_BOX to{x, y, 0, x + out_w, y + out_h, 1};
        glasir::overlay::set_scaled_picture(&to);
        running_ = true;
        nvidia_ran();
    }

    // mpv's swap chain is going: what was made on its device goes with it.
    void release() {
        sr_.release();
        let_go();
        to_place_.release();
        format_ = DXGI_FORMAT_UNKNOWN;
        running_ = false;
        told_ = 0;          // the script is told again on the next device
        failed_ = Key{};    // and it is tried again there
        mismatch_ = 0;
        device_ = nullptr;
        glasir::overlay::set_scaled_picture(nullptr);
    }

    // For the menu: the model (0 low .. 3 ultra), and whether it runs.
    int level() const { return int(g_settings.sr_level) - int(fg::SrLevel::low); }
    void set_level(int level) {
        g_settings.sr_level = fg::SrLevel(int(fg::SrLevel::low) + std::clamp(level, 0, 3));
        reshade::set_config_value(nullptr, kSection, "SrQuality", sr_quality_name(g_settings.sr_level));
        sr_.set_level(g_settings.sr_level);
        log_line(reshade::log::level::info, "SrQuality=%s (from the menu)", sr_quality_name(g_settings.sr_level));
    }
    int state() const { return running_ ? 1 : (wanted_ && told_ < 0 ? -1 : 0); }

private:
    // Tells glasir_scaling.lua, once for each change, whether the picture can be scaled here.
    void tell(bool ready, const char *why) {
        const int now = ready ? 1 : -1;
        if (told_ == now) return;
        told_ = now;
        glasir::overlay::mpv_command({"script-message", "glasir-sr", ready ? "ready" : "unavailable"});
        if (!ready) {
            glasir::overlay::set_scaled_picture(nullptr);
            running_ = false;
            if (why)
                log_line(reshade::log::level::warning,
                         "NVIDIA Video Super Resolution cannot scale the picture (%s); FSR 1 does, in mpv", why);
        }
    }
    // It cannot be done at these sizes and this format: FSR 1 scales until they change.
    void fail(const Key &key, const char *why) {
        failed_ = key;
        mismatch_ = 0;
        sr_.release();
        let_go();
        if (told_ < 0)  // already said: this is another size that cannot be done either
            log_line(reshade::log::level::warning, "NVIDIA Video Super Resolution cannot scale the picture (%s)", why);
        tell(false, why);
    }
    // The textures of one size.
    void let_go() {
        copy_.Reset();
        copy_view_.Reset();
        made_.Reset();
        made_view_.Reset();
        made_target_.Reset();
    }

    fg::Sr sr_;
    fg::Resample to_place_;         // the effect's picture to its place, where the two differ in size
    ComPtr<ID3D11Texture2D> copy_;  // the picture as mpv drew it
    ComPtr<ID3D11ShaderResourceView> copy_view_;
    ComPtr<ID3D11Texture2D> made_;  // the effect's picture, where it is not drawn straight into its place
    ComPtr<ID3D11ShaderResourceView> made_view_;
    ComPtr<ID3D11RenderTargetView> made_target_;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    bool wanted_ = false;             // the scaling chosen is this one
    bool installed_checked_ = false, installed_ = false;
    Key failed_;                      // it failed at these: not again until they change
    int mismatch_ = 0;                // frames on end where mpv's picture was not where expected
    ID3D11Device *device_ = nullptr;  // what all this was made on (not held)
    bool running_ = false;            // the frame being presented was scaled here
    int told_ = 0;                    // what the script was last told: 1 ready, -1 unavailable, 0 nothing yet
};

Upscale g_upscale;

// How a real frame B reaches the screen with a frame in between:
//
//   reshade_present  B is finished in the back buffer (the DLSS 5 pass ran
//                    during ReShade's effects, before this). Copy B out, draw
//                    the frame between A and B over it, return: mpv's own
//                    present shows the frame in between right away.
//   finish_present   That present has returned. Copy B back into the back
//                    buffer and present it through the original swap chain,
//                    held on the GPU until half an interval after the frame
//                    in between was done (Hold), and return.
//
// Why from when the frame in between is done: with VRR the screen shows a
// frame as soon as the GPU has drawn it, and the frame in between waits for
// the DLSS 5 pass and its own drawing - about 7 ms, nearly half an interval at
// 60 fps. Timed from mpv's present, B came right behind it: 5 then 11.5 ms on
// screen instead of 8.3 and 8.3. Held on the GPU rather than by sleeping here,
// because mpv's thread must be free for the next frame: from a frame's arrival
// to here takes mpv about 3 ms, so at 60 fps the next one arrives before B is
// due. B is let go before the next frame's DLSS 5 pass reaches the GPU (behind
// the wait it would be held too).
//
// Without the hold (no Direct3D 12, or GLASIR_FRAMEGEN_PACING=present for
// comparisons) finish_present sleeps until half an interval after B arrived
// and presents B then. The sleep happens there on purpose: during
// reshade_present ReShade holds its own lock and the device's multithread
// lock, so a sleep there would stall every other thread using the device.
//
// When it generates. The interval is the source's: with the link, the mean
// time between new frames from the card, pauses left out; without it, the
// median between mpv's frames (which, while generating, follow this add-on's
// own pace - so only the link can tell that the display is too slow for twice
// the source rate). A frame in between is only made when
//   - twice the source rate fits the display,
//   - fewer than two presents are still queued (else it would only add delay),
//   - the source is not paused (a still picture: only keepalive repeats),
//   - mpv is keeping up (watch_drops): drops caused while generating pause
//     generation for a while, longer each time; drops of mpv's own make it
//     wait until mpv has settled.
class FrameGen {
public:
    void on_reshade_present(reshade::api::effect_runtime *runtime);
    void on_finish_present(reshade::api::swapchain *swapchain);
    // For the in-game menu (src/overlay), on mpv's render thread like the rest.
    bool ready() const { return !broken_ && source_ok(); }
    void set_enabled(bool on);
    void set_fast(bool fast);
    glasir::overlay::FramegenStatus status() const {
        glasir::overlay::FramegenStatus s;
        const double now = qpc_seconds();
        if (now < paused_until_) {
            s.kind = s.Paused;
            s.seconds_left = paused_until_ - now;
        } else if (interval_ > 0 && max_hz_ > 0) {
            const int shown = fitting(interval_, max_hz_, g_settings.multiplier);  // what the display can show
            const int frames = std::min(shown, limit_);                            // ... and the GPU keeps up with
            s.display_hz = max_hz_;
            s.frames = frames;
            s.wanted = g_settings.multiplier;
            if (shown < 2) {
                s.kind = s.TooFast;
            } else if (frames < shown) {
                s.kind = s.Behind;
            } else if (shown < g_settings.multiplier) {
                s.kind = s.Fewer;
            } else if (frames > 2 && made_ >= 0 && made_ < 0.85 * (frames - 1)) {
                s.kind = s.Short;
                s.fps = (1.0 + made_) / interval_;
                s.target_fps = frames / interval_;
            }
        }
        return s;
    }
    void rates(double &source, double &real, double &generated) const {
        source = source_interval_ > 0 ? 1.0 / source_interval_ : 0;
        real = real_fps_;
        generated = generated_fps_;
    }
    // For the panel on ReShade's Add-ons page (draw_panel() below), on mpv's
    // render thread like the rest: the switches, changed while playing.
    // NVIDIA's filter in place of interp_, and its model's size (the menu).
    void set_vfg(bool on);
    // 2, 3 or 4 frames for each of the source's (the menu).
    void set_multiplier(int frames);
    void set_vfg_quality(int quality);
    void set_history_search(bool on);
    void set_picture_only(bool on);
    void set_edge_rules(bool on);
    void set_still_hud(bool on);
    bool broken() const { return broken_; }
    // Method=vfg, for the panel: 1 = NVIDIA's filter makes the frames, 0 = not
    // started yet, -1 = it could not (ReShade.log says why): Glasir's own does.
    int vfg_state() const { return vfg_.ready() && !vfg_broken_ ? 1 : (vfg_broken_ || vfg_failed_ ? -1 : 0); }
    // What the generator works on: the back buffer's size (0: none yet) and
    // the picture's place in it (all 0: the whole of it).
    void working_on(UINT &width, UINT &height, fg::Picture &picture) const {
        width = frames_[0] ? width_ : 0;
        height = frames_[0] ? height_ : 0;
        picture = picture_made_;
    }
    void on_effects_finished(reshade::api::effect_runtime *runtime) {
        if (mpv_swapchain_ && runtime->get_native() != mpv_swapchain_) return;  // another runtime's effects
        effects_ran_ = true;
    }
    // ReShade's present event, before its effects: the time of mpv's present.
    // Other swap chains (the DLSS 5 add-ons', a third-party tool's) are not
    // mpv's frame and must not move it.
    void on_mpv_present(reshade::api::swapchain *swapchain) {
        if (mpv_swapchain_ && swapchain->get_native() != mpv_swapchain_) return;
        mpv_presented_ = qpc_seconds();
        presented_color_space_ = swapchain->get_color_space();
        g_latency.frame_start();
        if (mpv_swapchain_) g_upscale.on_present(swapchain);  // mpv's own, once it is known
    }
    // This native swap chain is mpv's (known from its first reshade_present on).
    bool is_mpv(uint64_t native) const { return mpv_swapchain_ != 0 && native == mpv_swapchain_; }
    // NVIDIA's filter is making the frames in between.
    bool vfg_running() const { return g_settings.vfg && vfg_state() > 0; }

    bool owns(reshade::api::device *device) const {
        return device_ && device->get_api() == reshade::api::device_api::d3d11 &&
               reinterpret_cast<ID3D11Device *>(device->get_native()) == device_.Get();
    }

    void release_held() { hold_.release_all(); }

    void release() {
        capture_.reset(false);
        for (auto &t : frames_) t.Reset();
        for (auto &v : views_) v.Reset();
        interp_.release();
        vfg_.release();
        vfg_failed_ = vfg_broken_ = false;
        hold_.stop();
        g_latency.stop();
        context_.Reset();
        device_.Reset();
        reset_history();
        pending_ = false;
        width_ = height_ = 0;
        failed_w_ = failed_h_ = 0;
        broken_ = false;
        // The next swap chain's first present is taken as mpv's, as at start,
        // so its colour space is read from that very present.
        mpv_swapchain_ = 0;
        color_space_ = presented_color_space_ = reshade::api::color_space::unknown;
    }

private:
    bool init(ID3D11Device *device);
    bool ensure_frames(const D3D11_TEXTURE2D_DESC &bb);
    void follow_picture(const D3D11_TEXTURE2D_DESC &bb);
    bool use_vfg();
    bool take_vfg_mode(const char *why);
    bool draw_between(ID3D11Texture2D *back_buffer, int a, int b, bool follows, bool keep, int index, int frames);
    // Multiplier, or the most frames for each of the source's that the
    // display can show; 1: not even two.
    static int fitting(double interval, int max_hz, int most) {
        for (int frames = most; frames >= 2; --frames)
            if (frames / interval <= max_hz * 1.05) return frames;
        return 1;
    }
    // A setting of frame generation changed: what mpv could not keep up with
    // may be gone with it, so it is tried at once, not after the pause - and
    // with all the frames chosen again.
    void retry_now() {
        paused_until_ = settle_until_ = 0;
        backoff_ = 5.0;
        drop_times_[0] = drop_times_[1] = -1e9;
        all_frames_again();
        limit_wait_ = kLimitWait;
        limit_said_ = 4;
    }
    // 3x and 4x step down while the GPU cannot hold them (on_reshade_present):
    // back to what was chosen, to see whether it holds now.
    void all_frames_again() {
        limit_ = 4;
        strain_ = 0;
        made_ = -1;
    }
    // `frames` for each real frame cannot be held (why): one fewer, for a
    // while. Said in a toast the first time, in the log every time, and in the
    // menu for as long as it lasts.
    void fewer_frames(int frames, double now, const char *why) {
        limit_ = std::max(2, frames - 1);
        limit_until_ = now + limit_wait_;
        limit_neural_ = neural_now_;
        strain_ = 0;
        made_ = -1;
        log_line(reshade::log::level::warning, "%dx cannot be held (%s): %dx for the next %.0f s", frames, why, limit_,
                 limit_wait_);
        if (limit_ < limit_said_) {  // not again at every later try
            limit_said_ = limit_;
            char text[96];
            snprintf(text, sizeof(text), "Frame generation: %dx (the GPU cannot hold %dx here)", limit_,
                     g_settings.multiplier);
            glasir::overlay::toast(text, 3.0);
        }
        limit_wait_ = std::min(limit_wait_ * 2.0, 300.0);
    }
    void reset_history() {
        prev_ = -1;
        dt_interval_ = 0;
        intervals_seen_ = 0;
    }
    void push_interval(double dt) {
        recent_[intervals_seen_ % kRecent] = dt;
        ++intervals_seen_;
        const int n = std::min(intervals_seen_, kRecent);
        double sorted[kRecent];
        std::copy(recent_, recent_ + n, sorted);
        std::nth_element(sorted, sorted + n / 2, sorted + n);
        dt_interval_ = sorted[n / 2];
    }
    bool source_ok() const { return !g_settings.require_dedup || g_link.linked(); }
    // Whether the back buffer holds HDR10 (PQ) values: 10-bit with the PQ
    // colour space. The HDR launcher has mpv write PQ; the SDR one gets a
    // 10-bit back buffer too on a 10-bit desktop, with SDR values. Anything
    // else (gamma 2.2 BT.2020, which ReShade calls hdr10_hlg, or unknown) is
    // not PQ either.
    bool pq() const {
        return format_ == DXGI_FORMAT_R10G10B10A2_UNORM && color_space_ == reshade::api::color_space::hdr10_pq;
    }
    void toggle(reshade::api::effect_runtime *runtime);
    void use_edge_rules() {
        interp_.edge_cost = g_settings.edge_rules ? 0.01f : -1.0f;
        interp_.edge_explains = g_settings.edge_rules ? 0.0f : 1.0f;
    }
    void use_still_hud() {
        interp_.hud_detail = g_settings.still_hud ? 0.04f : 0.0f;
        if (g_settings.still_hud && !still_hud_logged_)
            log_line(reshade::log::level::info, "StillHud=1: cells that hold a HUD over moving scenery stand still");
        still_hud_logged_ = g_settings.still_hud;
    }
    void watch_drops(double now, bool stalled);
    void stats(double now);
    TraceLine trace_line(double now, double dt, int queued, const char *what) const {
        TraceLine l;
        l.now = now;
        l.dt = dt;
        l.interval = interval_;
        l.source_interval = source_interval_;
        l.queued = queued;
        l.event = what;
        l.source_age = source_age_;
        l.effects = effects_;
        return l;
    }
    void trace(double now, double dt, int queued, const char *what, double real = -1) {
        if (!g_trace) return;
        TraceLine l = trace_line(now, dt, queued, what);
        l.real = real;
        write_trace(l);
    }
    void fail(const char *what, HRESULT hr) {
        log_line(reshade::log::level::error, "%s failed (0x%08lX); frame generation stays off", what, hr);
        broken_ = true;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    fg::Interpolator interp_;  // makes the frame in between (fg_interp.h)
    // Method=vfg: NVIDIA's filter makes it instead (fg_vfg.h), for the back
    // buffer and picture interp_ is made for. It did not start for this size,
    // format and picture (vfg_failed_: tried again when one changes), or failed
    // while drawing (vfg_broken_: not again until the device goes): interp_
    // makes the frames, which is why it is always made too.
    fg::Vfg vfg_;
    DXGI_FORMAT vfg_format_ = DXGI_FORMAT_UNKNOWN;
    bool vfg_failed_ = false, vfg_broken_ = false;
    UINT vfg_failed_w_ = 0, vfg_failed_h_ = 0;
    DXGI_FORMAT vfg_failed_format_ = DXGI_FORMAT_UNKNOWN;
    fg::Picture vfg_failed_picture_;
    int vfg_profile_ = -1, vfg_stage_n_ = 0;  // GLASIR_VFG_PROFILE: -1 = not read yet
    double vfg_stage_sum_[3] = {};

    // The last two real frames, as copied out of the back buffer, and whether
    // ReShade's effects ran for each. Frames are only mixed with frames of the
    // same kind, so a frame from while the effects were reloading or switched
    // off is not mixed with a neural one. (That the effects ran does not prove
    // the DLSS 5 pass delivered: the feeder can skip a frame and say nothing.)
    ComPtr<ID3D11Texture2D> frames_[2];
    ComPtr<ID3D11ShaderResourceView> views_[2];
    bool neural_[2] = {};
    UINT width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    // The swap chain's colour space as ReShade tracks it (mpv sets it), read
    // at mpv's present (the effect runtime does not have it).
    reshade::api::color_space color_space_ = reshade::api::color_space::unknown;
    reshade::api::color_space presented_color_space_ = reshade::api::color_space::unknown;
    UINT failed_w_ = 0, failed_h_ = 0;  // the size that last failed, not retried until it changes
    DXGI_FORMAT failed_format_ = DXGI_FORMAT_UNKNOWN;
    int prev_ = -1;  // which of frames_ holds the previous real frame
    bool effects_ran_ = false;

    static constexpr int kRecent = 15;
    double last_real_ = 0;
    double dt_interval_ = 0;      // median time between mpv's recent frames
    double source_interval_ = 0;  // mean time between the source's new frames (link), pauses left out
    double interval_ = 0;         // the one in use
    double recent_[kRecent] = {};
    int intervals_seen_ = 0;

    // A real frame still to present after the frame in between.
    bool pending_ = false;
    // HistorySearch: interp_ holds the motion of the pair that ended on the last real frame, found at the
    // source's pace - what the next pair's search may start from. Any present without such a pair drops it.
    bool history_fresh_ = false;
    bool history_logged_ = false;
    bool still_hud_logged_ = false;
    // PictureOnly: where mpv has the picture in the back buffer (all 0: it fills it) - as last seen, for how
    // many presents in a row, what interp_ was made for, and one it could not be made for.
    fg::Picture picture_seen_, picture_made_, picture_failed_;
    int picture_steady_ = 0;
    int pending_slot_ = 0;
    int pending_frames_ = 2;  // frames shown for this real frame, itself included (Multiplier, or fewer)
    // 3x and 4x the GPU cannot hold (the frames of one real frame still queued when the next comes, which
    // then gets none): limit_ is the most frames shown for now, strain_ counts such frames, the limit is
    // lifted for a try at limit_until_, each time after twice as long (limit_wait_), or when ReShade's
    // effects start or stop running (limit_neural_: DLSS 5 switched in the menu). made_: frames in between
    // for each real frame lately.
    static constexpr double kLimitWait = 20.0;
    int limit_ = 4, strain_ = 0;
    double limit_until_ = 0, limit_wait_ = kLimitWait;
    bool limit_neural_ = false;
    bool neural_now_ = false;  // ReShade's effects ran for the frame being presented
    int limit_said_ = 4;       // the lowest limit a toast has told of since a setting changed
    double made_ = -1;
    double pending_target_ = 0, pending_since_ = 0;
    double pending_now_ = 0, pending_dt_ = 0, pending_half_ = 0, pending_latest_ = 0;
    int pending_queued_ = -1;
    IDXGISwapChain *pending_swapchain_ = nullptr;
    bool generated_last_ = false;

    Hold hold_;                    // holds the real frame on the GPU until its time
    bool hold_wanted_ = false;     // start it at the next finish_present (outside ReShade's locks)
    double source_age_ = -1;       // since the source delivered its latest frame, -1 without the link
    uint64_t mpv_swapchain_ = 0;   // native handle of mpv's swap chain, once seen
    double mpv_presented_ = 0;     // when mpv called Present, before ReShade's effects
    double effects_ = -1;          // from then to reshade_present (the DLSS 5 pass and the rest)
    LONG64 last_published_ = -1;   // the link's new-frame count at the last reshade_present
    double arrival_to_present_ = -1;  // from a new frame's arrival to mpv's present, the latest seen

    // Backing off when mpv cannot keep up.
    LONG64 last_dropped_ = -1;
    double drop_times_[2] = {-1e9, -1e9};
    double paused_until_ = 0, backoff_ = 5.0, settle_until_ = 0;
    int max_hz_ = 0;  // the most the display shows, as last read (for the menu)

    bool broken_ = false;
    double real_fps_ = 0, generated_fps_ = 0;  // over the last stats window, for the menu
    int hz_ = 0;
    double next_hz_try_ = 0;
    Sleeper sleeper_;
    Capture capture_;
    double capture_after_ = -1;  // GLASIR_FRAMEGEN_CAPTURE_AFTER: when, 0 = none, -1 = not read yet
    double toggle_after_ = -1;   // GLASIR_FRAMEGEN_TOGGLE_AFTER: the same

    // Stats since the last line.
    double stats_start_ = 0;
    unsigned real_ = 0, generated_ = 0, skipped_fast_ = 0, skipped_mixed_ = 0, skipped_queue_ = 0,
             skipped_paused_ = 0, skipped_backoff_ = 0, skipped_settle_ = 0, late_frames_ = 0, drops_ = 0;
    double late_max_ = 0, late_sum_ = 0, mpv_present_max_ = 0, mpv_present_sum_ = 0;
    double out_max_ = 0;  // the real frame's copy and present on mpv's thread, held
    unsigned vfg_generated_ = 0;  // frames in between that NVIDIA's filter made
    // VfgQuality=auto: the choice of model, and how long the last pair took - from its real frame's
    // arrival at Glasir until its frames in between were drawn (-1: it had none).
    fg::VfgAuto vfg_pick_;
    double vfg_busy_ = -1;
    bool vfg_use_failed_ = false;  // a model could not be made: said once
    int vfg_pick_frames_ = 0;      // the frames a pair had while vfg_pick_ learned (0: none yet)
};

FrameGen g_gen;

bool FrameGen::init(ID3D11Device *device) {
    device_ = device;
    device->GetImmediateContext(&context_);
    trace_open();
    char pacing[16] = {};
    const DWORD len = GetEnvironmentVariableA("GLASIR_FRAMEGEN_PACING", pacing, sizeof(pacing));
    hold_wanted_ = !(len > 0 && len < sizeof(pacing) && _stricmp(pacing, "present") == 0);
    if (!hold_wanted_)
        log_line(reshade::log::level::info, "GLASIR_FRAMEGEN_PACING=present: the real frame is timed from mpv's present");
    log_line(reshade::log::level::info, "ready: feature level 0x%X, %s, toggle key %d%s", device->GetFeatureLevel(),
             g_settings.enabled ? "on" : "off", g_settings.toggle_key,
             g_settings.require_dedup ? ", waiting for the link from Glasir's dedup" : "");
    g_latency.start(device);
    return true;
}

// The frame copies and the frame generator for this back buffer size. A
// failure here is tied to the size (allocation during a resize): it is logged
// once and tried again when the size changes, without turning frame
// generation off for good.
bool FrameGen::ensure_frames(const D3D11_TEXTURE2D_DESC &bb) {
    if (frames_[0] && bb.Width == width_ && bb.Height == height_ && bb.Format == format_) return true;
    if (bb.Width == failed_w_ && bb.Height == failed_h_ && bb.Format == failed_format_) return false;
    if (capture_.active()) capture_.reset();  // its copies are of the old size
    for (auto &t : frames_) t.Reset();
    for (auto &v : views_) v.Reset();
    reset_history();
    width_ = height_ = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = bb.Width;
    desc.Height = bb.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = bb.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = S_OK;
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
        hr = device_->CreateTexture2D(&desc, nullptr, &frames_[i]);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(frames_[i].Get(), nullptr, &views_[i]);
    }
    std::string error;
    if (FAILED(hr)) {
        char text[64];
        snprintf(text, sizeof(text), "frame copies: 0x%08lX", static_cast<unsigned long>(hr));
        error = text;
    } else if (!interp_.create(device_.Get(), bb.Width, bb.Height, error)) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        for (auto &t : frames_) t.Reset();
        for (auto &v : views_) v.Reset();
        failed_w_ = bb.Width;
        failed_h_ = bb.Height;
        failed_format_ = bb.Format;
        log_line(reshade::log::level::error,
                 "the frame generator did not start at %ux%u (%s); trying again at the next size", bb.Width,
                 bb.Height, error.c_str());
        return false;
    }
    if (g_settings.fast) {
        // The 2x2 cells around each pixel, compared on the pixel alone.
        interp_.warp_ring = 2;
        interp_.warp_taps = 1;
        interp_.soft_temp = 0.02f;
    }
    interp_.hist_search = g_settings.history_search ? 1 : 0;
    use_edge_rules();
    use_still_hud();
    picture_made_ = picture_failed_ = fg::Picture{};  // made for the whole frame: follow_picture() takes it from here
    picture_steady_ = 0;
    failed_w_ = failed_h_ = 0;
    width_ = bb.Width;
    height_ = bb.Height;
    format_ = bb.Format;
    log_line(reshade::log::level::info, "back buffer %ux%u, format %d", width_, height_, format_);
    return true;
}

// PictureOnly: makes the generator for the picture's part of the back buffer once mpv has had the picture in
// the same place for a quarter of a second of presents (a window being resized moves it with every frame,
// and each change makes the generator's textures anew).
void FrameGen::follow_picture(const D3D11_TEXTURE2D_DESC &bb) {
    constexpr int kSteady = 15;
    fg::Picture now;
    if (g_settings.picture_only) {
        const D3D11_BOX box = glasir::overlay::picture_box(bb.Width, bb.Height);
        if (box.right - box.left < bb.Width || box.bottom - box.top < bb.Height) now = {box.left, box.top, box.right, box.bottom};
    }
    if (!(now == picture_seen_)) {
        picture_seen_ = now;
        picture_steady_ = 0;
    } else if (picture_steady_ < kSteady) {
        ++picture_steady_;
    }
    // picture_failed_ is all 0 when none failed, which is also "the whole frame": back to that it always goes.
    if (now == picture_made_ || (now.x1 != 0 && now == picture_failed_) || picture_steady_ < kSteady) return;
    std::string error;
    if (interp_.create(device_.Get(), bb.Width, bb.Height, error, now)) {
        picture_made_ = now;
        picture_failed_ = fg::Picture{};
        if (now.x1 != 0)
            log_line(reshade::log::level::info, "PictureOnly=1: the picture is %ux%u at %u,%u of the %ux%u back buffer",
                     now.x1 - now.x0, now.y1 - now.y0, now.x0, now.y0, bb.Width, bb.Height);
        else if (g_settings.picture_only)
            log_line(reshade::log::level::info, "PictureOnly=1: the picture fills the back buffer");
        else
            log_line(reshade::log::level::info, "PictureOnly=0: the whole %ux%u back buffer again", bb.Width, bb.Height);
    } else {
        log_line(reshade::log::level::error, "PictureOnly=1: no generator for the picture alone (%s); the whole frame it is",
                 error.c_str());
        picture_failed_ = now;
        picture_made_ = fg::Picture{};
        if (!interp_.create(device_.Get(), bb.Width, bb.Height, error)) {
            // As a failure in ensure_frames(): tried again at the next size.
            for (auto &t : frames_) t.Reset();
            for (auto &v : views_) v.Reset();
            failed_w_ = bb.Width;
            failed_h_ = bb.Height;
            failed_format_ = bb.Format;
            width_ = height_ = 0;
            log_line(reshade::log::level::error, "the frame generator did not start at %ux%u (%s); trying again at the next size",
                     bb.Width, bb.Height, error.c_str());
        }
    }
    if (capture_.active()) capture_.reset();
    reset_history();
}

// Method=vfg: NVIDIA's filter for this back buffer and picture, made at the
// first frame in between that needs it (its model takes a moment to load).
// False: interp_ makes the frame; why is logged once for each size.
bool FrameGen::use_vfg() {
    if (!g_settings.vfg || vfg_broken_) return false;
    if (vfg_.ready() && vfg_.width() == width_ && vfg_.height() == height_ && vfg_format_ == format_ &&
        vfg_.picture() == picture_made_)
        return true;
    if (vfg_failed_ && vfg_failed_w_ == width_ && vfg_failed_h_ == height_ && vfg_failed_format_ == format_ &&
        vfg_failed_picture_ == picture_made_)
        return false;
    std::string error;
    bool ten_bit = false;
    if (!fg::Vfg::takes(format_, ten_bit))
        error = "it does not take this picture format";
    else if (vfg_.create(device_.Get(), width_, height_, ten_bit, vfg_runtime_folder(), g_settings.vfg_mode,
                         g_settings.vfg_auto_cut, error, picture_made_)) {
        vfg_format_ = format_;
        vfg_failed_ = false;
        vfg_pick_.reset(g_settings.vfg_mode);  // what a model takes is another matter at another size
        vfg_busy_ = -1;
        const fg::Picture &p = picture_made_;
        log_line(reshade::log::level::info,
                 "Method=vfg: NVIDIA Video Frame Generation %s makes the frames in between (quality %s%s, %ux%u, %d-bit)",
                 vfg_.version().c_str(), g_settings.vfg_auto ? "auto, to begin with " : "", vfg_mode_name(),
                 p.x1 ? p.x1 - p.x0 : width_, p.x1 ? p.y1 - p.y0 : height_, ten_bit ? 10 : 8);
        return true;
    }
    vfg_failed_ = true;
    vfg_failed_w_ = width_;
    vfg_failed_h_ = height_;
    vfg_failed_format_ = format_;
    vfg_failed_picture_ = picture_made_;
    log_line(reshade::log::level::warning,
             "Method=vfg: NVIDIA Video Frame Generation did not start (%s); Glasir's own frame generation makes the frames",
             error.c_str());
    return false;
}

// Draws a frame in between frames_[a] and frames_[b] into the back buffer:
// `index` of the `frames - 1` the pair gets (the only one at 2x, half-way).
// follows: the last call drew the pair that ended on frames_[a] and kept its
// motion; keep: this pair's motion is worth keeping for the next one.
bool FrameGen::draw_between(ID3D11Texture2D *back_buffer, int a, int b, bool follows, bool keep, int index, int frames) {
    ComPtr<ID3D11RenderTargetView> rtv;
    HRESULT hr = device_->CreateRenderTargetView(back_buffer, nullptr, &rtv);
    if (FAILED(hr)) return fail("CreateRenderTargetView", hr), false;

    fg::DrawOptions o;
    o.method = g_settings.method;
    o.index = index;
    o.multiplier = frames;
    o.t = float(index) / float(frames);
    o.same_pair = index > 1;  // the frames of the draw before, for the pair's next frame
    o.follows = follows;
    o.marker = g_settings.marker;
    o.marker_level = 0.75f;                                                // sRGB-encoded
    if (pq()) o.marker_level = 0.5f;                                       // PQ, about 90 nits
    if (format_ == DXGI_FORMAT_R16G16B16A16_FLOAT) o.marker_level = 1.0f;  // scRGB, 80 nits

    StateBackup backup(context_.Get());
    if (use_vfg()) {
        std::string error;
        const double before = qpc_seconds();
        if (vfg_.draw(context_.Get(), views_[a].Get(), views_[b].Get(), rtv.Get(), o, error)) {
            ++vfg_generated_;
            nvidia_ran();
            // For VfgQuality=auto: the pair's first frame in between is done this long after its real
            // frame arrived (the filter's call returns when it is done, the DLSS 5 pass before it too);
            // the pair's other frames add their own time.
            const double took = qpc_seconds() - before;
            if (index <= 1)
                vfg_busy_ = std::max(arrival_to_present_, 0.0) + std::max(effects_, 0.0) + took;
            else if (vfg_busy_ >= 0)
                vfg_busy_ += took;
            // GLASIR_VFG_PROFILE=1 (a developer's look, never in a user's log): where the calling
            // thread's time went, averaged over 150 frames in between.
            if (vfg_profile_ < 0) vfg_profile_ = GetEnvironmentVariableW(L"GLASIR_VFG_PROFILE", nullptr, 0) > 0 ? 1 : 0;
            if (vfg_profile_ > 0) {
                for (int i = 0; i < 3; ++i) vfg_stage_sum_[i] += vfg_.stage_ms()[i];
                if (++vfg_stage_n_ == 150) {
                    log_line(reshade::log::level::info,
                             "GLASIR_VFG_PROFILE: per frame in between, waiting for the buffers %.2f ms, the filter %.2f ms, "
                             "handing them back %.2f ms | model %s, the last pair %.2f ms from arrival to drawn, of %.2f",
                             vfg_stage_sum_[0] / 150, vfg_stage_sum_[1] / 150, vfg_stage_sum_[2] / 150, vfg_mode_name(),
                             vfg_busy_ * 1000.0, interval_ * 1000.0);
                    vfg_stage_sum_[0] = vfg_stage_sum_[1] = vfg_stage_sum_[2] = 0;
                    vfg_stage_n_ = 0;
                }
            }
            return true;
        }
        // The back buffer still holds the real frame: interp_ draws over it.
        vfg_broken_ = true;
        vfg_.release();
        log_line(reshade::log::level::error,
                 "Method=vfg: NVIDIA Video Frame Generation failed (%s); Glasir's own frame generation from here on",
                 error.c_str());
    }
    interp_.draw(context_.Get(), views_[a].Get(), views_[b].Get(), rtv.Get(), o);
    if (g_settings.history_search && keep && o.method == fg::Method::motion) {
        interp_.keep_history(context_.Get());
        history_fresh_ = true;
        if (follows && !history_logged_) {
            history_logged_ = true;
            log_line(reshade::log::level::info, "HistorySearch=1: searching around the last pair's motion as well");
        }
    }
    return true;
}

void FrameGen::toggle(reshade::api::effect_runtime *runtime) {
    // GLASIR_FRAMEGEN_TOGGLE_AFTER=<seconds>: one toggle that many seconds after
    // the first frame, as if the key had been pressed (for tests).
    const double now = qpc_seconds();
    if (toggle_after_ < 0) {
        char text[16] = {};
        const DWORD len = GetEnvironmentVariableA("GLASIR_FRAMEGEN_TOGGLE_AFTER", text, sizeof(text));
        toggle_after_ = (len > 0 && len < sizeof(text)) ? now + atof(text) : 0;
    }
    const bool timed = toggle_after_ > 0 && now >= toggle_after_;
    if (timed) toggle_after_ = 0;
    if (!timed &&
        (g_settings.toggle_key <= 0 || !runtime->is_key_pressed(static_cast<uint32_t>(g_settings.toggle_key))))
        return;
    if (broken_) {
        glasir::overlay::toast("Frame generation: failed, see ReShade.log");
        return;
    }
    if (!source_ok()) {
        // Not saved: this launcher cannot run it either way.
        glasir::overlay::toast("Frame generation: needs a launcher with dedup=1");
        return;
    }
    set_enabled(!g_settings.enabled);
    glasir::overlay::toast(g_settings.enabled ? "Frame generation: on" : "Frame generation: off");
}

void FrameGen::set_enabled(bool on) {
    if (on == g_settings.enabled) return;
    g_settings.enabled = on;
    reshade::set_config_value(nullptr, kSection, "Enabled", g_settings.enabled);
    if (capture_.active()) capture_.reset();
    reset_history();
    paused_until_ = settle_until_ = 0;
    backoff_ = 5.0;
    drop_times_[0] = drop_times_[1] = -1e9;  // a drop from before does not pair with one after
    log_line(reshade::log::level::info, "toggled %s", g_settings.enabled ? "on" : "off");
}

// Quality=fast: the 2x2 cells around each pixel, compared on the pixel alone.
// Read at every draw, so it changes at once.
void FrameGen::set_fast(bool fast) {
    g_settings.fast = fast;
    interp_.warp_ring = fast ? 2 : 3;
    interp_.warp_taps = fast ? 1 : 5;
    interp_.soft_temp = fast ? 0.02f : 0.04f;
    reshade::set_config_value(nullptr, kSection, "Quality", fast ? "fast" : "high");
    retry_now();
    log_line(reshade::log::level::info, "quality %s", fast ? "fast" : "high");
}

// Read at every frame, so it changes at once.
void FrameGen::set_multiplier(int frames) {
    frames = std::clamp(frames, 2, 4);
    if (frames == g_settings.multiplier) return;
    g_settings.multiplier = frames;
    reshade::set_config_value(nullptr, kSection, "Multiplier", std::to_string(frames).c_str());
    retry_now();
    log_line(reshade::log::level::info, "Multiplier=%d (from the menu)", frames);
}

// From the menu: NVIDIA's filter makes the frames in between, or interp_
// again. The filter is made at the next frame in between (use_vfg) and let
// go at once when it is no longer wanted; one that did not start gets
// another try.
void FrameGen::set_vfg(bool on) {
    if (on == g_settings.vfg) return;
    g_settings.vfg = on;
    reshade::set_config_value(nullptr, kSection, "Method",
                              on ? "vfg" : g_settings.method == fg::Method::blend ? "blend" : "motion");
    if (!on) vfg_.release();
    vfg_failed_ = false;
    retry_now();
    log_line(reshade::log::level::info, "Method=%s (from the menu)", on ? "vfg" : "motion");
}

// From the menu: 0 low, 1 medium, 2 high - that model from the next frame in
// between on, the others let go - or 3, auto: the add-on picks from here on,
// starting with the one in use.
void FrameGen::set_vfg_quality(int quality) {
    const bool pick = quality >= 3;
    const fg::VfgMode mode =
        pick ? g_settings.vfg_mode : quality <= 0 ? fg::VfgMode::low : quality >= 2 ? fg::VfgMode::high : fg::VfgMode::medium;
    if (pick == g_settings.vfg_auto && mode == g_settings.vfg_mode) return;
    g_settings.vfg_auto = pick;
    g_settings.vfg_mode = mode;
    reshade::set_config_value(nullptr, kSection, "VfgQuality", vfg_quality_name());
    vfg_pick_.reset(mode);
    vfg_busy_ = -1;
    vfg_use_failed_ = false;
    std::string error;
    // One that cannot be had this way is made anew, with the reason said there if it fails again.
    if (vfg_.ready() && !vfg_.use(mode, pick, error)) vfg_.release();
    vfg_failed_ = false;
    retry_now();
    log_line(reshade::log::level::info, "VfgQuality=%s (from the menu)", vfg_quality_name());
}

// VfgQuality=auto: vfg_pick_ has chosen another model. Its filter is made the
// first time (a moment's stop) and kept, so the next change to it costs
// nothing. NVIDIA's licence keeps timings of its software out of what users
// send to others: the line says why in words.
// False: it could not be made, and the one in use goes on.
bool FrameGen::take_vfg_mode(const char *why) {
    const fg::VfgMode want = vfg_pick_.mode();
    std::string error;
    if (vfg_.ready() && !vfg_.use(want, true, error)) {
        vfg_pick_.failed(vfg_.mode());
        if (!vfg_use_failed_)
            log_line(reshade::log::level::warning, "VfgQuality=auto: another model could not be made (%s); it stays at %s",
                     error.c_str(), vfg_mode_name());
        vfg_use_failed_ = true;
        return false;
    }
    g_settings.vfg_mode = want;
    vfg_busy_ = -1;
    vfg_stage_n_ = 0;  // GLASIR_VFG_PROFILE: this model's frames from here
    vfg_stage_sum_[0] = vfg_stage_sum_[1] = vfg_stage_sum_[2] = 0;
    log_line(reshade::log::level::info, "VfgQuality=auto: %s now (%s)", vfg_mode_name(), why);
    return true;
}

// From the panel. A check (DebugMarker, LogStats) is kept in ReShade.ini as
// the add-on reads it at start; a switch only while it differs from its
// default (switch_setting).
void save_switch(const char *key, bool on) {
    reshade::set_config_value(nullptr, kSection, key, on ? "1" : "0");
    log_line(reshade::log::level::info, "%s=%d (from the panel)", key, on ? 1 : 0);
}

void save_switch(const char *key, bool on, bool standard) {
    if (on != standard) return save_switch(key, on);
    forget_setting(key);
    log_line(reshade::log::level::info, "%s=%d (from the panel; the default)", key, on ? 1 : 0);
}

// Read at every draw. What was kept for the search is from before the switch:
// the next pair starts without it.
void FrameGen::set_history_search(bool on) {
    g_settings.history_search = on;
    interp_.hist_search = on ? 1 : 0;
    interp_.forget_history();
    history_fresh_ = history_logged_ = false;
    save_switch("HistorySearch", on, kDefaults.history_search);
}

// follow_picture() makes the generator anew, for the picture or for the whole
// frame, a quarter of a second of presents later.
void FrameGen::set_picture_only(bool on) {
    g_settings.picture_only = on;
    save_switch("PictureOnly", on, kDefaults.picture_only);
}

void FrameGen::set_edge_rules(bool on) {
    g_settings.edge_rules = on;
    use_edge_rules();
    save_switch("EdgeRules", on, kDefaults.edge_rules);
}

void FrameGen::set_still_hud(bool on) {
    g_settings.still_hud = on;
    use_still_hud();
    save_switch("StillHud", on, kDefaults.still_hud);
}

// New frames dropped upstream: mpv is not keeping up with the source.
//
// Drops seen now happened since the previous frame. When that frame had a
// frame in between and mpv came back at its usual pace, the extra present is
// part of why: two such within two seconds pause generation - 5 s the first
// time, twice as long each time it happens again, at most a minute; 30 s after
// the last pause and the last such drop forgive it.
//
// Drops across a stall of mpv's own (far longer than the half-interval hold:
// DLSS 5 starting, shaders recompiling) or while not generating are not
// counted against frame generation; it only waits until mpv has gone a second
// without dropping.
void FrameGen::watch_drops(double now, bool stalled) {
    const LONG64 dropped = g_link.dropped();
    const LONG64 fresh = last_dropped_ < 0 ? 0 : dropped - last_dropped_;
    last_dropped_ = dropped;
    if (fresh <= 0) {
        if (generated_last_ && now - std::max(paused_until_, drop_times_[1]) > 30.0) backoff_ = 5.0;
        return;
    }
    if (stalled || !generated_last_) {
        settle_until_ = now + 1.0;
        return;
    }
    drops_ += static_cast<unsigned>(fresh);
    drop_times_[0] = drop_times_[1];
    drop_times_[1] = now;
    if (fresh < 2 && now - drop_times_[0] > 2.0) return;
    // VfgQuality=auto: a smaller model first, where there is one.
    if (g_settings.vfg_auto && vfg_running() && vfg_pick_.dropped(now, interval_, neural_now_) &&
        take_vfg_mode("mpv dropped new frames while generating")) {
        drop_times_[0] = drop_times_[1] = -1e9;
        settle_until_ = now + 0.5;
        return;
    }
    // At 3x and 4x, fewer frames first: 2x may well hold where they do not.
    if (pending_frames_ > 2) {
        fewer_frames(pending_frames_, now, "mpv dropped new frames while generating");
        drop_times_[0] = drop_times_[1] = -1e9;  // a drop from before does not pair with one after
        settle_until_ = now + 0.5;
        return;
    }
    paused_until_ = now + backoff_;
    log_line(reshade::log::level::warning,
             "mpv fell behind the source (new frames dropped while generating); frame generation paused for %.0f s",
             backoff_);
    backoff_ = std::min(backoff_ * 2.0, 60.0);
    reset_history();
}

void FrameGen::on_reshade_present(reshade::api::effect_runtime *runtime) {
    reshade::api::device *api_device = runtime->get_device();
    if (api_device->get_api() != reshade::api::device_api::d3d11) return;
    // Both are the original objects under ReShade's proxies, so nothing done
    // through them fires add-on events or runs ReShade's present path again.
    auto *swapchain = reinterpret_cast<IDXGISwapChain *>(runtime->get_native());
    auto *device = reinterpret_cast<ID3D11Device *>(api_device->get_native());
    if (!swapchain || !device) return;
    if (!device_ && !init(device)) return;
    if (device != device_.Get()) return;  // not the device we set up on: not mpv's frame
    mpv_swapchain_ = runtime->get_native();

    // Whatever the effects did, it was for this frame only.
    const bool neural = effects_ran_;
    neural_now_ = neural;
    effects_ran_ = false;
    const bool history = history_fresh_;
    history_fresh_ = false;  // until this present draws a pair
    // A real frame left over from a present whose finish_present never came
    // (it failed, or was a "still drawing" try) is dropped.
    if (pending_) trace(pending_now_, pending_dt_, pending_queued_, "generated-real-lost");
    pending_ = false;
    capture_.tick(context_.Get());  // a finished capture is read back whatever happens below

    const double now = qpc_seconds();
    if (!g_link.linked()) {
        g_link.poll(now);
        // mpv has only just started: let it settle before the first frame in
        // between, so its start-up drops are not blamed on generating.
        if (g_link.linked()) settle_until_ = now + 2.0;
    }
    toggle(runtime);
    // While frame generation is off, mpv gets every frame as without --dedup:
    // nothing gains from new frames only then (DedupWhenOff=1 keeps them).
    // So too while the in-game menu is open on a still picture (a quarter of
    // a second without a new frame): only keepalive frames would come, and the
    // menu is drawn only when a frame is presented. On a moving picture it is
    // drawn on every frame shown, generated ones included.
    const bool still_menu =
        glasir::overlay::menu_open() && g_link.linked() && g_link.since_last(now) > 0.25;
    if (g_link.linked())
        g_link.send_repeats(still_menu || (!g_settings.dedup_when_off && (broken_ || !g_settings.enabled)));
    // Also with the in-game menu open (it is drawn after the frames saved).
    const bool capture_key =
        g_settings.capture_key > 0 && runtime->is_key_pressed(static_cast<uint32_t>(g_settings.capture_key));
    if (broken_ || !g_settings.enabled || !source_ok()) {
        last_real_ = now;
        generated_last_ = false;
        if (capture_.active()) capture_.reset();  // its frames would not be in a row
        // No frames to save: the bug report comes without them.
        if (capture_key && !capture_.busy()) mpv_bug_report(L"", g_settings.enabled ? "unavailable" : "off");
        return;
    }

    // The time since mpv's previous frame. Every present counts as a real
    // frame, including mpv drawing the same video frame again for its OSD: the
    // frame in between is then made from that redraw and the next frame, which
    // is the picture on screen anyway. Telling redraws apart by timing alone
    // fails on a jittery source, and a real frame wrongly left out would make
    // the next frame in between step back in time.
    const double dt = last_real_ > 0 ? now - last_real_ : 0;
    last_real_ = now;
    const double reset = g_settings.reset_ms / 1000.0;
    // A gap - longer than ResetMs, or than 2.5 frames once the pace is known
    // - starts the timing over, so pauses never stretch the interval.
    const bool gap = dt <= 0 || dt > reset || (intervals_seen_ >= 3 && dt > 2.5 * dt_interval_);
    if (gap)
        reset_history();
    else
        push_interval(dt);
    source_interval_ = g_link.source_interval(reset);
    interval_ = g_link.linked() ? source_interval_ : (intervals_seen_ >= 3 ? dt_interval_ : 0);
    if (g_link.linked()) watch_drops(now, gap || (interval_ > 0 && dt > 2.0 * interval_));
    source_age_ = g_link.linked() ? g_link.since_last(now) : -1;
    effects_ = mpv_presented_ > 0 && now >= mpv_presented_ ? now - mpv_presented_ : -1;
    // This present carries a new frame if the source delivered one since the
    // last: then it tells how long new frames take from arrival to here.
    const LONG64 published = g_link.published();
    if (published != last_published_ && source_age_ >= 0 && source_age_ < reset && effects_ >= 0 &&
        source_age_ >= effects_)
        arrival_to_present_ = source_age_ - effects_;
    last_published_ = published;

    ComPtr<ID3D11Texture2D> back_buffer;
    D3D11_TEXTURE2D_DESC bb{};
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        if (capture_.active()) capture_.reset();
        return;
    }
    back_buffer->GetDesc(&bb);
    if (presented_color_space_ != color_space_) {
        color_space_ = presented_color_space_;
        log_line(reshade::log::level::info, "colour space %s", color_space_name(color_space_));
        if (capture_.active()) capture_.reset();  // its frames would not all be one encoding
    }
    if (bb.SampleDesc.Count != 1 || !ensure_frames(bb)) {
        if (capture_.active()) capture_.reset();
        return;
    }
    follow_picture(bb);
    if (!frames_[0]) return;

    if (now >= next_hz_try_) {
        // Every second: the window may move to another screen or the mode
        // change. A failed read keeps the last rate.
        if (const int hz = display_hz(swapchain)) hz_ = hz;
        next_hz_try_ = now + 1.0;
    }
    const int max_hz = g_settings.max_output_hz > 0 ? g_settings.max_output_hz : (hz_ > 0 ? hz_ : 60);
    max_hz_ = max_hz;

    const int cur = prev_ == 0 ? 1 : 0;
    context_->CopyResource(frames_[cur].Get(), back_buffer.Get());
    neural_[cur] = neural;
    ++real_;
    if (prev_ >= 0 && interval_ > 0 && dt > 1.5 * interval_) ++late_frames_;  // still generated

    int queued = -1;
    bool generate = prev_ >= 0 && interval_ > 0;
    // A lower rate taken because the GPU could not hold the one chosen is tried again after a while,
    // and at once when the effects run where they did not, or the other way (DLSS 5 switched on or off).
    if (limit_ < 4 && (now >= limit_until_ || neural != limit_neural_)) {
        log_line(reshade::log::level::info, "trying %dx again", g_settings.multiplier);
        all_frames_again();
    }
    // Multiplier frames for each of the source's, or as many as the display shows and the GPU holds.
    const int frames = generate ? fitting(interval_, max_hz, std::min(g_settings.multiplier, limit_)) : 1;
    if (generate && frames < 2) {
        generate = false;  // the display cannot show twice this rate
        ++skipped_fast_;
    }
    bool source_paused = false;
    if (generate && g_link.linked() && g_link.since_last(now) > std::max(reset, 3.0 * interval_)) {
        generate = false;  // the source is paused: this is a keepalive repeat
        source_paused = true;
        ++skipped_paused_;
    }
    if (generate && now < paused_until_) {
        generate = false;
        ++skipped_backoff_;
    }
    if (generate && now < settle_until_) {
        generate = false;  // mpv dropped frames on its own a moment ago
        ++skipped_settle_;
    }
    if (generate && neural_[prev_] != neural) {
        generate = false;  // the effects ran for one frame and not the other
        ++skipped_mixed_;
    }
    if (generate && (queued = queued_presents(swapchain)) >= 2) {
        generate = false;  // the screen is behind: another present would only wait
        ++skipped_queue_;
        // At 3x and 4x that is the frames of the real frame before, still waiting: the GPU was done with
        // its first one too late for the others to fit (DLSS 5 takes most of the interval). Every other
        // real frame then gets none - three frames and a long one, which looks worse than 2x. After a few
        // of these the rate goes down by one, to what holds steady.
        // VfgQuality=auto takes a smaller model first (below), while there is one.
        const bool smaller_first = g_settings.vfg_auto && vfg_running() && vfg_pick_.has_smaller();
        if (frames > 2 && !smaller_first && (strain_ += 3) >= 18)
            fewer_frames(frames, now, "frames were still queued when the next real frame came");
    } else if (generate && strain_ > 0) {
        --strain_;
    }
    // VfgQuality=auto: how the pair before went, and whether this one is left without its frame because
    // the screen is behind.
    if (g_settings.vfg_auto && vfg_running() && prev_ >= 0 && interval_ > 0 && !source_paused) {
        const bool behind = !generate && queued >= 2;
        // What a pair takes is for one number of frames in between: another (2x, 3x, 4x, or fewer for a
        // while) starts over.
        if (generate && frames != vfg_pick_frames_) {
            if (vfg_pick_frames_ > 0) {
                vfg_pick_.reset(g_settings.vfg_mode);
                vfg_busy_ = -1;
            }
            vfg_pick_frames_ = frames;
        }
        if (vfg_pick_.frame(now, interval_, vfg_busy_, behind, neural))
            take_vfg_mode(vfg_pick_.why() == fg::VfgAuto::Why::room ? "there is time to spare"
                                                                     : "the frames in between were not keeping up");
        vfg_busy_ = -1;
    }
    // How many frames in between a real frame gets lately, for the menu (3x and 4x that fall short).
    if (prev_ >= 0 && interval_ > 0 && !source_paused) {
        const double made = generate ? frames - 1 : 0;
        made_ = made_ < 0 ? made : made_ * 0.93 + made * 0.07;
    }

    // The last pair's motion says where things go on to only at an even pace:
    // a late frame, or mpv drawing the same frame again for its OSD, is a pair
    // of another length.
    const bool steady = interval_ > 0 && dt > 0.5 * interval_ && dt < 1.5 * interval_;
    if (generate && draw_between(back_buffer.Get(), prev_, cur, history && steady, steady, 1, frames)) {
        pending_ = true;
        pending_slot_ = cur;
        pending_frames_ = frames;
        // The real frame goes out half an interval after the frame in between
        // (never more than 40 ms: below 12.5 fps there is little left to
        // smooth): held, half an interval after it was done; else half an
        // interval after now. Held, it goes at the latest half a millisecond
        // before the next frame's effects could start - even if that is
        // sooner than half an interval: holding longer would hold up the next
        // frame's DLSS 5 pass, queued behind it. The next new frame may arrive
        // as soon as the shortest recent gap after the latest one, and takes
        // about as long from there to mpv's present as new frames have lately
        // (this present may be an OSD redraw that carries none). When that
        // cannot be told, no later than the old time.
        constexpr double kNextEffectsMargin = 0.0005;
        // At 3x and 4x the step is a third or a quarter of the interval, and the frames after the
        // first one in between follow a step apart (finish_present).
        pending_half_ = std::min(interval_, 0.08) / frames;
        pending_target_ = now + pending_half_;
        // The real frame is the last of them: frames - 1 steps from now.
        pending_latest_ = now + (frames - 1) * pending_half_;
        const double soonest = g_link.shortest_recent_gap(reset);
        if (soonest > 0 && source_age_ >= 0 && arrival_to_present_ >= 0)
            pending_latest_ = now - source_age_ + soonest + arrival_to_present_ - kNextEffectsMargin;
        pending_now_ = now;
        pending_dt_ = dt;
        pending_swapchain_ = swapchain;
        pending_since_ = qpc_seconds();
        ++generated_;  // the others as they are drawn (finish_present)
    }
    generated_last_ = pending_;
    // GLASIR_FRAMEGEN_CAPTURE_AFTER=<seconds>: a capture that many seconds after
    // frame generation starts, as if the key had been pressed (for tests).
    if (capture_after_ < 0) {
        char text[16] = {};
        const DWORD len = GetEnvironmentVariableA("GLASIR_FRAMEGEN_CAPTURE_AFTER", text, sizeof(text));
        capture_after_ = (len > 0 && len < sizeof(text)) ? now + atof(text) : 0;
    }
    const bool timed = capture_after_ > 0 && now >= capture_after_;
    if (timed) capture_after_ = 0;
    if (!capture_.busy() && (timed || capture_key)) {
        if (!capture_.start(device_.Get(), bb, pq(), !timed)) {
            glasir::overlay::toast("Frame generation: cannot capture this picture format");
            if (!timed) mpv_bug_report(L"", "failed");
        }
    }
    if (pending_) capture_.between(context_.Get(), back_buffer.Get());  // it holds the frame in between now
    capture_.real(context_.Get(), frames_[cur].Get());
    if (pending_)
        pending_queued_ = queued;  // traced once the real frame went out
    else
        trace(now, dt, queued, prev_ < 0 ? "first" : "real-only");
    prev_ = cur;
    if (!pending_) stats(now);  // else after the real frame went out
}

void FrameGen::on_finish_present(reshade::api::swapchain *api_swapchain) {
    if (!pending_) return;
    auto *swapchain = reinterpret_cast<IDXGISwapChain *>(api_swapchain->get_native());
    if (swapchain != pending_swapchain_) return;  // another swap chain's present: B still waits for mpv's
    pending_ = false;
    if (broken_) {
        trace(pending_now_, pending_dt_, pending_queued_, "generated-real-lost");
        return;
    }

    const double returned = qpc_seconds();
    mpv_present_max_ = std::max(mpv_present_max_, returned - pending_since_);
    mpv_present_sum_ += returned - pending_since_;

    // After the present, buffer 0 is the next buffer in the chain (D3D11 keeps
    // that name for whichever is current) and holds nothing useful.
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        trace(pending_now_, pending_dt_, pending_queued_, "generated-real-lost");
        return;
    }

    if (hold_wanted_) {
        // Here, not in init: creating the D3D12 device runs other add-ons'
        // device events, which should not happen under ReShade's locks.
        hold_wanted_ = false;
        std::string error;
        if (hold_.start(device_.Get(), context_.Get(), error))
            log_line(reshade::log::level::info, "the real frame is held on the GPU until its time");
        else
            log_line(reshade::log::level::warning,
                     "the real frame cannot be held on the GPU (%s); it is timed from mpv's present", error.c_str());
    }
    // The frames still to show for this real frame, each a step after the
    // one before it: the other frames in between (3x and 4x), then the real
    // frame itself. Sync interval 1, like mpv: a 0 could drop a frame still
    // queued, and more than 1 would hold a frame longer than its step.
    const int a = 1 - pending_slot_, b = pending_slot_;
    for (int index = 2; index <= pending_frames_; ++index) {
        // After each present, buffer 0 is the next buffer in the chain.
        if (index > 2 && FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(back_buffer.ReleaseAndGetAddressOf())))) {
            trace(pending_now_, pending_dt_, pending_queued_, "generated-real-lost");
            break;
        }
        const bool held = hold_.active();
        HoldJob job;
        if (held) job.n = hold_.mark_drawn();
        // The real frame last - and at once, in place of a frame in between that could not be drawn.
        const bool real =
            index == pending_frames_ || !draw_between(back_buffer.Get(), a, b, false, false, index, pending_frames_);
        if (real) context_->CopyResource(back_buffer.Get(), frames_[b].Get());
        glasir::overlay::draw_again(context_.Get(), back_buffer.Get());
        const char *what = real ? "generated" : "generated-between";
        if (!real) ++generated_;
        HRESULT hr = S_OK;
        if (held) {
            hold_.wait_release(job.n);
            hr = swapchain->Present(1, 0);
            // Let go even if the present failed: the wait is queued either way.
            job.half = pending_half_;
            job.latest = pending_latest_;
            job.left = pending_frames_ - index + 1;
            job.line = trace_line(pending_now_, pending_dt_, pending_queued_, what);
            hold_.let_go(job);
        } else {
            const double target = pending_target_ + (index - 2) * pending_half_;
            sleeper_.until(target);
            const double out = qpc_seconds();
            late_max_ = std::max(late_max_, out - target);
            late_sum_ += out - target;
            hr = swapchain->Present(1, 0);
            trace(pending_now_, pending_dt_, pending_queued_, what, out - pending_now_);
        }
        if (FAILED(hr)) {
            fail("Present after the frame in between", hr);
            break;
        }
        if (real) break;
    }
    out_max_ = std::max(out_max_, qpc_seconds() - returned);
    stats(qpc_seconds());  // no ReShade lock held here, the frames are queued
}

void FrameGen::stats(double now) {
    if (stats_start_ == 0) {
        stats_start_ = now;
        return;
    }
    const double span = now - stats_start_;
    if (span < 5.0) return;
    const Hold::Stats held = hold_.take_stats();
    real_fps_ = real_ / span;
    generated_fps_ = generated_ / span;
    if (g_settings.log_stats) {
        const int max_hz = g_settings.max_output_hz > 0 ? g_settings.max_output_hz : hz_;
        const double n = generated_ ? double(generated_) : 1.0;
        char out[512];
        if (hold_.active() && held.frames == 0) {
            snprintf(out, sizeof(out), "real frame held on the GPU: none in this window");
        } else if (hold_.active()) {
            const double d = held.drawn ? double(held.drawn) : 1.0;
            // How long the frame in between took is left out while NVIDIA's filter makes it: its
            // licence keeps performance data of its software out of what users send to others, and
            // this log goes into bug reports.
            char done[64] = "";
            if (vfg_generated_ == 0)
                snprintf(done, sizeof(done), " (done %.2f ms avg after the add-on drew it)", held.drawn_sum / d * 1000.0);
            snprintf(out, sizeof(out),
                     "real frame held on the GPU %.2f ms avg after the frame in between was done%s, let go %.3f ms "
                     "late avg, %.3f max; of %u: %u timed (%u of "
                     "them let go early for the next frame, %u already due when done), %u without the done "
                     "signal, %u with a later frame (queue full, resize); queued in %.2f ms max",
                     held.held_sum / d * 1000.0, done,
                     held.late_sum / (held.timed ? double(held.timed) : 1.0) * 1000.0, held.late_max * 1000.0,
                     held.frames, held.drawn, held.early, held.overdue, held.not_drawn, held.at_once,
                     out_max_ * 1000.0);
        } else {
            snprintf(out, sizeof(out), "real frame out late by %.3f ms avg, %.3f max", late_sum_ / n * 1000.0,
                     late_max_ * 1000.0);
        }
        log_line(reshade::log::level::info,
                 "%.1f real + %.1f generated fps | interval %.2f ms (%s) | display %d Hz, cap %d Hz | %u late "
                 "frames, %u dropped upstream after a generated frame | not generated: %u too fast, %u queue full, "
                 "%u source paused, %u backing off, %u mpv settling, %u effects mix | mpv present %.2f ms avg, "
                 "%.2f max | %s",
                 real_ / span, generated_ / span, interval_ * 1000.0, g_link.linked() ? "source" : "mpv", hz_,
                 max_hz, late_frames_, drops_, skipped_fast_, skipped_queue_, skipped_paused_, skipped_backoff_,
                 skipped_settle_, skipped_mixed_, mpv_present_sum_ / n * 1000.0, mpv_present_max_ * 1000.0, out);
    }
    if (g_trace) fflush(g_trace);
    stats_start_ = now;
    real_ = generated_ = skipped_fast_ = skipped_mixed_ = skipped_queue_ = skipped_paused_ = skipped_backoff_ = 0;
    skipped_settle_ = 0;
    late_frames_ = drops_ = 0;
    late_max_ = late_sum_ = mpv_present_max_ = mpv_present_sum_ = 0;
    out_max_ = 0;
    vfg_generated_ = 0;
}

// ---- The foreground across a new window ---------------------------------------
//
// When mpv makes its output anew while playing (glasir_scaling.lua, leaving
// NVIDIA's super resolution), its window is a new one too. Windows hands the
// foreground to some other program the moment the old one goes, and a window
// that is not in front gets no keys and no G-SYNC - the very thing the new
// output was made for (seen 11-19: the browser was in front afterwards). mpv
// brings its new window to the front itself, but a process that is not in
// front is refused. So while mpv's window is still in front and its swap
// chain goes, mpv's process is given leave to do so later. (ReShade loads its
// add-ons anew with the new device: nothing kept here would outlive it.)
void keep_foreground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid == GetCurrentProcessId()) AllowSetForegroundWindow(GetCurrentProcessId());
}

// ---- ReShade events -----------------------------------------------------------

void on_reshade_present(reshade::api::effect_runtime *runtime) {
    // A screenshot first: the back buffer still holds mpv's real frame.
    glasir::overlay::before_frame_generation(runtime);
    g_gen.on_reshade_present(runtime);
    // The menu last: over the frame in between (or the frame) mpv presents now.
    glasir::overlay::on_reshade_present(runtime);
    if (g_gen.is_mpv(runtime->get_native())) g_latency.before_present();
}

void on_finish_present(reshade::api::command_queue *, reshade::api::swapchain *swapchain) {
    const bool mpv = g_gen.is_mpv(swapchain->get_native());
    if (mpv) g_latency.presented();
    g_gen.on_finish_present(swapchain);  // the frames after the one mpv presented
    if (mpv) {
        if (g_presents_since_nvidia < ~0u) ++g_presents_since_nvidia;
        g_latency.frame_end(!nvidia_ran_lately());
    }
}

void on_finish_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *, reshade::api::resource_view,
                       reshade::api::resource_view) {
    g_gen.on_effects_finished(runtime);
}

void on_present(reshade::api::command_queue *, reshade::api::swapchain *swapchain, const reshade::api::rect *,
                const reshade::api::rect *, uint32_t, const reshade::api::rect *) {
    g_gen.on_mpv_present(swapchain);
    glasir::overlay::note_color_space(swapchain->get_color_space());
}

void register_present_events() {
    reshade::register_event<reshade::addon_event::present>(on_present);
    reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::register_event<reshade::addon_event::finish_present>(on_finish_present);
}

void unregister_present_events() {
    reshade::unregister_event<reshade::addon_event::finish_present>(on_finish_present);
    reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
    reshade::unregister_event<reshade::addon_event::present>(on_present);
}

// ---- The panel on ReShade's Add-ons page --------------------------------------
//
// Under this add-on's name on the Add-ons page of ReShade's own overlay (Home):
// what the generator is doing, and its switches, so one can be flipped while
// looking at the scene it is about. ReShade draws its overlay in mpv's
// present, on the same thread as the events above, into the frame the
// generator then takes. Frame generation itself and its quality are not here:
// those are Glasir's settings (glasir.ini, its menu), written into ReShade.ini
// at every start.
void draw_panel(reshade::api::effect_runtime *) {
    namespace ui = fg::reshade_ui;
    char line[200];
    double source = 0, real = 0, generated = 0;
    g_gen.rates(source, real, generated);
    if (g_gen.broken())
        ui::text("Failed: see ReShade.log.");
    else if (!g_gen.ready())
        ui::text("Not running: it needs Glasir's own capture with dedup.");
    else if (!g_settings.enabled)
        ui::text("Off (F8, or Glasir's menu: Tab).");
    else if (real <= 0)
        ui::text("On.");
    else {
        snprintf(line, sizeof(line), "On: %.1f real + %.1f generated frames a second.", real, generated);
        ui::text(line);
    }
    if (g_settings.vfg && g_gen.ready() && g_settings.enabled) {
        const int state = g_gen.vfg_state();
        if (state > 0)
            snprintf(line, sizeof(line), "Frames in between by NVIDIA Video Frame Generation (quality %s%s%s).",
                     vfg_quality_name(), g_settings.vfg_auto ? ": " : "", g_settings.vfg_auto ? vfg_mode_name() : "");
        else if (state < 0)
            snprintf(line, sizeof(line), "NVIDIA Video Frame Generation could not run (see ReShade.log): Glasir's own makes the frames.");
        else
            snprintf(line, sizeof(line), "NVIDIA Video Frame Generation (quality %s) starts with the first frame in between.",
                     vfg_quality_name());
        ui::text(line);
        if (state >= 0) ui::note("Edge rules, Still HUD and History search below are of Glasir's own generator.");
    }
    UINT width = 0, height = 0;
    fg::Picture picture;
    g_gen.working_on(width, height, picture);
    if (width != 0 && picture.x1 != 0) {
        snprintf(line, sizeof(line), "Works on the picture alone: %ux%u at %u,%u of the %ux%u frame.",
                 picture.x1 - picture.x0, picture.y1 - picture.y0, picture.x0, picture.y0, width, height);
        ui::text(line);
    } else if (width != 0) {
        snprintf(line, sizeof(line), "Works on the whole %ux%u frame%s.", width, height,
                 g_settings.picture_only ? " (no black bars in it)" : "");
        ui::text(line);
    }

    ui::heading("Switches");
    bool on = g_settings.picture_only;
    if (ui::checkbox("Picture only", &on)) g_gen.set_picture_only(on);
    ui::tooltip("With black bars around the picture (a 16:9 game on a 21:9 screen), frames are\n"
                "generated from the picture alone. Better scenery at the picture's edges and less\n"
                "GPU time; a HUD near those edges loses the bars' protection (Still HUD is for that).");
    on = g_settings.edge_rules;
    if (ui::checkbox("Edge rules", &on)) g_gen.set_edge_rules(on);
    ui::tooltip("In a camera turn, the strip along the edges that only one frame shows is drawn\n"
                "along the turn's motion. Meant to go with Picture only where there are bars.");
    on = g_settings.still_hud;
    if (ui::checkbox("Still HUD", &on)) g_gen.set_still_hud(on);
    ui::tooltip("Icons and digits drawn over fast-moving scenery are held where they are instead of\n"
                "fading or breaking up; the scenery right beside them (16 px) stands still with them.\n"
                "Gives a HUD near the picture's edges back what Picture only takes from it; also acts\n"
                "on a frame without bars.");
    on = g_settings.history_search;
    if (ui::checkbox("History search", &on)) g_gen.set_history_search(on);
    ui::tooltip("A test: the motion search also starts from what each block did over the pair before.");

    ui::heading("Checks");
    on = g_settings.marker;
    if (ui::checkbox("Marker on generated frames", &on)) {
        g_settings.marker = on;
        save_switch("DebugMarker", on);
    }
    ui::tooltip("A magenta square in the corner of every generated frame.");
    on = g_settings.log_stats;
    if (ui::checkbox("Log the rates", &on)) {
        g_settings.log_stats = on;
        save_switch("LogStats", on);
    }
    ui::tooltip("A line in ReShade.log every 5 s: frame rates, timings, why frames were not generated.");

    ui::space();
    ui::note("Each works at once and is remembered (ReShade.ini, [GLASIR_FRAMEGEN]).");
    ui::note("As they come: the first three switches on, History search off.");
    ui::note("What each gains and loses: docs/FRAMEGEN-LEARNINGS.md in Glasir's source.");
}

void set_menu_host();

void on_init_effect_runtime(reshade::api::effect_runtime *) {
    static bool loaded = false;
    if (!loaded) {
        load_settings();
        set_menu_host();
        loaded = true;
    }
    // ReShade calls add-ons in the order they registered, and loads them in
    // whatever order the folder lists them. Registering again now, after all
    // of them loaded, puts us last regardless of the file name.
    unregister_present_events();
    register_present_events();
}

// Everything is let go with mpv's swap chain or device - not with the effect
// runtime, which ReShade rebuilds on every resize, and not with devices other
// add-ons make for themselves.
void on_destroy_swapchain(reshade::api::swapchain *swapchain, bool resize) {
    if (!g_gen.owns(swapchain->get_device())) return;
    if (resize) {
        g_gen.release_held();  // ResizeBuffers would wait for the held present
    } else {
        keep_foreground();  // mpv is closing, or making its output anew
        // The menu is made on this device too. (destroy_device comes after this, when owns()
        // no longer says so: without this the menu stayed on a device that was gone, and
        // dead on the one after it.)
        glasir::overlay::release();
        g_upscale.release();
        g_gen.release();
    }
}

// Direct3D 12 devices are other add-ons' (DLSS 5's feeder makes one): KeptDevices.
void on_init_device(reshade::api::device *device) {
    if (device->get_api() == reshade::api::device_api::d3d12)
        g_kept_devices.made(reinterpret_cast<IUnknown *>(device->get_native()));
}

void on_destroy_device(reshade::api::device *device) {
    if (device->get_api() == reshade::api::device_api::d3d12)
        g_kept_devices.going(reinterpret_cast<IUnknown *>(device->get_native()));
    if (!g_gen.owns(device)) return;
    glasir::overlay::release();
    g_upscale.release();
    g_gen.release();
}

void set_menu_host() {
    glasir::overlay::Host host;
    host.framegen_ready = [] { return g_gen.ready(); };
    host.framegen_enabled = [] { return g_settings.enabled; };
    host.set_framegen_enabled = [](bool on) { g_gen.set_enabled(on); };
    host.framegen_fast = [] { return g_settings.fast; };
    host.set_framegen_fast = [](bool fast) { g_gen.set_fast(fast); };
    host.multiplier = [] { return g_settings.multiplier; };
    host.set_multiplier = [](int frames) { g_gen.set_multiplier(frames); };
    // Whether NVIDIA's filter is installed is looked at once: setup runs while mpv does not.
    host.nvidia_offered = [] {
        static const bool installed = fg::Vfg::runtime_present(vfg_runtime_folder());
        return installed;
    };
    host.framegen_nvidia = [] { return g_settings.vfg; };
    host.set_framegen_nvidia = [](bool on) { g_gen.set_vfg(on); };
    host.nvidia_quality = [] { return g_settings.vfg_auto ? 3 : int(g_settings.vfg_mode); };
    host.nvidia_quality_now = [] { return int(g_settings.vfg_mode); };
    host.set_nvidia_quality = [](int quality) {
        g_gen.set_vfg_quality(quality);
    };
    host.nvidia_state = [] { return g_gen.vfg_state(); };
    host.rates = [](double &source, double &real, double &generated) { g_gen.rates(source, real, generated); };
    host.framegen_status = [] { return g_gen.status(); };
    host.low_latency = [] { return g_latency.mode(); };
    host.set_low_latency = [](int mode) { g_latency.set(mode); };
    host.gsync = [] { return g_latency.gsync(); };
    host.composed = [] { return g_latency.composed(); };
    host.mpv_pipe = g_settings.mpv_pipe;
    glasir::overlay::set_host(host);
    glasir::overlay::ScalerHost scaler;
    scaler.level = [] { return g_upscale.level(); };
    scaler.set_level = [](int level) { g_upscale.set_level(level); };
    scaler.state = [] { return g_upscale.state(); };
    glasir::overlay::set_scaler_host(scaler);
}

bool g_panel = false;

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module)) return FALSE;
        // A ReShade without the overlay (or with another Dear ImGui): no
        // panel, the switches stay in ReShade.ini.
        g_panel = fg::reshade_ui::init(reshade::internal::get_reshade_module_handle());
        if (g_panel) reshade::register_overlay(nullptr, draw_panel);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        register_present_events();
        break;
    case DLL_PROCESS_DETACH:
        if (g_panel) reshade::unregister_overlay(nullptr, draw_panel);
        unregister_present_events();
        reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::unregister_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
        reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::unregister_addon(module);
        g_link.close();
        if (g_trace) fclose(g_trace);
        break;
    }
    return TRUE;
}
