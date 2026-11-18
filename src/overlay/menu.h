// SPDX-License-Identifier: MIT
// What the in-game menu shows and does (Dear ImGui, between NewFrame and
// Render).
#pragma once
#include <string>

#include <reshade.hpp>

#include "compositor.h"
#include "mpv_client.h"
#include "overlay.h"

namespace glasir::overlay {

struct MenuFrame {
    reshade::api::effect_runtime *runtime = nullptr;
    const Host *host = nullptr;
    MpvClient *mpv = nullptr;
    Encoding encoding = Encoding::Sdr;
    std::wstring settings_path;  // glasir.ini, where the menu keeps what it changes ("" = nowhere)
    // As mpv started; the menu follows what changes while playing itself
    // (MpvState's signal and rtx_hdr, and its own switches).
    bool source_hdr = false;     // the capture is shown as HDR, whatever mpv outputs
    int picture = 1;             // glasir.ini video.mode: 0 auto (as the signal turns out to be), 1 sdr, 2 hdr
    bool picture_live = false;   // a 10-bit capture: Glasir switches the picture without starting it again
    int rtx_hdr = -1;            // glasir.ini video.rtx_hdr; -1: not offered (no NVIDIA RTX GPU)
    bool hdr_available = true;   // the card has an HDR mode (false: no "Switch to HDR")
    bool borderless = true;      // glasir.ini window.mode when mpv started (the menu follows its own switches)
    float scale = 1;             // 1 at 1080 lines
};

// A toast at the top of the picture; alpha fades it out.
void draw_toast(const std::string &text, float alpha, float scale);

// The theme, at this scale.
void apply_style(float scale);

// Draws the menu. False when it asks to be closed.
bool draw_menu(const MenuFrame &frame);

}  // namespace glasir::overlay
