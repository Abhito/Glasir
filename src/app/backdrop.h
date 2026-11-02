// SPDX-License-Identifier: MIT
// mpv's window while it plays: full screen (borderless) or a window, switched
// while playing (the in-game menu), and full screen without the black bars.
//
// Without the bars ([window] skip_black_bars): on a screen of another shape
// than the picture (a 16:9 console on an ultrawide), a full-screen mpv window
// hands DLSS 5, frame generation and the menu the whole screen, bars
// included: on 3440x1440 a quarter of the work is black. Instead mpv's window
// is made the picture's size, centred, and a black window of Glasir's fills
// the rest of the screen behind it. mpv keeps its own top-level window (a
// child window inside Glasir's would lose VRR), but a window smaller than the
// monitor keeps independent flip only while Windows gives it an overlay
// plane (MPO); without one it is composed and frame generation falls short.
// So it is a choice, off by default.
#pragma once
#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace glasir {

// The picture's place on a monitor: as large as fits with the source's
// shape (square pixels), centred. The monitor itself when it has that shape.
RECT fit_picture(const RECT &monitor, UINT32 width, UINT32 height);

// Where a window of width x height (client area) goes: centred in the work
// area, made smaller (same shape) if it does not fit.
RECT centre_window(const RECT &work, LONG width, LONG height);

struct WindowSetup {
    bool borderless = true;   // glasir.ini window.mode
    bool fullscreen = false;  // window.mode = fullscreen: mpv starts in its own full screen
    bool skip_bars = false;   // glasir.ini window.skip_black_bars (full screen only)
    UINT32 width = 0, height = 0;  // the picture; 0: unknown (full screen covers the screen)
    // Runs an mpv command over IPC (from the backdrop's own thread).
    std::function<void(const std::vector<std::wstring> &)> mpv_command;
};

class Backdrop {
public:
    Backdrop() = default;
    Backdrop(const Backdrop &) = delete;
    Backdrop &operator=(const Backdrop &) = delete;
    ~Backdrop() { stop(); }

    // Waits for mpv's window (process mpv_pid); once it is up, keeps it as
    // the setup asks: without the bars, the picture's size on its monitor
    // with the black window behind it (hidden while mpv is minimised or
    // covers the whole screen, e.g. mpv's own full screen).
    void start(DWORD mpv_pid, WindowSetup setup);
    // Full screen or a window, at once (any thread).
    void set_borderless(bool borderless);
    // mpv left its own full screen: the borderless window goes back in place
    // (a window stays where mpv puts it back). Any thread.
    void place_again();
    void stop();

private:
    void run();

    std::thread thread_;
    std::atomic<DWORD> thread_id_{0};
    std::atomic<bool> stopping_{false};
    DWORD pid_ = 0;
    WindowSetup setup_;
};

}  // namespace glasir
