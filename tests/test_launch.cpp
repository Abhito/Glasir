// SPDX-License-Identifier: MIT
// Checks picking the card's mode, settings parsing, and mpv's command line.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bug_report.h"
#include "backdrop.h"
#include "components.h"
#include "devices.h"
#include "glasir_version.h"
#include "gpu.h"
#include "json.h"
#include "log.h"
#include "mpv_launch.h"
#include "reshade_config.h"
#include "settings.h"
#include "update.h"

using namespace glasir;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

VideoMode mode(const wchar_t *subtype, UINT32 w, UINT32 h, double fps) {
    VideoMode m;
    m.subtype = subtype;
    m.width = w;
    m.height = h;
    m.fps = fps;
    for (PixelFormat c : {PixelFormat::NV12, PixelFormat::P010, PixelFormat::YUY2, PixelFormat::UYVY})
        if (m.subtype == format_name(c)) {
            m.format = c;
            m.supported = true;
        }
    return m;
}

bool is(const std::optional<VideoMode> &m, const wchar_t *subtype, UINT32 w, UINT32 h, double fps) {
    return m && m->subtype == subtype && m->width == w && m->height == h && std::abs(m->fps - fps) < 0.01;
}

bool has(const std::vector<std::wstring> &args, const std::wstring &arg) {
    for (const auto &a : args)
        if (a == arg) return true;
    return false;
}

}  // namespace

int main() {
    // A card like the Live Gamer Ultra 2.1: NV12 and P010 up to 4K, 120 at 1440p.
    const std::vector<VideoMode> card = {
        mode(L"NV12", 3840, 2160, 60), mode(L"NV12", 2560, 1440, 60), mode(L"NV12", 2560, 1440, 120),
        mode(L"NV12", 1920, 1080, 60), mode(L"P010", 3840, 2160, 60), mode(L"P010", 2560, 1440, 60),
        mode(L"MJPG", 3840, 2160, 60), mode(L"YUY2", 2560, 1440, 60), mode(L"NV12", 3440, 1440, 60),
    };
    {
        ModeRequest r;
        r.max_auto_height = 1440;
        expect(is(choose_mode(card, r), L"NV12", 2560, 1440, 60),
               "auto SDR, 1440p display: NV12 1440p60, not the 21:9 3440x1440");
        r.max_auto_height = 2160;
        expect(is(choose_mode(card, r), L"NV12", 3840, 2160, 60), "auto SDR on a 4K display: NV12 4K60");
        r.max_auto_height = 1080;
        expect(is(choose_mode(card, r), L"NV12", 1920, 1080, 60), "auto SDR on a 1080p display: 1080p60");
        r.max_auto_height = 720;
        expect(is(choose_mode(card, r), L"NV12", 1920, 1080, 60), "nothing fits a 720p display: the smallest");
        r.max_auto_height = 1440;
        r.hdr = true;
        expect(is(choose_mode(card, r), L"P010", 2560, 1440, 60), "auto HDR: P010 1440p60");
        r.hdr = false;
        r.fps = 120;
        expect(is(choose_mode(card, r), L"NV12", 2560, 1440, 120), "fps 120: the size that has 120");
        r.fps = 0;
        r.width = 1920;
        r.height = 1080;
        expect(is(choose_mode(card, r), L"NV12", 1920, 1080, 60), "fixed size 1080p: NV12 1080p60");
        r.width = 1280;
        r.height = 720;
        expect(!choose_mode(card, r), "a size the card lacks: none");
        r.width = r.height = 0;
        r.hdr = true;
        r.fps = 120;
        expect(is(choose_mode(card, r), L"P010", 2560, 1440, 60), "HDR 120 not offered: the nearest rate");
    }
    {
        // A cheap USB card: YUY2 1080p60, MJPEG 4K30.
        const std::vector<VideoMode> cheap = {mode(L"MJPG", 3840, 2160, 30), mode(L"YUY2", 1920, 1080, 60),
                                              mode(L"YUY2", 1920, 1080, 30)};
        ModeRequest r;
        r.max_auto_height = 2160;
        expect(is(choose_mode(cheap, r), L"YUY2", 1920, 1080, 60), "cheap card: YUY2 1080p60, not MJPEG");
        r.hdr = true;
        expect(!choose_mode(cheap, r), "cheap card: no HDR");
        r.hdr = false;
        const std::vector<VideoMode> mjpeg_only = {mode(L"MJPG", 1920, 1080, 60)};
        expect(!choose_mode(mjpeg_only, r), "MJPEG only: nothing for Media Foundation");
        r.supported_only = false;
        expect(is(choose_mode(mjpeg_only, r), L"MJPG", 1920, 1080, 60), "MJPEG only: DirectShow can use it");
    }
    {
        CaptureDevice webcam{L"HD Webcam", L"a", {mode(L"NV12", 1920, 1080, 30)}, true};
        CaptureDevice card_dev{L"Live Gamer Ultra 2.1-Video", L"b", card, true};
        CaptureDevice usb{L"USB Video", L"c", {mode(L"YUY2", 1920, 1080, 60)}, true};
        const std::vector<CaptureDevice> devices = {webcam, usb, card_dev};
        ModeRequest r;
        r.max_auto_height = 1440;
        const CaptureDevice *d = choose_device(devices, L"", r);
        expect(d && d->name == L"Live Gamer Ultra 2.1-Video", "auto device: the capture card, not the webcam");
        d = choose_device(devices, L"usb video", r);
        expect(d && d->name == L"USB Video", "named device: case-insensitive");
        d = choose_device(devices, L"Gamer Ultra", r);
        expect(d && d->name == L"Live Gamer Ultra 2.1-Video", "named device: part of the name");
        expect(choose_device(devices, L"Elgato", r) == nullptr, "named device missing: none");
        const std::vector<CaptureDevice> only_webcam = {webcam};
        d = choose_device(only_webcam, L"", r);
        expect(d && d->name == L"HD Webcam", "auto device: a webcam if that is all there is");
    }
    {
        Settings s;
        std::wstring error;
        expect(apply_override(s, L"video.mode=hdr", error) && s.hdr, "--set video.mode=hdr");
        expect(apply_override(s, L"capture.fps = 120", error) && s.fps == 120, "--set with spaces");
        expect(apply_override(s, L"Capture.Device=My Card", error) && s.device == L"My Card",
               "--set keeps the value's case");
        expect(!apply_override(s, L"capture.width=1281", error), "odd width rejected");
        expect(!apply_override(s, L"video.mode=dolby", error), "unknown mode rejected");
        expect(apply_override(s, L"future.thing=1", error), "unknown key skipped, not an error");

        wchar_t dir[MAX_PATH];
        GetTempPathW(MAX_PATH, dir);
        const std::wstring path = std::wstring(dir) + L"glasir_test_settings.ini";
        DeleteFileW(path.c_str());
        Settings fresh;
        expect(load_settings(path, fresh, error), "missing settings file: defaults");
        Settings reread;
        reread.hdr = true;              // must be reset by the file's mode = sdr
        reread.skip_black_bars = true;  // and by skip_black_bars = 0
        expect(load_settings(path, reread, error) && !reread.hdr && reread.dedup && reread.borderless &&
                   !reread.skip_black_bars,
               "the written defaults read back");
        expect(apply_override(reread, L"window.skip_black_bars=1", error) && reread.skip_black_bars,
               "--set window.skip_black_bars=1");
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        const char text[] = "\xEF\xBB\xBF[capture]\r\ndevice = Live Gamer ; my card\r\nfps=60\r\n[video]\r\nmode = HDR\r\n";
        DWORD written = 0;
        WriteFile(f, text, DWORD(sizeof(text) - 1), &written, nullptr);
        CloseHandle(f);
        Settings edited;
        expect(load_settings(path, edited, error) && edited.device == L"Live Gamer" && edited.fps == 60 && edited.hdr,
               "BOM, CRLF, inline comment, upper-case value");
        DeleteFileW(path.c_str());
    }
    {
        // Arguments survive quoting exactly (paths with spaces, quotes, trailing backslashes).
        const std::vector<std::wstring> tricky = {L"--config-dir=C:\\Program Files\\Glasir\\engine\\",
                                                  L"plain", L"say \"hi\"", L"C:\\a b\\\\", L"", L"--geometry=100%x100%"};
        const std::wstring line = join_command_line(L"C:\\My Apps\\mpv.exe", tricky);
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(line.c_str(), &argc);
        bool same = argc == int(tricky.size()) + 1 && std::wstring(argv[0]) == L"C:\\My Apps\\mpv.exe";
        for (size_t i = 0; same && i < tricky.size(); ++i) same = tricky[i] == argv[i + 1];
        LocalFree(argv);
        expect(same, "command line quoting round-trips");

        expect(ipc_command({L"loadfile", L"av://dshow:video=Card \"2\"", L"replace"}) ==
                   "{\"command\":[\"loadfile\",\"av://dshow:video=Card \\\"2\\\"\",\"replace\"]}\n",
               "IPC command JSON escaping");

        Settings s;
        MpvPlan plan;
        plan.raw = true;
        plan.layout = {PixelFormat::NV12, 2560, 1440};
        plan.fps = 60;
        auto args = mpv_arguments(plan, s);
        expect(has(args, L"--demuxer-rawvideo-mp-format=nv12") && has(args, L"--demuxer-rawvideo-fps=60") &&
                   has(args, L"--vf=format=colormatrix=bt.709:colorlevels=limited:primaries=bt.709:chroma-location=uhd"),
               "SDR NV12: raw format, rate and BT.709 tags");
        plan.layout.format = PixelFormat::YUY2;
        args = mpv_arguments(plan, s);
        expect(has(args, L"--demuxer-rawvideo-mp-format=yuyv422") &&
                   has(args, L"--vf=format=colormatrix=bt.709:colorlevels=limited:primaries=bt.709:"
                             L"chroma-location=mpeg2/4/h264"),
               "SDR YUY2: yuyv422, chroma sited left");
        s.hdr = true;
        plan.layout.format = PixelFormat::P010;
        plan.fps = 59.94;
        args = mpv_arguments(plan, s);
        expect(has(args, L"--demuxer-rawvideo-mp-format=p010") && has(args, L"--demuxer-rawvideo-fps=59.94") &&
                   has(args, L"--d3d11-output-csp=pq") && has(args, L"--target-colorspace-hint=yes"),
               "HDR P010: PQ output");
        s.hdr = false;
        plan.raw = false;
        plan.dshow_device = L"USB Video";
        plan.dshow_mode = mode(L"MJPG", 1920, 1080, 60);
        args = mpv_arguments(plan, s);
        expect(mpv_load_target(plan) == L"av://dshow:video=USB Video" &&
                   has(args, L"--demuxer-lavf-o-append=vcodec=mjpeg") &&
                   has(args, L"--demuxer-lavf-o-append=video_size=1920x1080") && !has(args, L"--demuxer=rawvideo"),
               "DirectShow fallback: URL, size, MJPEG");
        plan.dshow_mode.reset();
        s.width = 1920;
        s.height = 1080;
        s.fps = 60;
        args = mpv_arguments(plan, s);
        expect(has(args, L"--demuxer-lavf-o-append=video_size=1920x1080") &&
                   has(args, L"--demuxer-lavf-o-append=framerate=60") && has(args, L"--container-fps-override=60"),
               "DirectShow without a known mode: glasir.ini's size and rate");
        s.width = s.height = 0;
        s.fps = 0;
        args = mpv_arguments(plan, s);
        bool sized = false;
        for (const auto &a : args) sized = sized || a.find(L"video_size") != std::wstring::npos;
        expect(!sized, "DirectShow, nothing known or asked: the driver's default");
    }
    {
        // ReShade.ini, ReShadePreset.ini, dlss5-feed.cfg in a scratch engine folder.
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring engine = std::wstring(tmp) + L"glasir_test_engine";
        CreateDirectoryW(engine.c_str(), nullptr);
        CreateDirectoryW((engine + L"\\reshade-shaders").c_str(), nullptr);
        CreateDirectoryW((engine + L"\\reshade-shaders\\Shaders").c_str(), nullptr);
        CreateDirectoryW((engine + L"\\reshade-shaders\\Shaders\\DLSS5Feeder").c_str(), nullptr);
        auto touch = [&](const std::wstring &name, const char *text) {
            HANDLE f = CreateFileW((engine + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD n = 0;
            WriteFile(f, text, DWORD(strlen(text)), &n, nullptr);
            CloseHandle(f);
        };
        auto read = [&](const std::wstring &name) {
            std::string out;
            HANDLE f = CreateFileW((engine + name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                                   nullptr);
            char buf[4096];
            DWORD n = 0;
            while (f != INVALID_HANDLE_VALUE && ReadFile(f, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            return out;
        };
        // What ReShade writes itself on a first run, and a user's own effect path.
        touch(L"\\ReShade.ini", "[GENERAL]\r\nEffectSearchPaths=.\\,D:\\MyShaders\\**\r\nTextureSearchPaths=.\\\r\n"
                                "PreprocessorDefinitions=\r\n[OVERLAY]\r\nTutorialProgress=1\r\n");
        DeleteFileW((engine + L"\\ReShadePreset.ini").c_str());
        DeleteFileW((engine + L"\\dlss5-feed.cfg").c_str());
        Settings s;
        expect(!dlss5_installed(engine), "DLSS 5 files missing: not installed");
        for (const wchar_t *f : {L"\\dlss5-feed.addon64", L"\\renodx-dlss5.addon64", L"\\nvngx_dlssnr.dll",
                                 L"\\nvngx_dlss.dll", L"\\reshade-shaders\\Shaders\\DLSS5Feeder\\DLSS5_Feed.fx"})
            touch(f, "x");
        expect(dlss5_installed(engine), "DLSS 5 files there: installed");
        write_reshade_config(engine, s, true, true);
        std::string ini = read(L"\\ReShade.ini");
        expect(ini.find(".\\reshade-shaders\\Shaders\\**,.\\,D:\\MyShaders\\**") != std::string::npos,
               "effect path added in front, the user's kept");
        expect(ini.find("TextureSearchPaths=.\\reshade-shaders\\Textures\\**\r\n") != std::string::npos,
               "texture path replaces ReShade's bare default");
        expect(ini.find("PreprocessorDefinitions=DLSS5_MV_PROVIDER=3") != std::string::npos, "Lumenite motion vectors");
        expect(ini.find("TutorialProgress=1") != std::string::npos, "an existing ReShade value is not overwritten");
        expect(ini.find("NeuralUplift=1") != std::string::npos && ini.find("[GLASIR_FRAMEGEN]") != std::string::npos,
               "DLSS 5 and frame generation keys written");
        expect(read(L"\\ReShadePreset.ini").rfind("Techniques=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed", 0) == 0,
               "preset enables the DLSS 5 effects");
        expect(read(L"\\dlss5-feed.cfg").empty(), "DLSS 5 on: the Feeder's file is left for the Feeder to write");
        touch(L"\\dlss5-feed.cfg", "enabled=1\r\nmode=2\r\n");
        touch(L"\\ReShadePreset.ini", "Techniques=X@x.fx\r\nTechniqueSorting=X@x.fx\r\n\r\n[x.fx]\r\nA=1\r\n");
        write_reshade_config(engine, s, true, false);
        expect(read(L"\\ReShadePreset.ini").rfind("Techniques=\r\n", 0) == 0 &&
                   read(L"\\ReShade.ini").find("NeuralUplift=1") != std::string::npos &&
                   read(L"\\dlss5-feed.cfg") == "enabled=1\r\nmode=2\r\n",
               "DLSS 5 loaded but off: no effects, add-ons ready for the menu");
        write_reshade_config(engine, s, false, false);
        expect(read(L"\\ReShade.ini").find("NeuralUplift=0") != std::string::npos, "DLSS 5 off: NeuralUplift=0");
        expect(read(L"\\dlss5-feed.cfg") == "enabled=0\r\nmode=2\r\n", "DLSS 5 off: Feeder off, its other keys kept");
        const std::string preset = read(L"\\ReShadePreset.ini");
        expect(preset.rfind("Techniques=\r\n", 0) == 0 && preset.find("[x.fx]\r\nA=1") != std::string::npos,
               "DLSS 5 off: no effects, the preset's sections kept");
        for (const wchar_t *f : {L"\\ReShade.ini", L"\\ReShadePreset.ini", L"\\dlss5-feed.cfg", L"\\dlss5-feed.addon64",
                                 L"\\renodx-dlss5.addon64", L"\\nvngx_dlssnr.dll", L"\\nvngx_dlss.dll",
                                 L"\\reshade-shaders\\Shaders\\DLSS5Feeder\\DLSS5_Feed.fx"})
            DeleteFileW((engine + f).c_str());
    }
    {
        // GitHub's release answers.
        const auto j = Json::parse(R"({"tag_name":"v1.2","assets":[{"name":"aé.zip","size":1234,)"
                                   R"("digest":"sha256:ab","x":[1,-2.5e1,true,null,{}]}],"body":"line\nnext \"q\""})");
        expect(j && j->str("tag_name") == "v1.2", "JSON: a string member");
        const Json *assets = j ? j->get("assets") : nullptr;
        expect(assets && assets->items().size() == 1 && assets->items()[0].str("name") == "a\xC3\xA9.zip",
               "JSON: arrays, objects, \\u escapes to UTF-8");
        expect(assets && assets->items()[0].get("size")->number() == 1234, "JSON: numbers");
        expect(assets && assets->items()[0].get("x")->items()[1].number() == -25, "JSON: exponents");
        expect(j && j->str("body") == "line\nnext \"q\"", "JSON: escapes");
        expect(j && j->str("missing").empty() && !j->get("missing"), "JSON: a missing member");
        expect(!Json::parse("{\"a\":1,}") && !Json::parse("[1 2]") && !Json::parse("{\"a\":1} x") &&
                   !Json::parse("\"open"),
               "JSON: broken text is refused");
        expect(Json::parse("\"\\ud83d\\ude00\"")->string() == "\xF0\x9F\x98\x80", "JSON: surrogate pairs");

        int v[3] = {};
        expect(parse_release_tag(L"renodx-dlss5-1.10.2", L"renodx-dlss5", v) && v[0] == 1 && v[1] == 10 && v[2] == 2,
               "release tag PREFIX-x.y.z");
        expect(!parse_release_tag(L"renodx-dlss5-1.10.2-rc1", L"renodx-dlss5", v) &&
                   !parse_release_tag(L"renodx-dlss5-fork-1.0.0", L"renodx-dlss5", v) &&
                   !parse_release_tag(L"dlssnr-1.0.0", L"renodx-dlss5", v),
               "release candidates, forks and other tags are not picked");
    }
    {
        // What changed while playing goes back into glasir.ini, the rest kept.
        const std::string before = "; top\r\n[audio]\r\n; volume\r\nvolume = 100\r\nboost=100 ; a note\r\n"
                                   "device =\r\n\r\n[window]\r\nmode = borderless\r\n";
        const std::string after = update_settings_text(
            before, {{L"audio", L"volume", L"80"}, {L"audio", L"boost", L"150"}, {L"audio", L"device", L"X"},
                     {L"audio", L"delay_ms", L"40"}, {L"debug", L"mpv_log", L"1"}});
        expect(after == "; top\r\n[audio]\r\n; volume\r\nvolume = 80\r\nboost=150 ; a note\r\ndevice = X\r\n"
                        "delay_ms = 40\r\n\r\n[window]\r\nmode = borderless\r\n\r\n[debug]\r\nmpv_log = 1\r\n",
               "settings: values replaced in place, comments kept, missing keys added");
        expect(update_settings_text("[AUDIO]\nVolume=5\n", {{L"audio", L"volume", L"6"}}) == "[AUDIO]\nVolume=6\n",
               "settings: keys and sections match without case, LF kept");
        expect(update_settings_text("[audio]\nvolume = ;x\n", {{L"audio", L"volume", L"6"}}) == "[audio]\nvolume = 6 ;x\n",
               "settings: a comment right after = stays a comment");
        expect(update_settings_text(default_settings_text(), {}) == default_settings_text(),
               "settings: no values, no change");
        Settings s;
        std::wstring error;
        wchar_t temp[MAX_PATH + 1] = {};
        GetTempPathW(MAX_PATH + 1, temp);
        const std::wstring path = std::wstring(temp) + L"glasir_test_settings.ini";
        DeleteFileW(path.c_str());
        load_settings(path, s, error);  // writes the defaults
        expect(save_settings(path, {{L"audio", L"volume", L"35"}, {L"audio", L"delay_ms", L"120"}}),
               "settings: saved");
        Settings again;
        expect(load_settings(path, again, error) && again.volume == 35 && again.audio_delay_ms == 120 &&
                   again.boost == 100,
               "settings: the saved values load back");
        DeleteFileW(path.c_str());

        MpvWatch watch;
        watch.on_line(R"({"event":"property-change","id":1,"name":"volume","data":100.000000})");
        expect(!watch.changed("volume"), "watch: nothing before start");
        watch.track({"volume", "user-data/glasir/audio-boost", "user-data/glasir/audio-delay"});
        watch.on_line(R"({"event":"property-change","id":1,"name":"volume","data":100.000000})");
        watch.on_line(R"({"event":"property-change","id":2,"name":"user-data/glasir/audio-boost"})");
        watch.on_line(R"({"event":"property-change","id":2,"name":"user-data/glasir/audio-boost","data":"100"})");
        watch.on_line(R"({"event":"property-change","id":3,"name":"user-data/glasir/audio-delay","data":"0"})");
        expect(!watch.changed("volume") && !watch.changed("user-data/glasir/audio-boost"),
               "watch: the first values are not changes");
        watch.on_line(R"({"event":"property-change","id":1,"name":"volume","data":85.000000})");
        watch.on_line(R"({"event":"property-change","id":2,"name":"user-data/glasir/audio-boost","data":"150"})");
        watch.on_line(R"({"event":"property-change","id":3,"name":"user-data/glasir/audio-delay","data":"30"})");
        watch.on_line(R"({"event":"property-change","id":3,"name":"user-data/glasir/audio-delay","data":"0"})");
        watch.on_line(R"({"request_id":0,"error":"success"})");
        expect(watch.changed("volume") == "85" && watch.changed("user-data/glasir/audio-boost") == "150",
               "watch: changed values");
        expect(!watch.changed("user-data/glasir/audio-delay"), "watch: changed and back is no change");
    }
    {
        // Updates (update.h): versions and GitHub's release answer.
        int v[3] = {};
        expect(parse_version(L"v0.2.10", v) && v[0] == 0 && v[1] == 2 && v[2] == 10, "version v0.2.10");
        expect(parse_version(L"1.0.0", v) && v[0] == 1, "version without v");
        expect(!parse_version(L"v1.0.0-rc1", v) && !parse_version(L"v1.0", v) && !parse_version(L"", v),
               "pre-release and short tags are no version");
        const int a[3] = {0, 2, 0}, b[3] = {0, 1, 9}, c[3] = {0, 2, 0};
        expect(newer(a, b) && !newer(b, a) && !newer(a, c), "0.2.0 is newer than 0.1.9, not than itself");
        int now[3];
        current_version(now);
        expect(version_text(now) == widen(GLASIR_VERSION), "this build's version");

        const std::string digest(64, 'a');
        const std::string release = R"({"tag_name":"v0.2.0","html_url":"https://github.com/Abhito/Glasir/releases/tag/v0.2.0",
            "draft":false,"prerelease":false,"assets":[
            {"name":"Glasir-0.2.0.zip.sig","browser_download_url":"https://x/sig","size":10,"digest":null},
            {"name":"Glasir-0.2.0.zip","browser_download_url":"https://x/zip","size":900000,"digest":"sha256:)" +
                                    digest + R"("}]})";
        Release r;
        std::wstring error;
        expect(read_release(release, r, error) && r.version[1] == 2 && r.zip_url == L"https://x/zip" &&
                   r.zip_size == 900000 && r.zip_sha256 == digest && r.zip_name == L"Glasir-0.2.0.zip",
               "release: the zip of its version, with its SHA-256");
        expect(!read_release(R"({"tag_name":"v0.3.0","assets":[{"name":"Glasir-0.2.0.zip","browser_download_url":"https://x"}]})",
                             r, error),
               "release: a zip of another version is not it");
        expect(!read_release(R"({"tag_name":"v0.3.0","prerelease":true,"assets":[]})", r, error),
               "release: a pre-release is not offered");
        expect(!read_release("not json", r, error), "release: not JSON");
    }
    {
        // Bug reports: the profile folder is left out, whatever its spelling.
        const std::wstring profile = L"C:\\Users\\Someone";
        expect(redact_profile("open C:\\Users\\Someone\\Documents\\x.ini and c:\\users\\SOMEONE\\y", profile) ==
                   "open %USERPROFILE%\\Documents\\x.ini and %USERPROFILE%\\y",
               "redact: backslashes, any case");
        expect(redact_profile("C:/Users/Someone/a {\"p\":\"C:\\\\Users\\\\Someone\\\\b\"}", profile) ==
                   "%USERPROFILE%/a {\"p\":\"%USERPROFILE%\\\\b\"}",
               "redact: slashes and JSON's doubled backslashes");
        expect(redact_profile("C:\\Users\\Someone2", L"C:\\") == "C:\\Users\\Someone2", "redact: no real profile");
    }
    {
        Settings s;
        std::wstring error;
        expect(s.update_check && apply_override(s, L"update.check=0", error) && !s.update_check,
               "update.check, on by default");
        expect(s.scaling == Settings::Scaling::Mpv && apply_override(s, L"video.scaling=VSR", error) &&
                   s.scaling == Settings::Scaling::Vsr && !apply_override(s, L"video.scaling=dlss", error),
               "video.scaling: mpv by default, vsr, not anything else");
        expect(apply_override(s, L"video.scaling=cnn", error) && s.scaling == Settings::Scaling::Cnn &&
                   std::wstring(scaling_name(s.scaling)) == L"cnn" &&
                   apply_override(s, L"video.scaling=Anime4K", error) && s.scaling == Settings::Scaling::Anime4k &&
                   std::wstring(scaling_name(s.scaling)) == L"anime4k" && apply_override(s, L"video.scaling=vsr", error),
               "video.scaling: cnn and anime4k, named back the same");
        expect(apply_override(s, L"window.mode=fullscreen", error) && s.fullscreen && s.borderless &&
                   apply_override(s, L"window.mode=borderless", error) && !s.fullscreen && s.borderless &&
                   apply_override(s, L"window.mode=windowed", error) && !s.fullscreen && !s.borderless &&
                   !apply_override(s, L"window.mode=exclusive", error),
               "window.mode: fullscreen, borderless, windowed");
        {
            Settings w;
            w.fullscreen = true;
            const auto wargs = mpv_arguments(MpvPlan{}, w);
            const auto has_arg = [&](const wchar_t *arg) {
                for (const auto &x : wargs)
                    if (x == arg) return true;
                return false;
            };
            expect(has_arg(L"--fullscreen") && has_arg(L"--no-border"),
                   "window.mode = fullscreen: mpv starts in its full screen, borderless when it leaves it");
        }
        MpvPlan plan;
        plan.vsr = L"intel";
        plan.layout.width = 1920;
        plan.layout.height = 1080;
        s.hdr = true;
        s.hdr_peak_nits = 800;
        const auto args = mpv_arguments(plan, s);
        auto has = [&](const wchar_t *arg) {
            for (const auto &a : args)
                if (a == arg) return true;
            return false;
        };
        expect(has(L"--script-opts-append=glasir_scaling-mode=vsr") &&
                   has(L"--script-opts-append=glasir_scaling-hdr=yes") &&
                   has(L"--script-opts-append=glasir_scaling-hdr_peak=800") &&
                   has(L"--script-opts-append=glasir_scaling-vsr=intel"),
               "mpv gets the scaling mode, the GPU's VSR and the HDR tags for VSR");
        GpuInfo gpu{L"NVIDIA GeForce RTX 5070 Ti", kVendorNvidia, true};
        expect(std::wstring(vsr_kind(gpu)) == L"nvidia", "VSR: NVIDIA RTX");
        gpu = {L"NVIDIA GeForce GTX 1660", kVendorNvidia, true};
        expect(std::wstring(vsr_kind(gpu)) == L"none", "VSR: not on a GTX");
        gpu = {L"Intel(R) Arc(TM) A770 Graphics", kVendorIntel, true};
        expect(std::wstring(vsr_kind(gpu)) == L"intel", "VSR: Intel");
        gpu = {L"AMD Radeon RX 7900 XTX", 0x1002, true};
        expect(std::wstring(vsr_kind(gpu)) == L"none", "VSR: none on AMD");
    }
    {
        // Full screen without the bars: where the picture goes (backdrop.h).
        auto same = [](const RECT &r, LONG l, LONG t, LONG rr, LONG b) {
            return r.left == l && r.top == t && r.right == rr && r.bottom == b;
        };
        expect(same(fit_picture(RECT{0, 0, 3440, 1440}, 1920, 1080), 440, 0, 3000, 1440),
               "16:9 on 3440x1440: 2560x1440 in the middle");
        expect(same(fit_picture(RECT{0, 0, 1920, 1200}, 3840, 2160), 0, 60, 1920, 1140),
               "16:9 on 16:10: bars above and below");
        expect(same(fit_picture(RECT{0, 0, 2560, 1440}, 1920, 1080), 0, 0, 2560, 1440),
               "16:9 on 16:9: the whole screen");
        expect(same(fit_picture(RECT{-2560, 100, 0, 1540}, 1280, 720), -2560, 100, 0, 1540),
               "16:9 on a 16:9 screen left of the main one: the whole screen");
        expect(same(fit_picture(RECT{0, 0, 1366, 768}, 1920, 1080), 0, 0, 1366, 768),
               "1366x768 is 16:9 but for rounding: the whole screen");
        expect(same(fit_picture(RECT{0, 0, 3440, 1440}, 0, 0), 0, 0, 3440, 1440), "no picture size: the whole screen");
        // A window from the menu: centred in the work area, smaller if it must be.
        expect(same(centre_window(RECT{0, 0, 3440, 1392}, 1280, 720), 1080, 336, 2360, 1056),
               "1280x720 window centred above the taskbar");
        expect(same(centre_window(RECT{616, -1317, 2536, -237}, 1920, 1080), 712, -1263, 2440, -291),
               "a window as large as the screen: 90 % of it, centred");
    }
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
