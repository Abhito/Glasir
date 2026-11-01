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
    bool hdr = false;
    bool sdr_full_range = false;
    int hdr_peak_nits = 1000;
    // [frames]
    bool dedup = true;
    int dedup_threshold = 3;
    DWORD dedup_keepalive_ms = 200;
    // [framegen]
    bool framegen = true;
    bool framegen_fast = false;       // quality = fast: about half the GPU time
    bool all_frames_when_off = true;  // frame generation off: every frame (lowest latency)
    // [dlss5]
    enum class Dlss5 { Auto, On, Off };
    Dlss5 dlss5 = Dlss5::Auto;  // auto: on with an RTX 50 series GPU
    // [audio]
    bool audio = true;
    std::wstring audio_device;  // empty: the card's own audio input
    int volume = 100;           // 0-100 %
    int boost = 100;            // 100-400 %
    int audio_delay_ms = 0;     // 0-1000
    bool audio_min_period = true;
    // [window]
    bool borderless = true;
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
