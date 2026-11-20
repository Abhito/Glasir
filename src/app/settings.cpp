// SPDX-License-Identifier: MIT
#include "settings.h"

#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <vector>

#include "../common/ini_edit.h"
#include "log.h"

namespace glasir {
namespace {

std::wstring trim(const std::wstring &s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring lower(std::wstring s) {
    for (auto &c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool parse_bool(const std::wstring &v, bool &out) {
    const std::wstring l = lower(v);
    if (l == L"1" || l == L"yes" || l == L"true" || l == L"on") return out = true, true;
    if (l == L"0" || l == L"no" || l == L"false" || l == L"off") return out = false, true;
    return false;
}

bool parse_int(const std::wstring &v, long lo, long hi, long &out) {
    if (v.empty()) return false;
    wchar_t *end = nullptr;
    const long n = wcstol(v.c_str(), &end, 10);
    if (*end || n < lo || n > hi) return false;
    out = n;
    return true;
}

bool parse_double(const std::wstring &v, double lo, double hi, double &out) {
    if (v.empty()) return false;
    wchar_t *end = nullptr;
    const double n = wcstod(v.c_str(), &end);
    if (*end || n < lo || n > hi) return false;
    out = n;
    return true;
}

// false: bad value (error set). Unknown keys are logged and accepted.
bool apply(Settings &s, const std::wstring &section, const std::wstring &key, const std::wstring &value,
           std::wstring &error) {
    const std::wstring name = section + L"." + key;
    auto bad = [&](const wchar_t *expected) {
        error = L"glasir.ini: " + name + L" = " + value + L" (expected " + expected + L")";
        return false;
    };
    long n = 0;
    double d = 0;
    bool b = false;
    if (name == L"capture.device") {
        s.device = value;
    } else if (name == L"capture.width") {
        if (!parse_int(value, 0, 16384, n) || n % 2) return bad(L"0 or an even number of pixels");
        s.width = UINT32(n);
    } else if (name == L"capture.height") {
        if (!parse_int(value, 0, 16384, n) || n % 2) return bad(L"0 or an even number of pixels");
        s.height = UINT32(n);
    } else if (name == L"capture.fps") {
        if (!parse_double(value, 0, 1000, d)) return bad(L"0 or frames per second");
        s.fps = d;
    } else if (name == L"capture.path") {
        const std::wstring l = lower(value);
        if (l == L"auto") s.path = Settings::CapturePath::Auto;
        else if (l == L"mediafoundation") s.path = Settings::CapturePath::MediaFoundation;
        else if (l == L"directshow") s.path = Settings::CapturePath::DirectShow;
        else return bad(L"auto, mediafoundation or directshow");
    } else if (name == L"video.mode") {
        const std::wstring l = lower(value);
        if (l == L"sdr") s.hdr = false, s.hdr_auto = false;
        else if (l == L"hdr") s.hdr = true, s.hdr_auto = false;
        else if (l == L"auto") s.hdr_auto = true, s.hdr = false;
        else return bad(L"auto, sdr or hdr");
    } else if (name == L"video.auto_found") {
        const std::wstring l = lower(value);
        if (l == L"sdr") s.auto_found_hdr = false;
        else if (l == L"hdr") s.auto_found_hdr = true;
        else return bad(L"sdr or hdr");
    } else if (name == L"video.rtx_hdr") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.rtx_hdr = b;
    } else if (name == L"video.sdr_range") {
        const std::wstring l = lower(value);
        if (l == L"limited") s.sdr_full_range = false;
        else if (l == L"full") s.sdr_full_range = true;
        else return bad(L"limited or full");
    } else if (name == L"video.scaling") {
        const std::wstring l = lower(value);
        if (l == L"mpv") s.scaling = Settings::Scaling::Mpv;
        else if (l == L"fsr") s.scaling = Settings::Scaling::Fsr;
        else if (l == L"vsr") s.scaling = Settings::Scaling::Vsr;
        else if (l == L"cnn") s.scaling = Settings::Scaling::Cnn;
        else if (l == L"anime4k") s.scaling = Settings::Scaling::Anime4k;
        else if (l == L"nvidia") s.scaling = Settings::Scaling::Nvidia;
        else return bad(L"mpv, fsr, vsr, cnn, anime4k or nvidia");
    } else if (name == L"video.scaling_quality") {
        const std::wstring l = lower(value);
        if (l == L"low") s.scaling_quality = 0;
        else if (l == L"medium") s.scaling_quality = 1;
        else if (l == L"high") s.scaling_quality = 2;
        else if (l == L"ultra") s.scaling_quality = 3;
        else return bad(L"low, medium, high or ultra");
    } else if (name == L"video.scale_to") {
        std::wstring l = lower(value);
        if (l == L"screen") {
            s.scale_to = 0;
        } else {
            if (!l.empty() && l.back() == L'p') l.pop_back();  // 1440p
            if (!parse_int(l, 480, 4320, n) || n % 2) return bad(L"screen, or a height in lines: 480 to 4320, even");
            s.scale_to = int(n);
        }
    } else if (name == L"video.low_latency") {
        const std::wstring l = lower(value);
        if (l == L"off" || l == L"0") s.low_latency = 0;
        else if (l == L"on" || l == L"1") s.low_latency = 1;
        else if (l == L"boost") s.low_latency = 2;
        else return bad(L"off, on or boost");
    } else if (name == L"video.hdr_peak_nits") {
        if (!parse_int(value, 80, 10000, n)) return bad(L"80 to 10000");
        s.hdr_peak_nits = int(n);
    } else if (name == L"frames.dedup") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.dedup = b;
    } else if (name == L"frames.dedup_threshold") {
        if (!parse_int(value, 0, 1023, n)) return bad(L"0 to 1023");
        s.dedup_threshold = int(n);
    } else if (name == L"frames.dedup_keepalive_ms") {
        if (!parse_int(value, 0, 60000, n)) return bad(L"0 to 60000");
        s.dedup_keepalive_ms = DWORD(n);
    } else if (name == L"framegen.enabled") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.framegen = b;
    } else if (name == L"framegen.quality") {
        const std::wstring l = lower(value);
        if (l == L"high") s.framegen_fast = false;
        else if (l == L"fast") s.framegen_fast = true;
        else return bad(L"high or fast");
    } else if (name == L"framegen.multiplier") {
        if (!parse_int(value, 2, 4, n)) return bad(L"2, 3 or 4");
        s.framegen_multiplier = int(n);
    } else if (name == L"framegen.method") {
        const std::wstring l = lower(value);
        if (l == L"glasir") s.framegen_nvidia = false;
        else if (l == L"nvidia") s.framegen_nvidia = true;
        else return bad(L"glasir or nvidia");
    } else if (name == L"framegen.nvidia_quality") {
        const std::wstring l = lower(value);
        if (l == L"low") s.nvidia_quality = 0;
        else if (l == L"medium") s.nvidia_quality = 1;
        else if (l == L"high") s.nvidia_quality = 2;
        else if (l == L"auto") s.nvidia_quality = 3;
        else return bad(L"low, medium, high or auto");
    } else if (name == L"framegen.all_frames_when_off") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.all_frames_when_off = b;
    } else if (name == L"dlss5.enabled") {
        const std::wstring l = lower(value);
        if (l == L"auto") s.dlss5 = Settings::Dlss5::Auto;
        else if (l == L"on" || l == L"1") s.dlss5 = Settings::Dlss5::On;
        else if (l == L"off" || l == L"0") s.dlss5 = Settings::Dlss5::Off;
        else return bad(L"auto, on or off");
    } else if (name == L"audio.enabled") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.audio = b;
    } else if (name == L"audio.device") {
        s.audio_device = value;
    } else if (name == L"audio.volume") {
        if (!parse_int(value, 0, 100, n)) return bad(L"0 to 100");
        s.volume = int(n);
    } else if (name == L"audio.boost") {
        if (!parse_int(value, 100, 400, n)) return bad(L"100 to 400");
        s.boost = int(n);
    } else if (name == L"audio.delay_ms") {
        if (!parse_int(value, 0, 1000, n)) return bad(L"0 to 1000");
        s.audio_delay_ms = int(n);
    } else if (name == L"audio.period") {
        const std::wstring l = lower(value);
        if (l == L"minimum") s.audio_min_period = true;
        else if (l == L"default") s.audio_min_period = false;
        else return bad(L"minimum or default");
    } else if (name == L"window.mode") {
        const std::wstring l = lower(value);
        if (l == L"fullscreen") s.borderless = s.fullscreen = true;
        else if (l == L"borderless") s.borderless = true, s.fullscreen = false;
        else if (l == L"windowed") s.borderless = s.fullscreen = false;
        else return bad(L"fullscreen, borderless or windowed");
    } else if (name == L"window.skip_black_bars") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.skip_black_bars = b;
    } else if (name == L"launcher.show") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.show_launcher = b;
    } else if (name == L"update.check") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.update_check = b;
    } else if (name == L"debug.mpv_log") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.mpv_log = b;
    } else {
        log(L"glasir.ini: unknown setting {}, skipped", name);
    }
    return true;
}

}  // namespace

const wchar_t *scaling_name(Settings::Scaling scaling) {
    switch (scaling) {
    case Settings::Scaling::Fsr: return L"fsr";
    case Settings::Scaling::Vsr: return L"vsr";
    case Settings::Scaling::Cnn: return L"cnn";
    case Settings::Scaling::Anime4k: return L"anime4k";
    case Settings::Scaling::Nvidia: return L"nvidia";
    default: return L"mpv";
    }
}

const wchar_t *nvidia_quality_name(int quality) {
    return quality <= 0 ? L"low" : quality == 2 ? L"high" : quality >= 3 ? L"auto" : L"medium";
}

const wchar_t *scaling_quality_name(int quality) {
    return quality <= 0 ? L"low" : quality == 2 ? L"high" : quality >= 3 ? L"ultra" : L"medium";
}

std::wstring scale_to_name(int scale_to) { return scale_to > 0 ? std::to_wstring(scale_to) : L"screen"; }

const wchar_t *low_latency_name(int low_latency) {
    return low_latency >= 2 ? L"boost" : low_latency == 1 ? L"on" : L"off";
}

const char *default_settings_text() {
    return R"(; Glasir settings. Lines starting with ; are comments.
; A missing file is recreated with these defaults.

[capture]
; The card's name as "Glasir.exe --list" shows it. Empty: pick automatically.
device =
; Capture size and rate. 0: automatic (60 fps if offered, the largest size
; that fits the display).
width = 0
height = 0
fps = 0
; auto: Media Foundation if the card offers a usable mode, else DirectShow.
; DirectShow works with more cards but has no dedup or frame generation.
path = auto

[video]
; auto, sdr or hdr. auto: Glasir captures in the card's 10-bit (P010) mode,
; tells from the picture whether the console sends SDR or HDR, and follows it
; when that changes (while playing, within a second or so); a card without a
; 10-bit mode is SDR. sdr and hdr say it yourself: hdr needs a 10-bit mode
; and HDR on the console, sdr also offers the card's 8-bit modes (often
; larger or faster).
mode = auto
; What the signal was the last time Glasir watched it (Glasir writes this):
; how it starts.
auto_found = hdr
; mode = hdr on an NVIDIA RTX GPU. 0: everything is shown as HDR, whatever
; the picture looks like. 1: the screen stays in HDR, and when the console
; sends SDR, NVIDIA RTX Video HDR turns it into HDR (Windows' HDR must be
; on). Glasir then tells SDR from HDR by the picture, as mode = auto does.
rtx_hdr = 0
; The SDR signal's range: limited (most consoles) or full.
sdr_range = limited
; The brightest the console is taken to send, in nits. HDR is shown as it
; comes, up to what the display can show; on a display dimmer than this value
; the picture is tone-mapped down from it instead.
hdr_peak_nits = 1000
; How a capture smaller than the window is scaled up (before DLSS 5):
; mpv (its own Lanczos), fsr (AMD FSR 1: sharper, any GPU), vsr (the GPU's
; video super resolution: NVIDIA RTX or Intel; FSR 1 where it cannot run),
; cnn (ArtCNN, a small neural network: the most detail, any GPU, more GPU
; time) or anime4k (Anime4K: for cel-shaded and drawn styles, any GPU, more
; GPU time). NVIDIA's vsr has quality levels (1 to 4, or Auto): they are the
; driver's, set in the NVIDIA app (System, Video, Super resolution) for every
; program; there is no key for them here. G-SYNC does not run while NVIDIA's
; vsr does (the driver turns it off): pick another scaling to have G-SYNC.
; nvidia is NVIDIA's Video Super Resolution again, but run by Glasir itself
; (an NVIDIA RTX GPU, ReShade, and NVIDIA's download, which the window
; offers): its models for clean pictures such as a console's, a choice among
; them (scaling_quality), and G-SYNC stays. FSR 1 where it cannot run.
scaling = mpv
; scaling = nvidia: low, medium, high or ultra, NVIDIA's quality levels. A
; higher one is a larger model and takes more GPU time, which DLSS 5 and
; frame generation also need. The menu (Tab) switches it while playing.
scaling_quality = medium
; The size the scaler makes the picture: screen (the picture's place on the
; screen), or a height in lines whatever the screen's: 1440, 2160 (4K), 2880
; (5K). Larger than the screen, the picture is scaled up to that size and
; then down to the screen (supersampling): smoother edges and finer detail,
; for more GPU time. Smaller, a plain scaler does the rest of the way up.
; Not used by scaling = mpv, which has one step only.
scale_to = screen
; NVIDIA GPUs: NVIDIA Reflex's low latency mode for the picture. off, on, or
; boost (on, and the GPU is kept at full clock speed: more power, more heat).
; With frame generation on a GPU that has time to spare, frames reach the
; screen a few milliseconds sooner and more evenly. Where DLSS 5 keeps the
; GPU busy, or without frame generation, it changes little. Nothing on other
; GPUs. The menu (Tab) switches it while playing.
low_latency = off

[frames]
; 1: send mpv only new frames (a 30 fps game captured at 60 arrives at 30).
; Frame generation needs it.
dedup = 1
; How much a 16x16 block's average brightness must change (10-bit steps) to
; count as a new frame. Raise it for noisy sources.
dedup_threshold = 3
; A still picture is still sent this often (ms), so menus keep drawing.
dedup_keepalive_ms = 200

[framegen]
; 1: frame generation doubles the frame rate (30 -> 60, 60 -> 120) when the
; display can show it. Needs dedup = 1 and ReShade. F8 toggles it while playing;
; the menu (Tab) switches it and keeps the choice here.
enabled = 1
; How many frames are shown for each of the game's: 2 (30 fps looks like 60),
; 3 or 4. When the display cannot show that many, or the GPU cannot finish
; them in time, the highest that holds steady is used (the menu says so).
; 3 and 4 are provided as is and are not recommended: every generated
; frame takes GPU time and adds delay, and more of what is shown is made up.
; They are there for players who want them.
multiplier = 2
; Who makes the frames in between: glasir (Glasir's own, on any GPU) or
; nvidia (NVIDIA Video Frame Generation, on an RTX 40 or 50 series GPU: the
; window offers its download; Glasir's own makes them where it cannot run).
method = glasir
; Glasir's own: high, or fast (about half the GPU time, a little less clean).
quality = high
; NVIDIA's: low, medium or high. A larger model is cleaner and takes more GPU
; time; high is for 30 fps games with DLSS 5 off (with it on, medium).
; auto: Glasir picks, the largest that keeps up with the game, and picks
; again when the load changes (a 30 fps game after a 60 fps one, DLSS 5
; switched on). The first change to a model stops the picture for a fraction
; of a second while it loads; all three then stay in video memory.
nvidia_quality = medium
; While frame generation is off (F8): 1 = send mpv every frame (lowest
; latency), 0 = new frames only.
all_frames_when_off = 1

[dlss5]
; DLSS 5 neural rendering (NVIDIA RTX 30, 40 and 50 series). auto: on with
; one of those GPUs, off otherwise. on: always try (e.g. an RTX 20). off:
; starts off; on an RTX 30, 40 or 50 the menu (Tab) can switch it on. F6
; toggles it while playing.
enabled = auto

[audio]
; 1: play the card's sound (low latency, through mpv). 0: no sound.
enabled = 1
; The audio input to use. Empty: the card's own (found automatically).
device =
; Volume 0-100 and boost 100-400 (%). Keys: Up/Down volume, M mute,
; Ctrl+Up/Down boost. What you change while playing (keys or the menu) is
; kept here.
volume = 100
boost = 100
; Holds the sound back (ms) so it matches the picture; DLSS 5 and frame
; generation make the picture a little later. Keys: [ and ].
delay_ms = 0
; minimum: lowest latency. default: steadier if the sound crackles.
period = minimum

[window]
; fullscreen (mpv's own full screen, as f or a double-click switches it),
; borderless (a window without a frame that fills the screen) or windowed.
; The menu (Tab) switches it and keeps the choice here.
mode = borderless
; Full screen on a screen of another shape than the picture (16:9 on an
; ultrawide). 0: mpv covers the screen, black bars included. 1: mpv's window
; takes the picture only, black around it, so DLSS 5 and frame generation skip
; the bars. Some PCs then compose the window (no overlay plane for it):
; G-SYNC cannot follow it and frame generation falls short of double. The
; menu says when (its fps, and a line under the picture's More).
skip_black_bars = 0

[launcher]
; 1: Glasir.exe opens its window first (card, picture, enhancements, sound).
; 0: it plays at once; hold Shift while starting it to see the window.
show = 1

[update]
; 1: the window asks GitHub (github.com/Abhito/Glasir) whether a newer Glasir
; is out, and offers to install it. Nothing is installed unasked.
check = 1

[debug]
; 1: mpv writes a detailed log to logs\mpv.log.
mpv_log = 0
)";
}

bool load_settings(const std::wstring &path, Settings &s, std::wstring &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::ofstream out(path, std::ios::binary);
        if (out) {
            const std::string text = default_settings_text();
            out << text;
            log(L"wrote default settings to {}", path);
        } else {
            log(L"could not write {}; using defaults", path);
        }
        return true;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
    std::wistringstream lines(widen(bytes));
    std::wstring line, section;
    for (int number = 1; std::getline(lines, line); ++number) {
        line = trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[') {
            if (line.back() != L']') {
                error = L"glasir.ini line " + std::to_wstring(number) + L": " + line;
                return false;
            }
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            error = L"glasir.ini line " + std::to_wstring(number) + L": " + line + L" (expected key = value)";
            return false;
        }
        std::wstring value = line.substr(eq + 1);
        // A comment after a value needs a space before it: "fps = 60 ; note".
        for (size_t i = 1; i < value.size(); ++i)
            if ((value[i] == L';' || value[i] == L'#') && iswspace(value[i - 1])) {
                value.resize(i);
                break;
            }
        if (!apply(s, section, lower(trim(line.substr(0, eq))), trim(value), error)) return false;
    }
    return true;
}

namespace {
std::vector<IniValue> to_ini(const std::vector<SettingValue> &values) {
    std::vector<IniValue> out;
    for (const auto &v : values) out.push_back({narrow(v.section), narrow(v.key), narrow(v.value)});
    return out;
}
}  // namespace

std::string update_settings_text(const std::string &text, const std::vector<SettingValue> &values) {
    return ini_set_values(text, to_ini(values));
}

bool save_settings(const std::wstring &path, const std::vector<SettingValue> &values) {
    if (ini_save_values(path, to_ini(values))) return true;
    log(L"could not write {}", path);
    return false;
}

bool apply_override(Settings &s, const std::wstring &assignment, std::wstring &error) {
    const size_t dot = assignment.find(L'.');
    const size_t eq = assignment.find(L'=');
    if (dot == std::wstring::npos || eq == std::wstring::npos || dot > eq) {
        error = L"--set " + assignment + L" (expected section.key=value)";
        return false;
    }
    return apply(s, lower(trim(assignment.substr(0, dot))), lower(trim(assignment.substr(dot + 1, eq - dot - 1))),
                 trim(assignment.substr(eq + 1)), error);
}

}  // namespace glasir
