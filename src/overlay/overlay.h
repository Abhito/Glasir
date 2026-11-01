// SPDX-License-Identifier: MIT
// Glasir's in-game menu, drawn by the ReShade add-on it is built into, after
// everything else: after ReShade's effects (DLSS 5) and after frame
// generation, on every frame that reaches the screen - the frame in between
// and the real frame alike. Nothing of it goes into DLSS 5 or into frame
// generation, so it stays sharp and answers at once.
//
// Tab opens and closes it (Esc closes). While it is open, keys and the mouse
// go to the menu, not to mpv; mpv gets every frame and frame generation
// rests, so the menu redraws at the capture rate even on a still picture.
#pragma once
#include <d3d11.h>

#include <string>

#include <reshade.hpp>

namespace glasir::overlay {

// Why frame generation is not doubling the frame rate just now, if it is not.
struct FramegenStatus {
    enum Kind { Running, Paused, TooFast } kind = Running;
    double seconds_left = 0;  // Paused: until it tries again
    int display_hz = 0;       // TooFast: the most the display shows
};

// What the add-on around the menu does (frame generation).
struct Host {
    bool (*framegen_ready)() = nullptr;  // it can run (Glasir's dedup link is there, it did not fail)
    bool (*framegen_enabled)() = nullptr;
    void (*set_framegen_enabled)(bool on) = nullptr;
    bool (*framegen_fast)() = nullptr;
    void (*set_framegen_fast)(bool fast) = nullptr;
    // Frames per second over the last seconds: from the source, real and
    // generated as presented.
    void (*rates)(double &source, double &real, double &generated) = nullptr;
    FramegenStatus (*framegen_status)() = nullptr;
    std::string mpv_pipe = "glasir";  // mpv's IPC pipe name
};
void set_host(const Host &host);

// ReShade's reshade_present for mpv's swap chain, after frame generation:
// reads input, builds the menu and draws it on the back buffer (the image
// mpv presents now).
void on_reshade_present(reshade::api::effect_runtime *runtime);

// Draws the same menu again on another image about to be presented (the
// real frame that frame generation presents itself). context is the device's
// immediate context; back_buffer the swap chain's buffer 0.
void draw_again(ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer);

// The colour space of mpv's present (ReShade's present event): the menu's
// encoding (SDR, HDR10, scRGB).
void note_color_space(reshade::api::color_space color_space);

// The menu is open (mpv should get every frame, frame generation rest).
bool menu_open();

// mpv's device is going away: let go of everything made on it.
void release();

}  // namespace glasir::overlay
