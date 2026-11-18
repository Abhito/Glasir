// SPDX-License-Identifier: MIT
// Glasir's look in Dear ImGui (the in-game menu and the launcher): gold on a
// dark panel, groups on cards a shade lighter. ui_widgets.h has the controls
// drawn in it. Header only; include it where imgui.h is available.
#pragma once
#include <imgui.h>

namespace glasir::ui {

inline const ImVec4 kGold(0.88f, 0.69f, 0.25f, 1.0f);
inline const ImVec4 kGoldDim(0.88f, 0.69f, 0.25f, 0.55f);
inline const ImVec4 kGoldBright(1.0f, 0.82f, 0.40f, 1.0f);
inline const ImVec4 kOnGold(0.09f, 0.08f, 0.06f, 1.0f);  // text and knobs on gold
inline const ImVec4 kText(0.94f, 0.94f, 0.95f, 1.0f);
inline const ImVec4 kTextDim(0.62f, 0.62f, 0.65f, 1.0f);
inline const ImVec4 kCard(1.0f, 1.0f, 1.0f, 0.05f);  // a group's panel on the window
inline const ImVec4 kAlert(1.0f, 0.45f, 0.40f, 1.0f);  // what is not working as chosen

// The style at this scale (1 = 1080 lines for the menu, 96 DPI for the
// launcher); window_alpha: the panel's opacity.
inline void apply_style(float scale, float window_alpha) {
    ImGuiStyle s;
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(22, 20);
    s.FramePadding = ImVec2(12, 7);
    s.ItemSpacing = ImVec2(10, 10);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.GrabMinSize = 14;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 0;
    s.FrameRounding = 8;
    s.GrabRounding = 8;
    s.PopupRounding = 10;
    s.ChildRounding = 12;
    s.WindowRounding = 0;
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 8;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.055f, 0.065f, window_alpha);
    c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.11f, 0.125f, 0.98f);
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextDim;
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.17f);
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_SliderGrab] = kGold;
    c[ImGuiCol_SliderGrabActive] = kGoldBright;
    // Quiet buttons: gold is for what is on and for the one thing to press.
    c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.15f);
    c[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.22f);
    c[ImGuiCol_Header] = ImVec4(0.88f, 0.69f, 0.25f, 0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.16f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.22f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.30f);
    c[ImGuiCol_NavCursor] = ImVec4(1, 1, 1, 0.85f);  // white: gold would not show on a gold button
    s.ScaleAllSizes(scale);
    s.FontSizeBase = 20.0f;
    s.FontScaleMain = scale;
    ImGui::GetStyle() = s;
}

}  // namespace glasir::ui
