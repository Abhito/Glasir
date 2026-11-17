// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// See reshade_ui.h.

#include "reshade_ui.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>

// Dear ImGui's header for the types in ReShade's table only: no function of
// it is called in this file.
#include <imgui.h>

// ReShade's Dear ImGui is the docking branch, third_party/imgui is not: the
// two types of it that the table's function signatures name. Only the table's
// layout matters here, and it is all function pointers.
typedef int ImGuiDockNodeFlags;
struct ImGuiWindowClass;

// With this defined, reshade_overlay.hpp declares the table and leaves out the
// inline functions with Dear ImGui's names (reshade_ui.h says why). reshade.hpp
// is not included here, so it means nothing else.
#define RESHADE_API_LIBRARY_EXPORT
#include <reshade_overlay.hpp>
#undef RESHADE_API_LIBRARY_EXPORT

namespace fg::reshade_ui {
namespace {

const imgui_function_table *g_ui = nullptr;

void call_v(void (*function)(const char *, va_list), const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    function(fmt, args);
    va_end(args);
}

}  // namespace

bool init(void *reshade_module) {
    if (!reshade_module) return false;
    const auto get = reinterpret_cast<const imgui_function_table *(*)(uint32_t)>(
        GetProcAddress(static_cast<HMODULE>(reshade_module), "ReShadeGetImGuiFunctionTable"));
    g_ui = get ? get(IMGUI_VERSION_NUM) : nullptr;
    return g_ui != nullptr;
}

void heading(const char *text) { g_ui->SeparatorText(text); }
bool checkbox(const char *label, bool *value) { return g_ui->Checkbox(label, value); }
void tooltip(const char *text) { call_v(g_ui->SetItemTooltipV, "%s", text); }
void text(const char *text) { g_ui->TextUnformatted(text, nullptr); }
void note(const char *text) { call_v(g_ui->TextDisabledV, "%s", text); }
void space() { g_ui->Spacing(); }
void begin_disabled(bool disabled) { g_ui->BeginDisabled(disabled); }
void end_disabled() { g_ui->EndDisabled(); }

}  // namespace fg::reshade_ui
