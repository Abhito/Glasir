// SPDX-License-Identifier: MIT
#include "menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>

#include <imgui.h>

#include "ini_edit.h"
#include "ui_style.h"
#include "ui_widgets.h"

namespace glasir::overlay {
namespace {

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
    int borderless = -1;  // full screen (1) or a window (0) since the menu switched it; -1: as mpv started
    int picture = -1;     // Automatic, SDR or HDR since the menu switched it; -1: as mpv started
    int rtx_hdr = -1;     // RTX HDR for SDR likewise
    bool more_picture = false, more_sound = false;  // a card's "More" is open
};
MenuState g_menu;

void save(const MenuFrame &f, const char *section, const char *key, const std::string &value) {
    if (f.settings_path.empty()) return;
    if (!ini_save_values(f.settings_path, {{section, key, value}}))
        reshade::log::message(reshade::log::level::warning, "Glasir menu: could not save a setting to glasir.ini");
}

// Where a line's control starts, after its label (ui::choice puts the
// choices under the label where they would be squeezed beside it).
float column() { return ImGui::GetFontSize() * 4.4f; }

// What the frames are shown as now: Glasir says when it changes while playing.
bool signal_hdr(const MenuFrame &f, const MpvState &mpv) { return mpv.signal.empty() ? f.source_hdr : mpv.signal == "hdr"; }
int picture_mode(const MenuFrame &f) { return g_menu.picture >= 0 ? g_menu.picture : f.picture; }

std::string status_line(const MenuFrame &f, const MpvState &mpv) {
    char text[160];
    std::string out;
    if (mpv.width > 0 && mpv.height > 0) {
        std::snprintf(text, sizeof(text), "%dx%d", mpv.width, mpv.height);
        out = text;
    }
    out += out.empty() ? "" : "  \xC2\xB7  ";
    out += signal_hdr(f, mpv) ? "HDR" : mpv.rtx_hdr == "on" ? "SDR (RTX HDR)" : "SDR";
    if (picture_mode(f) == 0) out += " (auto)";
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

void picture_more(const MenuFrame &f, const MpvState &mpv);

// What a player switches while playing: DLSS 5 and frame generation. What
// is set once is under the card's "More".
void picture(const MenuFrame &f, const MpvState &mpv) {
    ui::section("PICTURE");
    ui::begin_card("picture");
    reshade::api::effect_runtime *rt = f.runtime;
    const reshade::api::effect_technique feed = rt->find_technique("DLSS5_Feed.fx", "DLSS5_Feed");
    // LumeniteFX's motion vectors, in an install whose DLSS 5 is from Glasir
    // 0.1.3 or before (reshade_config.cpp); switched with the feed.
    const reshade::api::effect_technique kernel = rt->find_technique("lumenite_Kernel.fx", "Lumenite_Kernel");
    bool dlss5_on = false;  // for what to say when frame generation falls short
    if (feed.handle == 0) {
        ImGui::BeginDisabled();
        bool off = false;
        ui::toggle("DLSS 5", &off);
        ImGui::EndDisabled();
        ui::note("Not available: needs an NVIDIA RTX 30, 40 or 50 series GPU and its download (Glasir.exe --setup).");
    } else {
        bool on = rt->get_technique_state(feed);
        static double switched_on = -100;
        if (ui::toggle("DLSS 5", &on)) {
            rt->set_technique_state(feed, on);
            if (kernel.handle != 0) rt->set_technique_state(kernel, on);
            save(f, "dlss5", "enabled", on ? "on" : "off");
            if (on) switched_on = ImGui::GetTime();
        }
        if (on && ImGui::GetTime() - switched_on < 10) ui::note("DLSS 5 takes a few seconds to start.");
        dlss5_on = on;
    }

    const Host &h = *f.host;
    const bool ready = h.framegen_ready && h.framegen_ready();
    if (!ready) ImGui::BeginDisabled();
    bool fg = ready && h.framegen_enabled && h.framegen_enabled();
    if (ui::toggle("Frame generation", &fg) && h.set_framegen_enabled) {
        h.set_framegen_enabled(fg);
        save(f, "framegen", "enabled", fg ? "1" : "0");
    }
    if (ready && fg) {
        const bool fast = h.framegen_fast && h.framegen_fast();
        // NVIDIA Video Frame Generation, where it is installed: who makes the
        // frames, and its model's size in place of Glasir's quality.
        if (h.multiplier && h.set_multiplier) {
            const int frames = ui::choice("Rate", {"2x", "3x", "4x"}, h.multiplier() - 2, column(),
                                          "How many frames are shown for each of the game's: 2x makes 30 fps look "
                                          "like 60, 3x like 90, 4x like 120. Where the display or the GPU cannot keep "
                                          "up with that many, the highest that holds steady is shown.");
            if (frames >= 0) {
                h.set_multiplier(frames + 2);
                save(f, "framegen", "multiplier", std::to_string(frames + 2));
            }
        }
        const bool offered = h.nvidia_offered && h.nvidia_offered() && h.framegen_nvidia && h.set_framegen_nvidia;
        bool nvidia = offered && h.framegen_nvidia();
        if (offered) {
            const int by = ui::choice("Made by", {"Glasir", "NVIDIA"}, nvidia ? 1 : 0, column(),
                                      "Glasir: its own frame generation. NVIDIA: NVIDIA Video Frame Generation.");
            if (by >= 0) {
                nvidia = by == 1;
                h.set_framegen_nvidia(nvidia);
                save(f, "framegen", "method", nvidia ? "nvidia" : "glasir");
            }
        }
        // Chosen, but it could not run: Glasir's own makes the frames, and its quality is the one to set.
        const bool fallen_back = nvidia && h.nvidia_state && h.nvidia_state() < 0;
        if (fallen_back)
            ui::warn("NVIDIA Video Frame Generation could not run (ReShade.log says why): Glasir's own makes the frames.");
        nvidia = nvidia && !fallen_back;
        if (nvidia && h.nvidia_quality && h.set_nvidia_quality) {
            static const char *const kQuality[] = {"low", "medium", "high"};
            ImGui::PushID("nvidia");
            const int q = ui::choice("Quality", {"Low", "Medium", "High"}, h.nvidia_quality(), column(),
                                     "NVIDIA's model. A larger one is cleaner and takes more GPU time, which adds "
                                     "delay. High is for 30 fps games.");
            ImGui::PopID();
            if (q >= 0) {
                h.set_nvidia_quality(q);
                save(f, "framegen", "nvidia_quality", kQuality[q]);
            }
        } else {
            const int q = ui::choice("Quality", {"High", "Fast"}, fast ? 1 : 0, column(),
                                     "Fast takes about half the GPU time and is a little less clean.");
            if (q >= 0 && h.set_framegen_fast) {
                h.set_framegen_fast(q == 1);
                save(f, "framegen", "quality", q == 1 ? "fast" : "high");
            }
        }
        if (h.framegen_status) {
            const FramegenStatus st = h.framegen_status();
            char text[200];
            if (st.kind == FramegenStatus::Paused) {
                std::snprintf(text, sizeof(text),
                              "Paused, back in %.0f s: the GPU could not keep up (mpv dropped frames). "
                              "Fast quality or a lighter DLSS 5 setting leaves it more room.",
                              std::ceil(st.seconds_left));
                ui::warn(text);
            } else if (st.kind == FramegenStatus::TooFast) {
                std::snprintf(text, sizeof(text), "Not doubling: the display shows at most %d Hz.", st.display_hz);
                ui::note(text);
            } else if (st.kind == FramegenStatus::Fewer) {
                std::snprintf(text, sizeof(text), "Showing %dx, not %dx: the display shows at most %d Hz.", st.frames,
                              st.wanted, st.display_hz);
                ui::alert(text);
            } else if (st.kind == FramegenStatus::Behind) {
                // What would leave it room: DLSS 5 takes the most by far, then NVIDIA's larger models.
                const char *room = dlss5_on ? " Turning DLSS 5 off leaves it room."
                                   : nvidia && h.nvidia_quality && h.nvidia_quality() > 0 ? " A lower Quality leaves it room."
                                   : !nvidia && !fast ? " Fast quality leaves it room."
                                                      : "";
                std::snprintf(text, sizeof(text),
                              "Showing %dx, not %dx: the GPU cannot finish more frames in time here, and uneven "
                              "frames look worse than fewer.%s",
                              st.frames, st.wanted, room);
                ui::alert(text);
            } else if (st.kind == FramegenStatus::Short) {
                std::snprintf(text, sizeof(text),
                              "Reaching about %.0f of %.0f fps: frames are being left out, so motion is uneven. A "
                              "lower rate looks smoother.",
                              st.fps, st.target_fps);
                ui::alert(text);
            }
        }
    }
    if (!ready) {
        ImGui::EndDisabled();
        ui::note("Frame generation needs Glasir's own capture with dedup (not the DirectShow fallback).");
    }
    if (mpv.connected && ui::disclosure("More", &g_menu.more_picture)) picture_more(f, mpv);
    ui::end_card();
}

// Volume and mute; boost and delay are under the card's "More".
void sound(const MenuFrame &f, const MpvState &mpv, double now) {
    ui::section("SOUND");
    ui::begin_card("sound");
    MpvClient &c = *f.mpv;
    if (mpv.volume >= 0) g_menu.volume.follow(int(std::lround(mpv.volume)), now);
    if (ui::slider("Volume", "##volume", &g_menu.volume.value, 0, 100, "%d %%", 2)) {
        g_menu.volume.changed(now);
        c.command({"set", "volume", std::to_string(g_menu.volume.value)});
    }
    if (ImGui::IsItemActive()) g_menu.volume.changed(now);
    bool mute = mpv.mute;
    if (ui::toggle("Mute", &mute)) c.command({"set", "mute", mute ? "yes" : "no"});
    if (mpv.boost < 0) {
        ui::note("The card's sound is off or not found (logs\\audio.log).");
    } else if (ui::disclosure("More", &g_menu.more_sound)) {
        g_menu.boost.follow(mpv.boost, now);
        if (ui::slider("Boost", "##boost", &g_menu.boost.value, 100, 400, "%d %%", 10)) {
            g_menu.boost.changed(now);
            c.command({"script-message", "glasir-audio-boost", std::to_string(g_menu.boost.value), "quiet"});
        }
        if (ImGui::IsItemActive()) g_menu.boost.changed(now);
        g_menu.delay.follow(std::max(mpv.delay, 0), now);
        if (ui::slider("Audio delay", "##delay", &g_menu.delay.value, 0, 500, "%d ms", 5)) {
            g_menu.delay.changed(now);
            c.command({"script-message", "glasir-audio-delay", std::to_string(g_menu.delay.value), "quiet"});
        }
        if (ImGui::IsItemActive()) g_menu.delay.changed(now);
        ui::tooltip("Holds the sound back to match the picture, which DLSS 5 and frame generation make a little later.");
    }
    ui::end_card();
}

// The picture's settings that are set once, under its card's "More": how it
// is scaled and shown, and SDR or HDR.
void picture_more(const MenuFrame &f, const MpvState &mpv) {
    // Scaling up (glasir_scaling.lua in mpv): switched at once.
    if (!mpv.scaling.empty()) {
        static const char *const kModes[] = {"mpv", "fsr", "vsr", "cnn", "anime4k"};
        static const char *const kLabels[] = {"mpv (Lanczos)", "FSR 1 (sharper)", "VSR (NVIDIA RTX, Intel)",
                                              "ArtCNN (more GPU time)", "Anime4K (more GPU time)"};
        int current = -1;
        for (int i = 0; i < 5; ++i)
            if (mpv.scaling == kModes[i]) current = i;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Scaling");
        ui::tooltip("How a capture smaller than the screen is scaled up, before DLSS 5.");
        ImGui::SameLine(column());
        ImGui::SetNextItemWidth(-1);
        if (ui::begin_combo("##scaling", current >= 0 ? kLabels[current] : mpv.scaling.c_str())) {
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(kLabels[i], i == current) && i != current) {
                    f.mpv->command({"script-message", "glasir-scaling", kModes[i]});
                    save(f, "video", "scaling", kModes[i]);
                }
                if (i == current) ImGui::SetItemDefaultFocus();  // the keyboard starts on the one in use
            }
            ImGui::EndCombo();
        }
    }

    // Glasir moves mpv's window (and the black around a picture of another
    // shape than the screen); mpv changes its border.
    // Full screen is mpv's own (f and a double-click switch it too); leaving
    // it goes back to the borderless window or the window it came from.
    const bool borderless = g_menu.borderless < 0 ? f.borderless : g_menu.borderless == 1;
    const int shown = ui::choice("Show as", {"Full screen", "Borderless", "Window"}, mpv.fullscreen ? 0 : borderless ? 1 : 2,
                                 column());
    if (shown == 0) {
        f.mpv->command({"set", "fullscreen", "yes"});
        save(f, "window", "mode", "fullscreen");
    } else if (shown > 0) {
        g_menu.borderless = shown == 1 ? 1 : 0;
        f.mpv->command({"script-message", "glasir-window", shown == 1 ? "borderless" : "windowed"});
        save(f, "window", "mode", shown == 1 ? "borderless" : "windowed");
    }
    const bool hdr = signal_hdr(f, mpv);
    if (hdr || f.hdr_available) {  // a card without HDR modes has nothing to switch to
        static constexpr const char *kPicture[] = {"auto", "sdr", "hdr"};
        const int mode = picture_mode(f);
        const int picture = ui::choice(
            "Picture", {"Automatic", "SDR", "HDR"}, mode, column(),
            f.picture_live ? "Automatic tells SDR from HDR by the picture and follows the console. SDR and HDR say it "
                             "yourself."
                           : "Automatic tells SDR from HDR by the picture and follows the console. A change here "
                             "starts the picture again: the card has to be opened in another mode.");
        if (picture >= 0) {
            save(f, "video", "mode", kPicture[picture]);
            if (f.picture_live) {
                // A 10-bit capture: Glasir shows the same frames as that from now on.
                g_menu.picture = picture;
                f.mpv->command({"script-message", "glasir-picture", kPicture[picture]});
            } else {
                // Glasir starts the picture again in that mode (mpv's exit
                // code 42 asks for that).
                f.mpv->command({"quit", "42"});
            }
        }
        // NVIDIA RTX Video HDR, where the GPU has it: the screen stays in HDR
        // and an SDR signal is turned into HDR.
        bool rtx = (g_menu.rtx_hdr >= 0 ? g_menu.rtx_hdr : f.rtx_hdr) == 1;
        const bool rtx_offered = f.rtx_hdr >= 0 && f.picture_live && picture_mode(f) == 2;
        if (rtx_offered &&
            ui::toggle("RTX HDR for SDR", &rtx,
                       "When the console sends SDR, NVIDIA RTX Video HDR turns it into HDR and the screen stays in "
                       "HDR. Glasir tells SDR from HDR by the picture. Off: everything is shown as HDR.")) {
            g_menu.rtx_hdr = rtx ? 1 : 0;
            save(f, "video", "rtx_hdr", rtx ? "1" : "0");
            f.mpv->command({"script-message", "glasir-rtx-hdr", rtx ? "on" : "off"});
        }
        if (picture_mode(f) == 0)
            ui::note(hdr ? "The console sends HDR." : "The console sends SDR.");
        else if (rtx_offered && rtx && !hdr)
            ui::note(mpv.rtx_hdr == "unavailable"
                         ? "The console sends SDR. RTX Video HDR could not start here (it needs HDR on in Windows)."
                         : "The console sends SDR: shown in HDR by RTX Video HDR.");
    }
}

}  // namespace

void apply_style(float scale) { ui::apply_style(scale, 0.92f); }

void draw_toast(const std::string &text, float alpha, float scale) {
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.05f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * scale, 10.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(ui::bold_font(), ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::PushTextWrapPos(io.DisplaySize.x * 0.5f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(4);
}

// The panel at the right of the picture: what is switched while playing
// (DLSS 5, frame generation, the volume) in sight, what is set once behind
// each card's "More", which stays as it was left while Glasir runs.
bool draw_menu(const MenuFrame &f) {
    ImGuiIO &io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    const bool opened = ImGui::GetFrameCount() != g_menu.last_frame + 1;
    g_menu.last_frame = ImGui::GetFrameCount();

    const float width = std::min(io.DisplaySize.x, std::max(io.DisplaySize.x * 0.27f, 400.0f * f.scale));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - width, 0));
    ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y));
    if (opened) ImGui::SetNextWindowFocus();
    bool keep = true;
    ImGui::Begin("Glasir", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    if (opened) ImGui::SetNavCursorVisible(true);  // the first item shows as chosen at once
    const MpvState mpv = f.mpv->state();

    ImGui::PushFont(ui::bold_font(), ImGui::GetStyle().FontSizeBase * 1.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kGold);
    ImGui::TextUnformatted("Glasir");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ui::note(status_line(f, mpv).c_str());

    picture(f, mpv);
    if (mpv.connected) {
        sound(f, mpv, now);
    } else {
        ui::note("Connecting to mpv...");
    }

    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 0.5f));
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Close", ImVec2(half, 0))) keep = false;
    ImGui::SameLine();
    if (ImGui::Button("Quit Glasir", ImVec2(half, 0))) f.mpv->command({"quit"});
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
    ui::note("Tab or Esc closes \xC2\xB7 F6 DLSS 5 \xC2\xB7 F8 frame generation \xC2\xB7 S screenshot \xC2\xB7 F9 bug report");
    ImGui::PopFont();
    ImGui::End();
    return keep;
}

}  // namespace glasir::overlay
