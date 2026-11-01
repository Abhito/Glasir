// SPDX-License-Identifier: MIT
// Starting mpv with the right options and telling it what to play.
#pragma once
#include <windows.h>

#include <optional>
#include <string>
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
};

}  // namespace glasir
