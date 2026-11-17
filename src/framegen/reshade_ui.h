// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// The few widgets of ReShade's Dear ImGui that the add-on's panel on ReShade's
// Add-ons page is made of (glasir_framegen.cpp draws it).
//
// The add-on has a Dear ImGui of its own, for Glasir's in-game menu
// (src/overlay). ReShade's header for add-ons (reshade_overlay.hpp) offers
// ReShade's as inline functions with Dear ImGui's own names: in one DLL with
// the real ones, the linker keeps one of each name for both. So nothing here
// goes by those names: reshade_ui.cpp calls ReShade's functions through the
// table ReShade hands out, and is the only file that knows it.
#pragma once

namespace fg::reshade_ui {

// reshade_module: ReShade's DLL. False if it draws no overlay, or with a Dear
// ImGui other than the one reshade_overlay.hpp is for: no panel then.
bool init(void *reshade_module);

void heading(const char *text);
bool checkbox(const char *label, bool *value);
void tooltip(const char *text);  // for the widget before it
void text(const char *text);
void note(const char *text);     // dimmed
void space();
void begin_disabled(bool disabled);
void end_disabled();

}  // namespace fg::reshade_ui
