// SPDX-License-Identifier: MIT
// Starting mpv with the right options and telling it what to play.
#pragma once
#include <windows.h>

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "devices.h"
#include "settings.h"

namespace glasir {

struct MpvPlan {
    std::wstring mpv_exe;
    std::wstring config_dir;      // mpv's portable_config
    std::wstring screenshot_dir;
    std::wstring log_file;        // mpv's own log; empty = none
    std::wstring ipc_pipe;        // \\.\pipe\...

    // Raw frames on mpv's standard input (Media Foundation or --input-raw),
    // or else mpv opens the card itself through DirectShow.
    bool raw = true;
    FrameLayout layout;           // raw
    double fps = 0;               // raw: the capture rate
    std::wstring dshow_device;    // DirectShow
    std::optional<VideoMode> dshow_mode;  // DirectShow: the mode to ask for, if known

    // The audio plugin (glasir_audio.dll): the endpoint to capture, empty for
    // none; audio_missing: the card's audio input was not found (say so).
    std::wstring audio_device;
    std::wstring audio_log;
    bool audio_missing = false;
};

// mpv's arguments (without the program).
std::vector<std::wstring> mpv_arguments(const MpvPlan &plan, const Settings &settings);
// What to load once mpv is up: "-" (stdin) or the DirectShow URL.
std::wstring mpv_load_target(const MpvPlan &plan);

// One argument quoted the way CommandLineToArgvW reads it back.
std::wstring quote_argument(const std::wstring &arg);
std::wstring join_command_line(const std::wstring &program, const std::vector<std::wstring> &args);

// {"command":[...]} plus a newline, as mpv's IPC reads it.
std::string ipc_command(const std::vector<std::wstring> &words);

// mpv, in a job object so it ends with Glasir.
class MpvProcess {
public:
    MpvProcess() = default;
    MpvProcess(const MpvProcess &) = delete;
    MpvProcess &operator=(const MpvProcess &) = delete;
    ~MpvProcess();

    // feed_stdin: mpv's standard input is a pipe this process writes frames to.
    bool start(const std::wstring &command_line, bool feed_stdin, std::wstring &error);
    // Sends a command line over IPC, waiting up to timeout_ms for mpv to
    // open its pipe. False if mpv exits or the time runs out.
    bool send(const std::wstring &pipe, const std::string &command, DWORD timeout_ms);

    enum class LoadResult { Loaded, Failed, NoAnswer };
    // Loads target and waits up to timeout_ms for mpv to say it opened it,
    // or why it could not (detail: mpv's reason).
    LoadResult load(const std::wstring &pipe, const std::wstring &target, DWORD timeout_ms, std::wstring &detail);
    bool write_frame(const BYTE *data, size_t bytes);
    void close_stdin();
    bool exited(DWORD wait_ms = 0) const;
    DWORD exit_code() const;
    DWORD pid() const { return pi_.dwProcessId; }
    HANDLE process() const { return pi_.hProcess; }
    // Asks mpv to quit over IPC, then ends it after grace_ms.
    void stop(const std::wstring &pipe, DWORD grace_ms);

private:
    // A connection to mpv's IPC pipe, or nullptr.
    HANDLE connect(const std::wstring &pipe, DWORD timeout_ms) const;

    PROCESS_INFORMATION pi_{};
    HANDLE job_ = nullptr;
    HANDLE stdin_ = nullptr;
    friend class MpvWatch;
};

// Follows mpv properties over IPC while mpv runs: what the user changes while
// playing (volume, the audio plugin's boost and delay), to keep for next time.
class MpvWatch {
public:
    MpvWatch() = default;
    MpvWatch(const MpvWatch &) = delete;
    MpvWatch &operator=(const MpvWatch &) = delete;
    ~MpvWatch() { stop(); }

    bool start(const MpvProcess &mpv, const std::wstring &pipe, const std::vector<std::string> &properties);
    // Ends the watch (mpv closing ends it too).
    void stop();
    // The property's last value if it changed since the first one seen
    // (numbers as written by mpv, strings as they are).
    std::optional<std::string> changed(const std::string &property) const;
    // Called (on the watch's thread) with the words of each script-message
    // mpv passes on to its clients. Set before start().
    std::function<void(const std::vector<std::string> &)> on_message;

    // What start() does before it connects, and one IPC line: for the tests.
    void track(const std::vector<std::string> &properties);
    void on_line(const std::string &line);

private:
    struct Value {
        std::optional<std::string> first, last;
    };
    mutable std::mutex m_;
    std::vector<std::string> names_;
    std::vector<Value> values_;
    HANDLE pipe_ = nullptr;
    std::thread thread_;
};

}  // namespace glasir
