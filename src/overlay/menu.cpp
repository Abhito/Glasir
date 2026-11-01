// SPDX-License-Identifier: MIT
#include "menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "ini_edit.h"
#include "ui_style.h"

namespace glasir::overlay {
namespace {

using ui::kGold;
using ui::kTextDim;

// A value the menu shows that mpv owns: mpv's value, except for a moment
// after the menu changed it (until mpv's answer comes back) and while it is
// being dragged.
struct Synced {
    int value = 0;
    double hold_until = -1;
    void follow(int from_mpv, double now) {
        if (now >= hold_until) value = from_mpv;
    }
    void changed(double now) { hold_until = now + 0.75; }
};

struct MenuState {
    int last_frame = -10;
    Synced volume, boost, delay;
};
MenuState g_menu;

void save(const MenuFrame &f, const char *section, const char *key, const std::string &value) {
    if (f.settings_path.empty()) return;
    if (!ini_save_values(f.settings_path, {{section, key, value}}))
        reshade::log::message(reshade::log::level::warning, "Glasir menu: could not save a setting to glasir.ini");
}

ImFont *bold_font() {
    ImGuiIO &io = ImGui::GetIO();
    return io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
}

void section(const char *title) {
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
    ImFont *bold = bold_font();
    ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 0.8f);
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Separator();
}

void note(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// A note that matters now (gold).
void warn(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// A labelled slider across the panel; true when moved.
// Left and Right move it by step while it is highlighted (no need to press
// Enter on it first, as Dear ImGui would want).
bool slider(const char *label, const char *id, int *value, int lo, int hi, const char *format, int step) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    bool changed = ImGui::SliderInt(id, value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
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
    return changed;
}

std::string status_line(const MenuFrame &f, const MpvState &mpv) {
    char text[160];
    std::string out;
    if (mpv.width > 0 && mpv.height > 0) {
        std::snprintf(text, sizeof(text), "%dx%d", mpv.width, mpv.height);
        out = text;
    }
    out += out.empty() ? "" : "  \xC2\xB7  ";
    out += f.source_hdr ? "HDR" : "SDR";
    if (f.host && f.host->rates) {
        double source = 0, real = 0, generated = 0;
        f.host->rates(source, real, generated);
        if (generated >= 1)
            std::snprintf(text, sizeof(text), "  \xC2\xB7  %.0f \xE2\x86\x92 %.0f fps", source, real + generated);
        else if (source >= 1)
            std::snprintf(text, sizeof(text), "  \xC2\xB7  %.0f fps", source);
        else
            text[0] = 0;
        out += text;
    }
    return out;
}

void picture(const MenuFrame &f) {
    section("PICTURE");
    reshade::api::effect_runtime *rt = f.runtime;
    const reshade::api::effect_technique feed = rt->find_technique("DLSS5_Feed.fx", "DLSS5_Feed");
    const reshade::api::effect_technique kernel = rt->find_technique("lumenite_Kernel.fx", "Lumenite_Kernel");
    if (feed.handle == 0) {
        ImGui::BeginDisabled();
        bool off = false;
        ImGui::Checkbox("DLSS 5", &off);
        ImGui::EndDisabled();
        note("Not available: needs an NVIDIA RTX 50 series GPU and its download (Glasir.exe --setup).");
    } else {
        bool on = rt->get_technique_state(feed);
        static double switched_on = -100;
        if (ImGui::Checkbox("DLSS 5", &on)) {
            rt->set_technique_state(feed, on);
            if (kernel.handle != 0) rt->set_technique_state(kernel, on);
            save(f, "dlss5", "enabled", on ? "on" : "off");
            if (on) switched_on = ImGui::GetTime();
        }
        if (on && ImGui::GetTime() - switched_on < 10) note("DLSS 5 takes a few seconds to start.");
    }

    const Host &h = *f.host;
    const bool ready = h.framegen_ready && h.framegen_ready();
    if (!ready) ImGui::BeginDisabled();
    bool fg = ready && h.framegen_enabled && h.framegen_enabled();
    if (ImGui::Checkbox("Frame generation", &fg) && h.set_framegen_enabled) {
        h.set_framegen_enabled(fg);
        save(f, "framegen", "enabled", fg ? "1" : "0");
    }
    if (ready && fg) {
        bool fast = h.framegen_fast && h.framegen_fast();
        ImGui::Indent();
        ImGui::TextUnformatted("Quality");
        ImGui::SameLine();
        if (ImGui::RadioButton("High", !fast) && fast && h.set_framegen_fast) {
            h.set_framegen_fast(false);
            save(f, "framegen", "quality", "high");
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Fast", fast) && !fast && h.set_framegen_fast) {
            h.set_framegen_fast(true);
            save(f, "framegen", "quality", "fast");
        }
        ImGui::Unindent();
        if (h.framegen_status) {
            const FramegenStatus st = h.framegen_status();
            char text[200];
            if (st.kind == FramegenStatus::Paused) {
                std::snprintf(text, sizeof(text),
                              "Paused, back in %.0f s: the GPU could not keep up (mpv dropped frames). "
                              "Fast quality or a lighter DLSS 5 setting leaves it more room.",
                              std::ceil(st.seconds_left));
                warn(text);
            } else if (st.kind == FramegenStatus::TooFast) {
                std::snprintf(text, sizeof(text), "Not doubling: the display shows at most %d Hz.", st.display_hz);
                note(text);
            }
        }
    }
    if (!ready) {
        ImGui::EndDisabled();
        note("Frame generation needs Glasir's own capture with dedup (not the DirectShow fallback).");
    }
}

void sound(const MenuFrame &f, const MpvState &mpv, double now) {
    section("SOUND");
    MpvClient &c = *f.mpv;
    if (mpv.volume >= 0) g_menu.volume.follow(int(std::lround(mpv.volume)), now);
    if (slider("Volume", "##volume", &g_menu.volume.value, 0, 100, "%d %%", 2)) {
        g_menu.volume.changed(now);
        c.command({"set", "volume", std::to_string(g_menu.volume.value)});
    }
    if (ImGui::IsItemActive()) g_menu.volume.changed(now);
    bool mute = mpv.mute;
    if (ImGui::Checkbox("Mute", &mute)) c.command({"set", "mute", mute ? "yes" : "no"});

    if (mpv.boost < 0) {
        note("The card's sound is off or not found (logs\\audio.log).");
        return;
    }
    g_menu.boost.follow(mpv.boost, now);
    if (slider("Boost", "##boost", &g_menu.boost.value, 100, 400, "%d %%", 10)) {
        g_menu.boost.changed(now);
        c.command({"script-message", "glasir-audio-boost", std::to_string(g_menu.boost.value), "quiet"});
    }
    if (ImGui::IsItemActive()) g_menu.boost.changed(now);
    g_menu.delay.follow(std::max(mpv.delay, 0), now);
    if (slider("Audio delay", "##delay", &g_menu.delay.value, 0, 500, "%d ms", 5)) {
        g_menu.delay.changed(now);
        c.command({"script-message", "glasir-audio-delay", std::to_string(g_menu.delay.value), "quiet"});
    }
    if (ImGui::IsItemActive()) g_menu.delay.changed(now);
    note("Delay holds the sound back to match the picture.");
}

void display(const MenuFrame &f, const MpvState &mpv) {
    section("DISPLAY");
    bool full = mpv.fullscreen;
    if (ImGui::Checkbox("Full screen", &full)) f.mpv->command({"set", "fullscreen", full ? "yes" : "no"});
    const bool hdr = f.source_hdr;
    if (ImGui::Button(hdr ? "Switch to SDR" : "Switch to HDR", ImVec2(-1, 0))) {
        // Glasir starts the picture again in the other mode (mpv's exit
        // code 42 asks for that).
        save(f, "video", "mode", hdr ? "sdr" : "hdr");
        f.mpv->command({"quit", "42"});
    }
    note(hdr ? "Restarts the picture in SDR." : "Restarts the picture in HDR (needs HDR on the console and a card "
                                                "with a 10-bit mode).");
}

}  // namespace

void apply_style(float scale) { ui::apply_style(scale, 0.90f); }

bool draw_menu(const MenuFrame &f) {
    ImGuiIO &io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    const bool opened = ImGui::GetFrameCount() != g_menu.last_frame + 1;
    g_menu.last_frame = ImGui::GetFrameCount();

    const float width = std::min(io.DisplaySize.x, std::max(io.DisplaySize.x * 0.26f, 380.0f * f.scale));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - width, 0));
    ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y));
    if (opened) ImGui::SetNextWindowFocus();
    bool keep = true;
    ImGui::Begin("Glasir", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    if (opened) ImGui::SetNavCursorVisible(true);  // the first item shows as chosen at once
    const MpvState mpv = f.mpv->state();

    ImGui::PushFont(bold_font(), ImGui::GetStyle().FontSizeBase * 1.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextUnformatted("Glasir");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    note(status_line(f, mpv).c_str());

    picture(f);
    if (mpv.connected) {
        sound(f, mpv, now);
        display(f, mpv);
    } else {
        section("SOUND");
        note("Connecting to mpv...");
    }

    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 2));
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Close", ImVec2(half, 0))) keep = false;
    ImGui::SameLine();
    if (ImGui::Button("Quit Glasir", ImVec2(half, 0))) f.mpv->command({"quit"});
    note("Tab or Esc: close   \xC2\xB7   F6: DLSS 5   \xC2\xB7   F8: frame generation");
    ImGui::End();
    return keep;
}

}  // namespace glasir::overlay
