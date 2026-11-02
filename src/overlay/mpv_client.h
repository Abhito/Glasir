// SPDX-License-Identifier: MIT
// The menu's line to mpv (its IPC pipe), on a thread of its own: mpv's render
// thread, where the menu is drawn, never waits on the pipe. It follows the
// properties the menu shows and sends the commands the menu gives.
#pragma once
#include <windows.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace glasir::overlay {

struct MpvState {
    bool connected = false;
    double volume = -1;  // -1: not known yet
    bool mute = false;
    bool fullscreen = false;
    int boost = -1;   // the audio plugin's (user-data/glasir/audio-boost), -1: no plugin
    int delay = -1;   // ms, likewise
    int width = 0, height = 0;  // the video's size
    std::string scaling;        // glasir_scaling.lua's mode (user-data/glasir/scaling): mpv, fsr, vsr, cnn, anime4k
};

// A short message to show over the picture (a toast): from a key mpv
// handled (volume, mute) or one sent with "script-message glasir-toast
// <text> [ms]". serial grows with each; 0: none yet.
struct Toast {
    std::string text;
    double seconds = 1.5;
    bool from_property = false;  // a value changed (also by the menu itself)
    unsigned serial = 0;
};

class MpvClient {
public:
    MpvClient() = default;
    MpvClient(const MpvClient &) = delete;
    MpvClient &operator=(const MpvClient &) = delete;
    ~MpvClient() { stop(); }

    void start(const std::wstring &pipe);
    void stop();
    MpvState state() const;
    // Queues {"command":[words...]}; sent in order.
    void command(const std::vector<std::string> &words);
    Toast toast() const;

private:
    void run();
    void on_line(const std::string &line);

    std::wstring pipe_;
    std::thread thread_;
    HANDLE wake_ = nullptr;  // a command is queued, or stop
    bool stopping_ = false;  // under m_
    mutable std::mutex m_;
    MpvState state_;
    Toast toast_;
    bool seen_volume_ = false, seen_mute_ = false;  // since connecting: the first values are no change
    std::vector<std::string> queue_;  // JSON lines
};

// One JSON string, quoted and escaped.
std::string json_quote(const std::string &s);

}  // namespace glasir::overlay
