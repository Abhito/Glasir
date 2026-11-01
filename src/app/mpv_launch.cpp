// SPDX-License-Identifier: MIT
#include "mpv_launch.h"

#include <algorithm>
#include <format>

#include "json.h"
#include "log.h"

namespace glasir {
namespace {

std::wstring fps_text(double fps) {
    std::wstring s = std::format(L"{:.3f}", fps);
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    return s;
}

// The colour tags raw frames need: they carry none of their own. SDR from
// consoles is BT.709; NV12 from cards is sited top-left, 4:2:2 left.
std::wstring colour_filter(const MpvPlan &plan, const Settings &s) {
    if (s.hdr)
        return std::format(L"format=colormatrix=bt.2020-ncl:colorlevels=limited:primaries=bt.2020:gamma=pq:"
                           L"max-luma={0}:max_cll={0}",
                           s.hdr_peak_nits);
    const wchar_t *siting = plan.layout.format == PixelFormat::NV12 ? L"uhd" : L"mpeg2/4/h264";
    return std::format(L"format=colormatrix=bt.709:colorlevels={}:primaries=bt.709:chroma-location={}",
                       s.sdr_full_range ? L"full" : L"limited", siting);
}

}  // namespace

std::vector<std::wstring> mpv_arguments(const MpvPlan &plan, const Settings &s) {
    std::vector<std::wstring> a = {
        // Start idle with the window up, so ReShade is loaded before
        // playback; Glasir then loads the source over IPC.
        L"--idle=yes",
        L"--force-window=immediate",
        L"--no-terminal",
        L"--config-dir=" + plan.config_dir,
        L"--input-ipc-server=" + plan.ipc_pipe,
        L"--title=Glasir",
        L"--screenshot-dir=" + plan.screenshot_dir,
        // Low latency: no buffering, show each frame as it arrives.
        L"--profile=low-latency",
        L"--untimed",
        L"--demuxer-thread=no",
        L"--vd-lavc-threads=1",
        L"--sws-scaler=point",
        L"--vo=gpu-next",
        L"--gpu-api=d3d11",
        L"--hwdec=no",
        L"--glsl-shaders-clr",
        L"--deband=no",
        L"--osc=no",
        L"--cursor-autohide=100",
        // Audio comes later (its own plugin), not through mpv's demuxer.
        L"--aid=no",
        L"--audio-file-auto=no",
        L"--no-auto-window-resize",
    };
    if (s.borderless) {
        a.push_back(L"--no-border");
        a.push_back(L"--geometry=100%x100%");
    } else {
        a.push_back(L"--geometry=1280x720");
    }
    if (!plan.log_file.empty()) a.push_back(L"--log-file=" + plan.log_file);

    // Sound: glasir_audio.dll (portable_config\scripts) plays the card's
    // audio input; mpv's volume and mute drive it.
    a.push_back(std::format(L"--volume={}", s.volume));
    a.push_back(L"--volume-max=100");
    auto opt = [&](const std::wstring &key, const std::wstring &value) {
        a.push_back(L"--script-opts-append=glasir_audio-" + key + L"=" + value);
    };
    opt(L"device", plan.audio_device);
    opt(L"delay", std::to_wstring(s.audio_delay_ms));
    opt(L"boost", std::to_wstring(s.boost));
    opt(L"period", s.audio_min_period ? L"minimum" : L"default");
    if (!plan.audio_log.empty()) opt(L"log", plan.audio_log);
    if (plan.audio_missing) opt(L"missing", L"1");

    if (plan.raw) {
        a.push_back(L"--cache=no");
        a.push_back(L"--demuxer=rawvideo");
        a.push_back(std::format(L"--demuxer-rawvideo-w={}", plan.layout.width));
        a.push_back(std::format(L"--demuxer-rawvideo-h={}", plan.layout.height));
        a.push_back(std::format(L"--demuxer-rawvideo-mp-format={}", mpv_format_name(plan.layout.format)));
        a.push_back(L"--demuxer-rawvideo-fps=" + fps_text(plan.fps));
        a.push_back(L"--vf=" + colour_filter(plan, s));
    } else {
        a.push_back(L"--demuxer-lavf-o-append=rtbufsize=64M");
        // The mode from the card's list, or else what glasir.ini asks for.
        const UINT32 width = plan.dshow_mode ? plan.dshow_mode->width : s.width;
        const UINT32 height = plan.dshow_mode ? plan.dshow_mode->height : s.height;
        const double fps = plan.dshow_mode ? plan.dshow_mode->fps : s.fps;
        if (width && height) a.push_back(std::format(L"--demuxer-lavf-o-append=video_size={}x{}", width, height));
        if (fps > 0) {
            a.push_back(L"--demuxer-lavf-o-append=framerate=" + fps_text(fps));
            a.push_back(L"--container-fps-override=" + fps_text(fps));
        }
        if (plan.dshow_mode) {
            if (plan.dshow_mode->supported)
                a.push_back(std::format(L"--demuxer-lavf-o-append=pixel_format={}",
                                        mpv_format_name(plan.dshow_mode->format)));
            else if (plan.dshow_mode->subtype == L"MJPG")
                a.push_back(L"--demuxer-lavf-o-append=vcodec=mjpeg");
        }
    }
    if (s.hdr) {
        a.push_back(L"--target-colorspace-hint=yes");
        a.push_back(L"--d3d11-output-format=rgb10_a2");
        a.push_back(L"--d3d11-output-csp=pq");
        a.push_back(L"--hdr-compute-peak=no");
    } else {
        a.push_back(L"--target-colorspace-hint=no");
    }
    return a;
}

std::wstring mpv_load_target(const MpvPlan &plan) {
    return plan.raw ? L"-" : L"av://dshow:video=" + plan.dshow_device;
}

std::wstring quote_argument(const std::wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');  // they come before the closing quote
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring join_command_line(const std::wstring &program, const std::vector<std::wstring> &args) {
    std::wstring line = quote_argument(program);
    for (const auto &a : args) {
        line.push_back(L' ');
        line += quote_argument(a);
    }
    return line;
}

std::string ipc_command(const std::vector<std::wstring> &words) {
    std::string out = "{\"command\":[";
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) out.push_back(',');
        out.push_back('"');
        for (unsigned char c : narrow(words[i])) {
            if (c == '"' || c == '\\') {
                out.push_back('\\');
                out.push_back(char(c));
            } else if (c < 0x20) {
                out += std::format("\\u{:04x}", c);
            } else {
                out.push_back(char(c));
            }
        }
        out.push_back('"');
    }
    out += "]}\n";
    return out;
}

MpvProcess::~MpvProcess() {
    close_stdin();
    if (job_) CloseHandle(job_);  // kill-on-close: mpv ends with us
    if (pi_.hProcess) CloseHandle(pi_.hProcess);
    if (pi_.hThread) CloseHandle(pi_.hThread);
}

bool MpvProcess::start(const std::wstring &command_line, bool feed_stdin, std::wstring &error) {
    // mpv holds the card (DirectShow) or the frame pipe: it must not outlive
    // Glasir, so it runs in a job that ends it when Glasir's handle closes,
    // even if Glasir crashes.
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = std::format(L"Could not prepare mpv's job object (error {}).", GetLastError());
        return false;
    }

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_end = nullptr;
    if (feed_stdin) {
        if (!CreatePipe(&read_end, &stdin_, &inherit, 8u << 20)) {
            error = std::format(L"Could not create the frame pipe (error {}).", GetLastError());
            return false;
        }
        SetHandleInformation(stdin_, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = read_end ? read_end : nul;
    si.hStdOutput = nul;
    si.hStdError = nul;

    std::vector<wchar_t> cmd(command_line.begin(), command_line.end());
    cmd.push_back(0);
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr,
                                   &si, &pi_);
    const DWORD err = GetLastError();
    if (read_end) CloseHandle(read_end);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        error = std::format(L"Could not start mpv (error {}).", err);
        log(L"could not start mpv (error {}): {}", err, command_line);
        close_stdin();
        return false;
    }
    if (!AssignProcessToJobObject(job_, pi_.hProcess)) {
        const DWORD assign_err = GetLastError();
        TerminateProcess(pi_.hProcess, 1);  // still suspended: it never ran
        error = std::format(L"Could not put mpv in Glasir's job object (error {}).", assign_err);
        close_stdin();
        return false;
    }
    ResumeThread(pi_.hThread);
    log(L"started mpv (pid {}): {}", pi_.dwProcessId, command_line);
    return true;
}

HANDLE MpvProcess::connect(const std::wstring &pipe, DWORD timeout_ms) const {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        HANDLE h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        const DWORD err = GetLastError();
        if (exited() || GetTickCount64() >= deadline) {
            log(L"could not reach mpv's IPC pipe (error {})", err);
            return nullptr;
        }
        if (err == ERROR_PIPE_BUSY)
            WaitNamedPipeW(pipe.c_str(), 200);
        else
            Sleep(30);
    }
}

bool MpvProcess::send(const std::wstring &pipe, const std::string &command, DWORD timeout_ms) {
    HANDLE h = connect(pipe, timeout_ms);
    if (!h) return false;
    // No FlushFileBuffers: it waits until mpv has read everything, which can
    // be long while mpv's core is busy opening a source.
    DWORD written = 0;
    const BOOL ok = WriteFile(h, command.data(), DWORD(command.size()), &written, nullptr);
    if (!ok) log(L"IPC write failed (error {})", GetLastError());
    CloseHandle(h);
    return ok && written == command.size();
}

MpvProcess::LoadResult MpvProcess::load(const std::wstring &pipe, const std::wstring &target, DWORD timeout_ms,
                                        std::wstring &detail) {
    HANDLE h = connect(pipe, timeout_ms);
    if (!h) return LoadResult::NoAnswer;
    const std::string command = ipc_command({L"loadfile", target, L"replace"});
    DWORD written = 0;
    if (!WriteFile(h, command.data(), DWORD(command.size()), &written, nullptr)) {
        log(L"IPC write failed (error {})", GetLastError());
        CloseHandle(h);
        return LoadResult::NoAnswer;
    }
    // The time to open the source starts once mpv is listening. Every client
    // gets mpv's events: opening ends in "file-loaded", or "end-file" with
    // reason "error" and mpv's file_error.
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    LoadResult result = LoadResult::NoAnswer;
    std::string pending;
    while (result == LoadResult::NoAnswer && GetTickCount64() < deadline && !exited()) {
        DWORD available = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) {
            Sleep(20);
            continue;
        }
        std::string chunk(available, '\0');
        DWORD got = 0;
        if (!ReadFile(h, chunk.data(), available, &got, nullptr)) break;
        pending.append(chunk.data(), got);
        for (size_t nl; result == LoadResult::NoAnswer && (nl = pending.find('\n')) != std::string::npos;) {
            const std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (line.find("\"event\":\"file-loaded\"") != std::string::npos) {
                result = LoadResult::Loaded;
            } else if (line.find("\"event\":\"end-file\"") != std::string::npos &&
                       line.find("\"reason\":\"error\"") != std::string::npos) {
                result = LoadResult::Failed;
                const std::string key = "\"file_error\":\"";
                const size_t at = line.find(key);
                if (at != std::string::npos) {
                    const size_t end = line.find('"', at + key.size());
                    detail = widen(line.substr(at + key.size(), end - at - key.size()));
                }
            }
        }
    }
    CloseHandle(h);
    return result;
}

bool MpvProcess::write_frame(const BYTE *data, size_t bytes) {
    while (bytes) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes, 4u << 20));
        DWORD n = 0;
        if (!WriteFile(stdin_, data, chunk, &n, nullptr) || !n) return false;
        data += n;
        bytes -= n;
    }
    return true;
}

void MpvProcess::close_stdin() {
    if (stdin_) CloseHandle(stdin_);
    stdin_ = nullptr;
}

bool MpvProcess::exited(DWORD wait_ms) const {
    return !pi_.hProcess || WaitForSingleObject(pi_.hProcess, wait_ms) == WAIT_OBJECT_0;
}

DWORD MpvProcess::exit_code() const {
    DWORD code = 0;
    if (pi_.hProcess) GetExitCodeProcess(pi_.hProcess, &code);
    return code;
}

void MpvProcess::stop(const std::wstring &pipe, DWORD grace_ms) {
    if (exited(200)) return;  // already closing (its window was closed)
    if (send(pipe, ipc_command({L"quit"}), 500) && exited(grace_ms)) return;
    TerminateJobObject(job_, 1);
    exited(2000);
}

bool MpvWatch::start(const MpvProcess &mpv, const std::wstring &pipe, const std::vector<std::string> &properties) {
    stop();
    pipe_ = mpv.connect(pipe, 2000);
    if (!pipe_) return false;
    track(properties);
    std::string commands;
    for (size_t i = 0; i < properties.size(); ++i)
        commands += std::format("{{\"command\":[\"observe_property\",{},\"{}\"]}}\n", i + 1, properties[i]);
    DWORD written = 0;
    if (!WriteFile(pipe_, commands.data(), DWORD(commands.size()), &written, nullptr)) {
        log(L"IPC write failed (error {})", GetLastError());
        CloseHandle(pipe_);
        pipe_ = nullptr;
        return false;
    }
    thread_ = std::thread([this] {
        std::string pending;
        char buffer[4096];
        DWORD got = 0;
        // Ends when mpv closes the pipe, or stop() cancels the read.
        while (ReadFile(pipe_, buffer, sizeof(buffer), &got, nullptr) && got) {
            pending.append(buffer, got);
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                on_line(pending.substr(0, nl));
                pending.erase(0, nl + 1);
            }
        }
    });
    return true;
}

void MpvWatch::track(const std::vector<std::string> &properties) {
    std::lock_guard lock(m_);
    names_ = properties;
    values_.assign(properties.size(), {});
}

void MpvWatch::stop() {
    if (thread_.joinable()) {
        while (WaitForSingleObject(thread_.native_handle(), 20) == WAIT_TIMEOUT)
            CancelSynchronousIo(thread_.native_handle());
        thread_.join();
    }
    if (pipe_) CloseHandle(pipe_);
    pipe_ = nullptr;
}

void MpvWatch::on_line(const std::string &line) {
    if (on_message && line.find("\"client-message\"") != std::string::npos) {
        const auto j = Json::parse(line);
        if (!j || j->str("event") != "client-message") return;
        std::vector<std::string> words;
        if (const Json *args = j->get("args"))
            for (const Json &a : args->items()) words.push_back(a.string());
        if (!words.empty()) on_message(words);
        return;
    }
    if (line.find("\"property-change\"") == std::string::npos) return;
    const auto j = Json::parse(line);
    if (!j || j->str("event") != "property-change") return;
    const Json *data = j->get("data");
    if (!data) return;  // not set (yet)
    std::string value;
    if (data->is(Json::Type::String)) value = data->string();
    else if (data->is(Json::Type::Number)) value = std::format("{}", data->number());
    else if (data->is(Json::Type::Bool)) value = data->boolean() ? "yes" : "no";
    else return;
    std::lock_guard lock(m_);
    for (size_t i = 0; i < names_.size(); ++i)
        if (names_[i] == j->str("name")) {
            if (!values_[i].first) values_[i].first = value;
            values_[i].last = value;
        }
}

std::optional<std::string> MpvWatch::changed(const std::string &property) const {
    std::lock_guard lock(m_);
    for (size_t i = 0; i < names_.size(); ++i)
        if (names_[i] == property && values_[i].last != values_[i].first) return values_[i].last;
    return std::nullopt;
}

}  // namespace glasir
