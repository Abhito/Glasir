// SPDX-License-Identifier: MIT
// glasir.ini: the one settings file. The app writes everything else (mpv's
// options, later ReShade's) from it.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace glasir {

struct Settings {
    enum class CapturePath { Auto, MediaFoundation, DirectShow };

    // [capture]
    std::wstring device;  // empty: pick automatically
    UINT32 width = 0, height = 0;
    double fps = 0;
    CapturePath path = CapturePath::Auto;
    // [video]
    // mode = auto: a 10-bit capture, shown as what the signal turns out to
    // be (signal_kind.h). hdr is then what it is shown as: what was found
    // last time (auto_found) until the picture says otherwise.
    bool hdr_auto = true;
    bool hdr = false;
    bool auto_found_hdr = true;
    // mode = hdr on an NVIDIA RTX GPU: an SDR signal is shown in HDR by RTX
    // Video HDR (the signal is watched there too). Off, the default: all of
    // it is HDR - mode = hdr is what the docs tell people to choose when
    // Automatic takes their console's HDR for SDR, and watching the signal
    // would bring that back.
    bool rtx_hdr = false;
    bool sdr_full_range = false;
    int hdr_peak_nits = 1000;
    // How the capture is scaled up to the window (glasir_scaling.lua).
    enum class Scaling { Mpv, Fsr, Vsr, Cnn, Anime4k, Nvidia };
    Scaling scaling = Scaling::Mpv;
    // scaling = nvidia: the model of NVIDIA's Video Super Resolution, 0 low,
    // 1 medium, 2 high, 3 ultra (the add-on's SrQuality).
    int scaling_quality = 1;
    // scale_to: the height in lines the scaler makes the picture, whatever
    // the screen's; mpv's own scaler takes it from there to the window (down
    // from a larger one: supersampling). 0 = screen: the picture's place in
    // the window.
    int scale_to = 0;
    // low_latency: NVIDIA Reflex's low latency mode in mpv (the frame
    // generation add-on asks the driver for it): 0 off, 1 on, 2 on with boost
    // (the GPU kept at full clock speed). Nothing on another GPU.
    int low_latency = 0;
    // [frames]
    bool dedup = true;
    int dedup_threshold = 3;
    DWORD dedup_keepalive_ms = 200;
    // [framegen]
    bool framegen = true;
    bool framegen_fast = false;       // quality = fast: about half the GPU time
    // method = nvidia: NVIDIA Video Frame Generation makes the frames in
    // between (an RTX 40 or 50, once it is downloaded); Glasir's own where it
    // cannot run.
    bool framegen_nvidia = false;
    // nvidia_quality: 0 low, 1 medium, 2 high (its model's size), 3 auto (the
    // largest that keeps up, chosen again as the load changes).
    int nvidia_quality = 1;
    int framegen_multiplier = 2;      // multiplier: 2, 3 or 4 frames shown for each of the game's
    bool all_frames_when_off = true;  // frame generation off: every frame (lowest latency)
    // [dlss5]
    enum class Dlss5 { Auto, On, Off };
    Dlss5 dlss5 = Dlss5::Auto;  // auto: on with an RTX 30, 40 or 50 series GPU
    // [audio]
    bool audio = true;
    std::wstring audio_device;  // empty: the card's own audio input
    int volume = 100;           // 0-100 %
    int boost = 100;            // 100-400 %
    int audio_delay_ms = 0;     // 0-1000
    bool audio_min_period = true;
    // [window]
    bool borderless = true;
    // window.mode = fullscreen: mpv's own full screen (f, double-click);
    // borderless is then true too, where leaving it goes.
    bool fullscreen = false;
    bool skip_black_bars = false;  // full screen: mpv's window takes the picture only (backdrop.h)
    // [launcher]
    bool show_launcher = true;  // Glasir.exe opens its window first
    // [update]
    bool update_check = true;  // the launcher looks for a newer Glasir
    // [debug]
    bool mpv_log = false;
};

// Reads the file; a missing file is created with the defaults. Unknown keys
// are logged and skipped. False with a message for a value that makes no sense.
bool load_settings(const std::wstring &path, Settings &s, std::wstring &error);

// "mpv", "fsr", "vsr", "cnn", "anime4k", "nvidia": glasir.ini's and glasir_scaling.lua's names.
const wchar_t *scaling_name(Settings::Scaling scaling);
// "low", "medium", "high", "auto": glasir.ini's framegen.nvidia_quality and the add-on's VfgQuality.
const wchar_t *nvidia_quality_name(int quality);
// "low", "medium", "high", "ultra": glasir.ini's video.scaling_quality and the add-on's SrQuality.
const wchar_t *scaling_quality_name(int quality);
// "screen" or the lines: glasir.ini's video.scale_to.
std::wstring scale_to_name(int scale_to);
// "off", "on", "boost": glasir.ini's video.low_latency and the add-on's LowLatency.
const wchar_t *low_latency_name(int low_latency);

// One setting, as "section.key=value" (the command line's --set).
bool apply_override(Settings &s, const std::wstring &assignment, std::wstring &error);

// The file written when there is none: every key at its default, explained.
const char *default_settings_text();

struct SettingValue {
    std::wstring section, key, value;
};
// The text with these values set: an existing key keeps its line (spacing,
// a comment after it), a missing one is added at the end of its section (the
// section at the end of the file if there is none). Everything else stays.
std::string update_settings_text(const std::string &text, const std::vector<SettingValue> &values);
// update_settings_text on the file, which is replaced as a whole.
bool save_settings(const std::wstring &path, const std::vector<SettingValue> &values);

}  // namespace glasir
