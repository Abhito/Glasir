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
    bool source_hdr = false;     // the capture is HDR (glasir.ini video.mode), whatever mpv outputs
    float scale = 1;             // 1 at 1080 lines
};

// A toast at the top of the picture; alpha fades it out.
void draw_toast(const std::string &text, float alpha, float scale);

// The theme, at this scale.
void apply_style(float scale);

// Draws the menu. False when it asks to be closed.
bool draw_menu(const MenuFrame &frame);

}  // namespace glasir::overlay
