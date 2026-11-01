// SPDX-License-Identifier: MIT
// Full screen without paying for the black bars.
//
// On a screen of another shape than the picture (a 16:9 console on an
// ultrawide), a full-screen mpv window would hand DLSS 5, frame generation
// and the menu the whole screen, bars included: on 3440x1440 a quarter of
// the work is black. Instead mpv's window is made the picture's size,
// centred, and a black window of Glasir's fills the rest of the screen behind
// it. mpv keeps its own top-level window: measured with PresentMon, it is
// still presented by independent flip and keeps VRR (a child window inside
// Glasir's would lose VRR).
#pragma once
#include <windows.h>

#include <atomic>
#include <thread>

namespace glasir {

// The picture's place on a monitor: as large as fits with the source's
// shape (square pixels), centred. The monitor itself when it has that shape.
RECT fit_picture(const RECT &monitor, UINT32 width, UINT32 height);

class Backdrop {
public:
    Backdrop() = default;
    Backdrop(const Backdrop &) = delete;
    Backdrop &operator=(const Backdrop &) = delete;
    ~Backdrop() { stop(); }

    // Waits for mpv's window (process mpv_pid); once it is up, sizes it to
    // a width x height picture on its monitor and keeps the black window
    // behind it - hidden while mpv is minimised or covers the whole screen.
    void start(DWORD mpv_pid, UINT32 width, UINT32 height);
    void stop();

private:
    void run();

    std::thread thread_;
    std::atomic<DWORD> thread_id_{0};
    std::atomic<bool> stopping_{false};
    DWORD pid_ = 0;
    UINT32 width_ = 0, height_ = 0;
};

}  // namespace glasir
