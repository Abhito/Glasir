// SPDX-License-Identifier: MIT
// Glasir's controls in Dear ImGui, for the launcher and the in-game menu: a
// card around a group, a switch, a row of choices, a slider with its value
// beside its label, and a row that opens the settings few people change.
// Each is an ordinary ImGui item underneath (an invisible button or a
// slider), so the mouse, the keyboard's arrows and Space work on them as on
// any other (Dear ImGui draws the keyboard's ring around them). Header only;
// include it where imgui.h is available.
#pragma once
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "ui_style.h"

namespace glasir::ui {

inline ImFont *bold_font() {
    ImGuiIO &io = ImGui::GetIO();
    return io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
}

// A value that moves to its target over a few frames, kept for the last item.
inline float eased(float target) {
    float *value = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), target);
    *value += (target - *value) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
    if (std::abs(target - *value) < 0.01f) *value = target;
    return *value;
}

inline void note(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// A note that matters now.
inline void warn(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Something is not working as it was chosen (red).
inline void alert(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kAlert);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// What the last item is for, shown while the mouse rests on it.
inline void tooltip(const char *text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    const float size = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(size * 0.6f, size * 0.45f));
    if (ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(size * 20.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar();
}

// A group's name above its card.
inline void section(const char *title) {
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 0.35f));
    ImGui::PushFont(bold_font(), ImGui::GetStyle().FontSizeBase * 0.72f);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// A panel around what follows, as tall as it. The keyboard moves through
// it as through the window around it.
inline void begin_card(const char *id) {
    const ImGuiStyle &s = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, s.FrameRounding * 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(s.WindowPadding.x * 0.72f, s.WindowPadding.y * 0.7f));
    ImGui::BeginChild(id, ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize |
                          ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}

inline void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// A faint line between two parts of a card.
inline void divider() {
    const ImGuiStyle &s = ImGui::GetStyle();
    ImGui::Dummy(ImVec2(0, s.ItemSpacing.y * 0.1f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y),
                                        ImGui::GetColorU32(ImVec4(1, 1, 1, 0.08f)));
    ImGui::Dummy(ImVec2(0, s.ItemSpacing.y * 0.1f));
}

// A switch: the label at the left, the switch at the right, the whole line
// to click; tip: what it is for (tooltip()). True when it was switched.
inline bool toggle(const char *label, bool *value, const char *tip = nullptr) {
    const ImGuiStyle &s = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight(), w = ImGui::GetContentRegionAvail().x;
    const float track_h = std::floor(h * 0.72f), track_w = std::floor(track_h * 1.85f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##toggle", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopID();
    if (pressed) *value = !*value;
    if (tip) tooltip(tip);
    const bool hovered = ImGui::IsItemHovered();
    const float on = eased(*value ? 1.0f : 0.0f);
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddText(ImVec2(p.x, p.y + s.FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), label);
    const ImVec2 t0(p.x + w - track_w, p.y + std::floor((h - track_h) * 0.5f)), t1(p.x + w, t0.y + track_h);
    const float off_alpha = hovered ? 0.24f : 0.16f;
    const ImVec4 track(1.0f + (kGold.x - 1.0f) * on, 1.0f + (kGold.y - 1.0f) * on, 1.0f + (kGold.z - 1.0f) * on,
                       off_alpha + (1.0f - off_alpha) * on);
    d->AddRectFilled(t0, t1, ImGui::GetColorU32(track), track_h * 0.5f);
    const float r = track_h * 0.5f, x = t0.x + r + (track_w - track_h) * on;
    const ImVec4 knob(0.95f - 0.86f * on, 0.95f - 0.87f * on, 0.95f - 0.89f * on, 1.0f);
    d->AddCircleFilled(ImVec2(x, t0.y + r), r - std::max(2.0f, track_h * 0.14f), ImGui::GetColorU32(knob), 24);
    return pressed;
}

// A row of choices, one chosen, across `width` (0: what is left of the
// line); tip: what they are for. The one clicked if it is not the chosen
// one, else -1.
inline int segmented(const char *id, std::initializer_list<const char *> labels, int current, float width = 0,
                     const char *tip = nullptr) {
    const ImGuiStyle &s = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    float text = 0, widest = 0;
    for (const char *label : labels) {
        const float w = ImGui::CalcTextSize(label).x;
        text += w;
        widest = std::max(widest, w);
    }
    // Equal parts where the longest label fits in one; else each its text
    // and an equal share of the room left.
    const float part = width / float(labels.size());
    const bool equal = widest + s.FramePadding.x <= part;
    const float share = (width - text) / float(labels.size());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *d = ImGui::GetWindowDrawList();
    const float inset = std::max(2.0f, h * 0.08f), round = s.FrameRounding;
    d->AddRectFilled(p, ImVec2(p.x + width, p.y + h), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.07f)), round);
    ImGui::PushID(id);
    int clicked = -1, i = 0;
    for (const char *label : labels) {
        const float w = std::floor(equal ? part : ImGui::CalcTextSize(label).x + share);
        if (i > 0) ImGui::SameLine(0, 0);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##choice", ImVec2(std::max(w, 1.0f), h), ImGuiButtonFlags_EnableNav) && i != current) clicked = i;
        ImGui::PopID();
        if (tip) tooltip(tip);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool chosen = i == current, hovered = ImGui::IsItemHovered();
        if (chosen)
            d->AddRectFilled(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset), ImGui::GetColorU32(kGold),
                             std::max(0.0f, round - inset));
        else if (hovered)
            d->AddRectFilled(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset),
                             ImGui::GetColorU32(ImVec4(1, 1, 1, 0.09f)), std::max(0.0f, round - inset));
        const ImVec2 size = ImGui::CalcTextSize(label);
        const ImVec4 ink = chosen ? kOnGold : hovered ? kText : ImVec4(kText.x, kText.y, kText.z, 0.78f);
        d->AddText(ImVec2(std::floor(a.x + (b.x - a.x - size.x) * 0.5f), a.y + s.FramePadding.y), ImGui::GetColorU32(ink), label);
        ++i;
    }
    ImGui::PopID();
    return clicked;
}

// The width segmented() needs to show these choices unsqueezed.
inline float segmented_width(std::initializer_list<const char *> labels) {
    float text = 0;
    for (const char *label : labels) text += ImGui::CalcTextSize(label).x;
    return text + float(labels.size()) * ImGui::GetStyle().FramePadding.x * 2.0f;
}

// A label, and the choices in the rest of the line from `column` on - or
// under the label, across the line, where they would be squeezed there.
inline int choice(const char *label, std::initializer_list<const char *> labels, int current, float column,
                  const char *tip = nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tip) tooltip(tip);
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    if (segmented_width(labels) <= right - column) ImGui::SameLine(column);
    return segmented(label, labels, current, 0, tip);
}

// A slider across the line with its label and value above it; true when
// moved. Left and Right move it by `step` while the keyboard is on it (no
// need to press Enter on it first, as Dear ImGui would want).
inline bool slider(const char *label, const char *id, int *value, int lo, int hi, const char *format, int step) {
    const ImGuiStyle &s = ImGui::GetStyle();
    char text[32];
    std::snprintf(text, sizeof(text), format, *value);
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(label);
    ImGui::SameLine(right - ImGui::CalcTextSize(text).x);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();

    // Dear ImGui's slider does the work, unseen; the track and knob are drawn over it.
    ImGui::SetNextItemWidth(-1);
    const ImVec4 none(0, 0, 0, 0);
    for (ImGuiCol c : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab,
                       ImGuiCol_SliderGrabActive})
        ImGui::PushStyleColor(c, none);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.FramePadding.x, s.FramePadding.y * 0.45f));
    bool changed = ImGui::SliderInt(id, value, lo, hi, "", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    if (ImGui::IsItemFocused() && !ImGui::IsItemActive()) {
        int by = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) by -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) by += step;
        const int next = std::clamp(*value + by, lo, hi);
        if (next != *value) {
            *value = next;
            changed = true;
        }
    }
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool lit = ImGui::IsItemHovered() || ImGui::IsItemActive();
    const float y = std::floor((a.y + b.y) * 0.5f), r = std::floor((b.y - a.y) * 0.34f), half = std::max(1.5f, r * 0.28f);
    const float t = hi > lo ? float(*value - lo) / float(hi - lo) : 0.0f;
    const float x = a.x + r + (b.x - a.x - 2 * r) * t;
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(ImVec2(a.x, y - half), ImVec2(b.x, y + half), ImGui::GetColorU32(ImVec4(1, 1, 1, lit ? 0.20f : 0.14f)), half);
    d->AddRectFilled(ImVec2(a.x, y - half), ImVec2(x, y + half), ImGui::GetColorU32(kGold), half);
    d->AddCircleFilled(ImVec2(x, y), lit ? r + 1.0f : r, ImGui::GetColorU32(lit ? kGoldBright : kGold), 24);
    return changed;
}

// A list to pick from: Dear ImGui's combo with a small arrow of ours at
// its right end. As ImGui::BeginCombo: true while its list is open (then
// the items, then ImGui::EndCombo).
inline bool begin_combo(const char *id, const char *preview) {
    // Drawn first, under the frame, which lets it show through.
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::CalcItemWidth(), h = ImGui::GetFrameHeight(), a = ImGui::GetFontSize() * 0.2f;
    const ImVec2 c(p.x + w - ImGui::GetStyle().FramePadding.x - a, p.y + h * 0.5f + a * 0.3f);
    ImDrawList *d = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::GetColorU32(kTextDim);
    const float t = std::max(1.5f, ImGui::GetFontSize() * 0.09f);
    d->AddLine(ImVec2(c.x - a, c.y - a), ImVec2(c.x, c.y), ink, t);
    d->AddLine(ImVec2(c.x, c.y), ImVec2(c.x + a, c.y - a), ink, t);
    return ImGui::BeginCombo(id, preview, ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_HeightLarge);
}

// A line that shows or hides what few people change: a small arrow and a
// label. Returns *open.
inline bool disclosure(const char *label, bool *open) {
    const ImGuiStyle &s = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight(), w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(label, ImVec2(w, h), ImGuiButtonFlags_EnableNav)) *open = !*open;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *d = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::GetColorU32(hovered ? kText : kTextDim);
    const float size = ImGui::GetFontSize(), a = size * 0.22f;
    const ImVec2 c(p.x + size * 0.35f, p.y + h * 0.5f);
    if (*open)
        d->AddTriangleFilled(ImVec2(c.x - a, c.y - a * 0.6f), ImVec2(c.x + a, c.y - a * 0.6f), ImVec2(c.x, c.y + a * 0.8f), ink);
    else
        d->AddTriangleFilled(ImVec2(c.x - a * 0.6f, c.y - a), ImVec2(c.x - a * 0.6f, c.y + a), ImVec2(c.x + a * 0.8f, c.y), ink);
    d->AddText(ImVec2(p.x + size * 0.95f, p.y + s.FramePadding.y), ink, label);
    return *open;
}

}  // namespace glasir::ui
