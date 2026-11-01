// SPDX-License-Identifier: MIT
// Checks picking the card's mode, settings parsing, and mpv's command line.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "devices.h"
#include "mpv_launch.h"
#include "settings.h"

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
        reread.hdr = true;  // must be reset by the file's mode = sdr
        expect(load_settings(path, reread, error) && !reread.hdr && reread.dedup && reread.borderless,
               "the written defaults read back");
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
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
