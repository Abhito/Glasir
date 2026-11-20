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
//
// It also shows toasts: short messages at the top of the picture for the
// keys (volume, mute, boost, delay, frame generation, screenshots) instead of
// mpv's OSD, which would go through DLSS 5 and frame generation. mpv's
// volume and mute are followed; anything else sends "script-message
// glasir-toast <text> [ms]" to mpv, or calls toast() in this add-on.
#pragma once
#include <d3d11.h>

#include <string>
#include <vector>

#include <reshade.hpp>

namespace glasir::overlay {

// Why frame generation is not doubling the frame rate just now, if it is not.
struct FramegenStatus {
    // 3x and 4x that do not reach their rate - Fewer: the display shows no
    // more than `frames` for each of the source's. Behind: the GPU has no time
    // for more than `frames`. Short: frames are being left out.
    enum Kind { Running, Paused, TooFast, Fewer, Behind, Short } kind = Running;
    double seconds_left = 0;  // Paused: until it tries again
    int display_hz = 0;       // TooFast, Fewer: the most the display shows
    int frames = 0;           // Fewer, Behind: how many it is
    int wanted = 0;           // Fewer, Behind, Short: how many were chosen
    double fps = 0, target_fps = 0;  // Short: what reaches the screen, and what was chosen
};

// What the add-on around the menu does (frame generation).
struct Host {
    bool (*framegen_ready)() = nullptr;  // it can run (Glasir's dedup link is there, it did not fail)
    bool (*framegen_enabled)() = nullptr;
    void (*set_framegen_enabled)(bool on) = nullptr;
    bool (*framegen_fast)() = nullptr;
    void (*set_framegen_fast)(bool fast) = nullptr;
    // 2, 3 or 4 frames shown for each of the source's.
    int (*multiplier)() = nullptr;
    void (*set_multiplier)(int frames) = nullptr;
    // NVIDIA Video Frame Generation in place of the add-on's own generator.
    bool (*nvidia_offered)() = nullptr;         // it is installed
    bool (*framegen_nvidia)() = nullptr;        // it is chosen
    void (*set_framegen_nvidia)(bool on) = nullptr;
    int (*nvidia_quality)() = nullptr;          // 0 low, 1 medium, 2 high, 3 auto: the add-on picks
    int (*nvidia_quality_now)() = nullptr;      // the model in use: 0 low, 1 medium, 2 high
    void (*set_nvidia_quality)(int quality) = nullptr;
    int (*nvidia_state)() = nullptr;            // 1 = it makes the frames, 0 = not started yet, -1 = it could not
    // Frames per second over the last seconds: from the source, real and
    // generated as presented.
    void (*rates)(double &source, double &real, double &generated) = nullptr;
    FramegenStatus (*framegen_status)() = nullptr;
    // NVIDIA Reflex's low latency mode on mpv's device: 0 off, 1 on, 2 on
    // with boost; -1: not offered (no NVIDIA driver, or it has no such call).
    int (*low_latency)() = nullptr;
    void (*set_low_latency)(int mode) = nullptr;
    // G-SYNC as the NVIDIA driver has it for mpv's window: 1 on, 0 off, -1 not known (another GPU).
    int (*gsync)() = nullptr;
    // Windows composes mpv's window instead of flipping its frames straight to the screen (the driver
    // says): 1 composed, 0 straight, -1 not known. Composed, G-SYNC cannot follow it.
    int (*composed)() = nullptr;
    std::string mpv_pipe = "glasir";  // mpv's IPC pipe name
};
void set_host(const Host &host);

// ReShade's reshade_present for mpv's swap chain, before frame generation
// draws the frame in between into the back buffer: takes Glasir's
// screenshot (the s key, screenshot.h) of mpv's real frame when one was
// asked for, and finishes one under way.
void before_frame_generation(reshade::api::effect_runtime *runtime);

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

// The picture's part of a width x height back buffer: without the black bars
// mpv draws around it (all of it until mpv has said where the picture is).
// On the thread of mpv's presents.
D3D11_BOX picture_box(UINT width, UINT height);

// The menu is open (mpv should get every frame, frame generation rest).
bool menu_open();

// What mpv shows now, as the menu's line to it has it: for the add-on's own
// scaling of the picture (NVIDIA Super Resolution).
struct Shown {
    bool connected = false;
    std::string scaling;            // glasir_scaling.lua's mode
    bool unscaled = false;          // it has mpv draw the picture as large as it comes
    int video_w = 0, video_h = 0;   // the picture as it comes
    int target = 0;                 // "Scale to": a height in lines, 0 for the picture's place in the window
};
Shown shown();

// Where mpv itself has the picture in a width x height back buffer (its
// black bars left out), whatever set_scaled_picture() says.
D3D11_BOX mpv_picture_box(UINT width, UINT height);

// The add-on made the picture larger than mpv drew it: picture_box() is this
// from now on. Null: as mpv has it again.
void set_scaled_picture(const D3D11_BOX *box);

// NVIDIA Super Resolution's part of the menu's Host, set apart from frame
// generation's: its model (0 low .. 3 ultra), and whether it runs (1), has
// nothing to do or is not chosen (0), or could not (-1). made: the size it
// made the picture on the frame being presented, before that went to its
// place in the window (false: it did not scale this frame).
struct ScalerHost {
    int (*level)() = nullptr;
    void (*set_level)(int level) = nullptr;
    int (*state)() = nullptr;
    bool (*made)(int &width, int &height) = nullptr;
};
void set_scaler_host(const ScalerHost &host);
const ScalerHost &scaler_host();

// Shows a toast for seconds (any thread).
void toast(const std::string &text, double seconds = 1.5);

// The menu's line to mpv, for the add-on around it: whether mpv is listening,
// and a command for it (queued; sent in order).
bool mpv_connected();
void mpv_command(const std::vector<std::string> &words);

// mpv's device is going away: let go of everything made on it.
void release();

}  // namespace glasir::overlay
