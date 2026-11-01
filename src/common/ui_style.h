// SPDX-License-Identifier: MIT
// Glasir's look in Dear ImGui (the in-game menu and the launcher): gold on a
// dark panel. Header only; include it where imgui.h is available.
#pragma once
#include <imgui.h>

namespace glasir::ui {

inline const ImVec4 kGold(0.88f, 0.69f, 0.25f, 1.0f);
inline const ImVec4 kGoldDim(0.88f, 0.69f, 0.25f, 0.55f);
inline const ImVec4 kText(0.93f, 0.93f, 0.93f, 1.0f);
inline const ImVec4 kTextDim(0.62f, 0.62f, 0.62f, 1.0f);

// The style at this scale (1 = 1080 lines for the menu, 96 DPI for the
// launcher); window_alpha: the panel's opacity.
inline void apply_style(float scale, float window_alpha) {
    ImGuiStyle s;
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(22, 18);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 9);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.GrabMinSize = 14;
    s.WindowBorderSize = 0;
    s.PopupBorderSize = 0;
    s.FrameRounding = 6;
    s.GrabRounding = 6;
    s.PopupRounding = 6;
    s.WindowRounding = 0;
    s.ScrollbarSize = 10;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.07f, window_alpha);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.11f, 0.98f);
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextDim;
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.20f);
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_SliderGrab] = kGold;
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.82f, 0.40f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_ButtonHovered] = kGoldDim;
    c[ImGuiCol_ButtonActive] = kGold;
    c[ImGuiCol_Header] = kGoldDim;
    c[ImGuiCol_HeaderHovered] = kGoldDim;
    c[ImGuiCol_HeaderActive] = kGold;
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_NavCursor] = kGold;
    s.ScaleAllSizes(scale);
    s.FontSizeBase = 20.0f;
    s.FontScaleMain = scale;
    ImGui::GetStyle() = s;
}

}  // namespace glasir::ui
