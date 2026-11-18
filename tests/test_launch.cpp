// SPDX-License-Identifier: MIT
// Checks picking the card's mode, settings parsing, and mpv's command line.
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "bug_report.h"
#include "backdrop.h"
#include "components.h"
#include "devices.h"
#include "files.h"
#include "frame_preview.h"
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

// A PNG as a frame generation capture writes it: 16 bits a channel (HDR10
// PQ, R G B A) or 8 (B G R A), every pixel the same but the right half black.
bool write_png(const std::wstring &path, UINT w, UINT h, bool deep, const UINT16 left[3]) {
    using Microsoft::WRL::ComPtr;
    const UINT bytes = deep ? 8 : 4;
    std::vector<BYTE> pixels(size_t(w) * h * bytes);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            BYTE *px = pixels.data() + (size_t(y) * w + x) * bytes;
            if (deep) {
                const UINT16 c[4] = {x < w / 2 ? left[0] : UINT16(0), x < w / 2 ? left[1] : UINT16(0),
                                     x < w / 2 ? left[2] : UINT16(0), 0xFFFF};
                memcpy(px, c, 8);
            } else {
                for (int k = 0; k < 3; ++k) px[k] = x < w / 2 ? BYTE(left[k]) : BYTE(0);
                px[3] = 255;
            }
        }
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = deep ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
           SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           SUCCEEDED(frame->WritePixels(h, w * bytes, UINT(pixels.size()), pixels.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// The size of an image file and its pixel at (x, y), as B G R.
bool read_pixel(const std::wstring &path, UINT x, UINT y, UINT &w, UINT &h, BYTE bgr[3]) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> source;
    ComPtr<IWICFormatConverter> converter;
    const WICRect one{INT(x), INT(y), 1, 1};
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
           SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                    &decoder)) &&
           SUCCEEDED(decoder->GetFrame(0, &source)) && SUCCEEDED(source->GetSize(&w, &h)) &&
           SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
           SUCCEEDED(converter->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                           WICBitmapPaletteTypeCustom)) &&
           SUCCEEDED(converter->CopyPixels(&one, 3, 3, bgr));
}

bool near_to(BYTE value, int wanted, int by) { return value >= wanted - by && value <= wanted + by; }

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
        expect(s.hdr_auto && !s.hdr, "video.mode: automatic unless said");
        expect(apply_override(s, L"video.mode=hdr", error) && s.hdr && !s.hdr_auto, "--set video.mode=hdr");
        expect(apply_override(s, L"video.mode=auto", error) && s.hdr_auto && !s.hdr &&
                   apply_override(s, L"video.auto_found=sdr", error) && !s.auto_found_hdr &&
                   !apply_override(s, L"video.mode=dolby", error),
               "--set video.mode=auto, and what it found last");
        expect(apply_override(s, L"video.mode=hdr", error) && s.hdr && !s.hdr_auto, "back to video.mode=hdr");
        expect(!Settings().rtx_hdr && apply_override(s, L"video.rtx_hdr=1", error) && s.rtx_hdr &&
                   apply_override(s, L"video.rtx_hdr=0", error) && !s.rtx_hdr && !apply_override(s, L"video.rtx_hdr=x", error),
               "video.rtx_hdr: off unless said (mode = hdr is the way out when Automatic misreads), 1 or 0");
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
                   has(args, L"--vf=@glasir-colour:format=colormatrix=bt.709:colorlevels=limited:primaries=bt.709:"
                             L"chroma-location=uhd") &&
                   has(args, L"--target-colorspace-hint=no") &&
                   has(args, L"--script-opts-append=glasir_scaling-rtx_hdr=no"),
               "SDR NV12: raw format, rate and BT.709 tags (a labelled filter: replaced while playing)");
        plan.layout.format = PixelFormat::YUY2;
        args = mpv_arguments(plan, s);
        expect(has(args, L"--demuxer-rawvideo-mp-format=yuyv422") &&
                   has(args, L"--vf=@glasir-colour:format=colormatrix=bt.709:colorlevels=limited:primaries=bt.709:"
                             L"chroma-location=mpeg2/4/h264"),
               "SDR YUY2: yuyv422, chroma sited left");
        s.hdr = true;
        plan.layout.format = PixelFormat::P010;
        plan.fps = 59.94;
        args = mpv_arguments(plan, s);
        expect(has(args, L"--demuxer-rawvideo-mp-format=p010") && has(args, L"--demuxer-rawvideo-fps=59.94") &&
                   has(args, L"--d3d11-output-csp=pq") && has(args, L"--target-colorspace-hint=yes"),
               "HDR P010: PQ output");
        // The picture switched while playing: the tags (the same label, so
        // the filter is replaced), the script's filters, mpv's output, what
        // the menu reads.
        const std::string to_sdr = picture_commands(plan, s, Picture{false, false});
        const std::string to_hdr = picture_commands(plan, s, Picture{true, false});
        const std::string to_rtx = picture_commands(plan, s, Picture{false, true});
        auto in = [](const std::string &text, const char *part) { return text.find(part) != std::string::npos; };
        expect(in(to_sdr, "[\"vf\",\"add\",\"@glasir-colour:format=colormatrix=bt.709:colorlevels=limited:"
                          "primaries=bt.709:chroma-location=uhd\"]") &&
                   in(to_sdr, "[\"script-message\",\"glasir-signal\",\"sdr\"]") &&
                   in(to_sdr, "[\"set\",\"target-colorspace-hint\",\"no\"]") &&
                   in(to_sdr, "[\"set\",\"user-data/glasir/signal\",\"sdr\"]"),
               "live switch to SDR: BT.709 tags, an SDR output");
        expect(in(to_hdr, "[\"vf\",\"add\",\"@glasir-colour:format=colormatrix=bt.2020-ncl:") &&
                   in(to_hdr, "gamma=pq:max-luma=1000:max_cll=1000\"]") &&
                   in(to_hdr, "[\"script-message\",\"glasir-signal\",\"hdr\"]") &&
                   in(to_hdr, "[\"set\",\"target-colorspace-hint\",\"yes\"]") &&
                   in(to_hdr, "[\"set\",\"user-data/glasir/signal\",\"hdr\"]"),
               "live switch to HDR: PQ tags, an HDR output");
        expect(in(to_rtx, "primaries=bt.709:chroma-location=uhd\"]") &&
                   in(to_rtx, "[\"script-message\",\"glasir-signal\",\"rtx\"]") &&
                   in(to_rtx, "[\"set\",\"target-colorspace-hint\",\"yes\"]") &&
                   in(to_rtx, "[\"set\",\"user-data/glasir/signal\",\"sdr\"]") &&
                   !in(to_rtx, "d3d11-output-csp"),
               "live switch to SDR through RTX Video HDR: BT.709 tags, an HDR output (never d3d11-output-csp)");
        expect(Picture{false, true}.output_hdr() && Picture{true, false}.output_hdr() && !Picture{false, false}.output_hdr(),
               "the screen is in HDR for an HDR signal and for RTX Video HDR");
        // A start with an SDR signal through RTX Video HDR: SDR tags, HDR output.
        s.hdr = false;
        plan.rtx_hdr = true;
        args = mpv_arguments(plan, s);
        expect(has(args, L"--vf=@glasir-colour:format=colormatrix=bt.709:colorlevels=limited:primaries=bt.709:"
                         L"chroma-location=uhd") &&
                   has(args, L"--script-opts-append=glasir_scaling-hdr=no") &&
                   has(args, L"--script-opts-append=glasir_scaling-rtx_hdr=yes") &&
                   has(args, L"--d3d11-output-csp=pq") && has(args, L"--target-colorspace-hint=yes"),
               "RTX Video HDR at start: SDR tags, the filter asked for, PQ output");
        plan.rtx_hdr = false;
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
                                 L"\\nvngx_dlss.dll"})
            touch(f, "x");
        // DLSS5-Feeder 2.0's shader: its own motion vectors are provider 5.
        touch(L"\\reshade-shaders\\Shaders\\DLSS5Feeder\\DLSS5_Feed.fx", "#elif DLSS5_MV_PROVIDER == 5\n");
        expect(dlss5_installed(engine), "DLSS 5 files there: installed");
        write_reshade_config(engine, s, true, true);
        std::string ini = read(L"\\ReShade.ini");
        expect(ini.find(".\\reshade-shaders\\Shaders\\**,.\\,D:\\MyShaders\\**") != std::string::npos,
               "effect path added in front, the user's kept");
        expect(ini.find("TextureSearchPaths=.\\reshade-shaders\\Textures\\**\r\n") != std::string::npos,
               "texture path replaces ReShade's bare default");
        expect(ini.find("PreprocessorDefinitions=DLSS5_MV_PROVIDER=5\r\n") != std::string::npos,
               "motion vectors: the Feeder's own");
        expect(ini.find("TutorialProgress=1") != std::string::npos, "an existing ReShade value is not overwritten");
        expect(ini.find("NeuralUplift=1") != std::string::npos && ini.find("[GLASIR_FRAMEGEN]") != std::string::npos,
               "DLSS 5 and frame generation keys written");
        expect(ini.find("NRScreenshotSource=2") != std::string::npos, "F5 screenshots: model buffers, not Auto");
        WritePrivateProfileStringW(L"RenoDX.DLSS5", L"NRScreenshotSource", L"1", (engine + L"\\ReShade.ini").c_str());
        write_reshade_config(engine, s, true, true);
        expect(read(L"\\ReShade.ini").find("NRScreenshotSource=1") != std::string::npos,
               "F5 screenshots: a source other than Auto is kept");
        expect(read(L"\\ReShadePreset.ini").rfind("Techniques=DLSS5_Feed@DLSS5_Feed.fx\r\n", 0) == 0,
               "preset enables the DLSS 5 effect, nothing else");

        // An install from Glasir 0.1.3: LumeniteFX's vectors in ReShade.ini.
        auto defines = [&] {
            wchar_t v[256] = {};
            GetPrivateProfileStringW(L"GENERAL", L"PreprocessorDefinitions", L"", v, 256,
                                     (engine + L"\\ReShade.ini").c_str());
            return std::wstring(v);
        };
        auto set_defines = [&](const wchar_t *v) {
            WritePrivateProfileStringW(L"GENERAL", L"PreprocessorDefinitions", v, (engine + L"\\ReShade.ini").c_str());
        };
        set_defines(L"DLSS5_MV_PROVIDER=3,MY_DEFINE=1");
        write_reshade_config(engine, s, true, true);
        expect(defines() == L"DLSS5_MV_PROVIDER=5,MY_DEFINE=1",
               "from LumeniteFX to the Feeder's own vectors once the Feeder has them; other defines kept");
        // Updated Glasir, DLSS 5 not yet: Feeder 1.19 with LumeniteFX stays as it was.
        CreateDirectoryW((engine + L"\\reshade-shaders\\Shaders\\LumeniteFX").c_str(), nullptr);
        touch(L"\\reshade-shaders\\Shaders\\LumeniteFX\\lumenite_Kernel.fx", "x");
        touch(L"\\reshade-shaders\\Shaders\\DLSS5Feeder\\DLSS5_Feed.fx", "#elif DLSS5_MV_PROVIDER == 4\n");
        write_reshade_config(engine, s, true, true);
        expect(defines() == L"DLSS5_MV_PROVIDER=3,MY_DEFINE=1" &&
                   read(L"\\ReShadePreset.ini").rfind("Techniques=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed", 0) == 0,
               "an older Feeder: LumeniteFX's vectors and technique until DLSS 5 is updated");
        // A provider a user installed is theirs.
        set_defines(L"DLSS5_MV_PROVIDER=2");
        write_reshade_config(engine, s, true, true);
        expect(defines() == L"DLSS5_MV_PROVIDER=2", "a provider the user chose (1, 2) is kept");
        set_defines(L"DLSS5_MV_PROVIDER=15,,A=1");
        write_reshade_config(engine, s, true, true);
        expect(defines() == L"DLSS5_MV_PROVIDER=3,A=1", "=15 is not provider 1; empty items dropped");
        std::wstring many = L"DLSS5_MV_PROVIDER=3";
        for (int i = 0; i < 300; ++i) many += std::format(L",DEFINE_NUMBER_{}=1", i);
        set_defines(many.c_str());
        write_reshade_config(engine, s, true, true);
        wchar_t whole[16384] = {};
        GetPrivateProfileStringW(L"GENERAL", L"PreprocessorDefinitions", L"", whole, 16384,
                                 (engine + L"\\ReShade.ini").c_str());
        expect(std::wstring(whole).ends_with(L",DEFINE_NUMBER_299=1"), "a long list of defines is kept whole");
        DeleteFileW((engine + L"\\reshade-shaders\\Shaders\\LumeniteFX\\lumenite_Kernel.fx").c_str());
        RemoveDirectoryW((engine + L"\\reshade-shaders\\Shaders\\LumeniteFX").c_str());
        set_defines(L"");
        touch(L"\\reshade-shaders\\Shaders\\DLSS5Feeder\\DLSS5_Feed.fx", "#elif DLSS5_MV_PROVIDER == 5\n");
        write_reshade_config(engine, s, true, true);
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
        // Bug reports: JPEG pictures of a capture's frames, HDR tone-mapped.
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        WorkDir work(L"test-previews");
        const std::wstring from = work.path() + L"\\20260101-120000-000", to = work.path() + L"\\out";
        CreateDirectoryW(from.c_str(), nullptr);
        // 203 nits (the reference white) in 10-bit PQ is code 594, repeated into 16 bits.
        const UINT16 white[3] = {UINT16(594 << 6 | 594 >> 4), UINT16(594 << 6 | 594 >> 4), UINT16(594 << 6 | 594 >> 4)};
        const UINT16 orange[3] = {20, 128, 240};  // B G R
        expect(write_png(from + L"\\f01.png", 64, 32, true, white) &&
                   write_png(from + L"\\between_f01_f02.png", 64, 32, false, orange),
               "previews: a capture to make them of");
        FramePreviews p;
        std::wstring error;
        const bool made = write_frame_previews(from, to, 20u << 20, p, error);
        std::error_code ec;
        const uint64_t size = std::filesystem::file_size(to + L"\\f01.jpg", ec) +
                              std::filesystem::file_size(to + L"\\between_f01_f02.jpg", ec);
        expect(made && p.frames == 2 && p.width == 64 && p.height == 32 && p.hdr && p.quality == 90 && !ec &&
                   p.bytes == size && p.source_bytes > 0,
               "previews: one JPEG a frame, at the frame's size");
        UINT w = 0, h = 0;
        BYTE left[3] = {}, right[3] = {};
        // The reference white is above the knee: 0.75 + 0.25 * tanh(1) = 0.94 of the SDR white, 248 in sRGB.
        expect(read_pixel(to + L"\\f01.jpg", 8, 16, w, h, left) && read_pixel(to + L"\\f01.jpg", 56, 16, w, h, right) &&
                   w == 64 && h == 32 && near_to(left[0], 248, 4) && near_to(left[1], 248, 4) && near_to(left[2], 248, 4) &&
                   right[0] <= 3 && right[1] <= 3 && right[2] <= 3,
               "previews: HDR (16-bit PQ) tone-mapped as the screenshot is");
        expect(read_pixel(to + L"\\between_f01_f02.jpg", 8, 16, w, h, left) && near_to(left[0], 20, 6) &&
                   near_to(left[1], 128, 6) && near_to(left[2], 240, 6),
               "previews: SDR (8-bit) as it is");
        expect(write_frame_previews(from, to, 1, p, error) && p.frames == 2 && p.quality == 60 && p.bytes < size,
               "previews: a lower quality when they are too big");
        {
            std::ofstream bad(from + L"\\f02.png", std::ios::binary);
            bad << "not a PNG";
        }
        expect(!write_frame_previews(from, to, 20u << 20, p, error) && error.find(L"f02.png") != std::wstring::npos &&
                   !std::filesystem::exists(to),
               "previews: a frame that cannot be read: none, and why");
        CreateDirectoryW((work.path() + L"\\empty").c_str(), nullptr);
        expect(!write_frame_previews(work.path() + L"\\empty", to, 20u << 20, p, error), "previews: no frames, none");
        if (com) CoUninitialize();
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

        // DLSS 5 neural rendering: RTX 30, 40 and 50 series.
        auto nr = [](const wchar_t *name, unsigned vendor = kVendorNvidia) {
            return dlss5_gpu(GpuInfo{name, vendor, true});
        };
        expect(nr(L"NVIDIA GeForce RTX 5070 Ti") && nr(L"NVIDIA GeForce RTX 4070 SUPER") &&
                   nr(L"NVIDIA GeForce RTX 3060 Laptop GPU") && nr(L"NVIDIA GeForce RTX 3090") &&
                   nr(L"NVIDIA GeForce RTX 4060") && nr(L"NVIDIA GeForce RTX 5090"),
               "DLSS 5: GeForce RTX 30, 40 and 50");
        expect(nr(L"NVIDIA RTX A4000") && nr(L"NVIDIA RTX 4000 Ada Generation") &&
                   nr(L"NVIDIA RTX PRO 6000 Blackwell Workstation Edition"),
               "DLSS 5: workstation RTX of the same generations");
        for (const char *name : {"NVIDIA GeForce RTX 2080 Ti", "NVIDIA GeForce RTX 2050", "Quadro RTX 4000",
                                 "Quadro RTX 5000", "Quadro RTX 6000", "Quadro RTX 8000", "NVIDIA GeForce GTX 1660",
                                 "NVIDIA GeForce MX570"}) {
            const std::string what = std::string("DLSS 5: not on ") + name;
            expect(!nr(std::wstring(name, name + std::strlen(name)).c_str()), what.c_str());
        }
        expect(!nr(L"NVIDIA GeForce RTX 3060", 0x1002), "DLSS 5: not on another vendor");
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
    {
        // The tested versions of the downloaded programs (components.json).
        const std::string sha(64, 'a');
        PinSet pins;
        std::wstring error;
        expect(read_pins(R"({"about":"text","mpv":{"version":"1","url":"https://x/y.7z","sha256":")" +
                             std::string(64, 'A') + R"(","size":12}})",
                         pins, error) &&
                   pins.parts.size() == 1 && pins.find("mpv") && pins.find("mpv")->sha256 == sha &&
                   pins.find("mpv")->size == 12 && !pins.find("reshade"),
               "tested set: a part read, its SHA-256 in lower case, text members skipped");
        expect(!read_pins(R"({"mpv":{"version":"1","url":"https://x/y.7z","sha256":"abc"}})", pins, error) &&
                   !read_pins(R"({"mpv":{"version":"1","url":"http://x/y.7z","sha256":")" + sha + R"("}})", pins, error) &&
                   !read_pins(R"({"mpv":{"url":"https://x/y.7z","sha256":")" + sha + R"("}})", pins, error) &&
                   !read_pins("[]", pins, error) && pins.parts.empty(),
               "tested set: a short SHA-256, http, no version or no object is refused");
        const PinSet &tested = tested_components();
        bool all = true;
        for (const char *part : {"mpv", "reshade", "feeder", "renodx", "dlssnr", "dlss", "reshade_fxh", "vfg"})
            all = all && tested.find(part);
        expect(all, "tested set: the built-in one names every part");

        // What is installed against it (components.ini in a scratch folder).
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring engine = std::wstring(tmp) + L"glasir_test_components";
        CreateDirectoryW(engine.c_str(), nullptr);
        const std::wstring ini = engine + L"\\components.ini";
        DeleteFileW(ini.c_str());
        auto set = [&](const wchar_t *section, const std::wstring &key, const std::wstring &value) {
            WritePrivateProfileStringW(section, key.c_str(), value.c_str(), ini.c_str());
        };
        const Pin *mpv = tested.find("mpv");
        expect(mpv && !component_as_tested(engine, Component::Mpv), "nothing recorded: not the tested mpv");
        if (mpv) {
            set(L"mpv", L"version", mpv->version);
            set(L"mpv", L"sha256", widen(mpv->sha256));
            set(L"mpv", L"source", mpv->url);
            expect(component_as_tested(engine, Component::Mpv), "the tested mpv: version and SHA-256 match");
            set(L"mpv", L"sha256", widen(sha));
            expect(!component_as_tested(engine, Component::Mpv), "the tested version with another SHA-256: not it");
            set(L"mpv", L"version", L"20990101-git-0000000000");
            expect(!component_as_tested(engine, Component::Mpv), "a newer mpv: not the tested one");
            set(L"mpv", L"instead_of", mpv->version);
            expect(component_as_tested(engine, Component::Mpv),
                   "the newest in place of a tested build no longer offered: nothing to update");
        }
        // As Glasir 0.1.3 recorded DLSS 5: versions only, two parts by date.
        for (const char *part : {"feeder", "renodx", "dlssnr", "dlss"})
            if (const Pin *pin = tested.find(part)) set(L"dlss5", widen(part), pin->version);
        set(L"dlss5", L"lumenite", L"mainline 2026-11-05");
        set(L"dlss5", L"reshade_fxh", L"crosire/reshade-shaders slim 2026-11-05");
        expect(!component_as_tested(engine, Component::Dlss5), "DLSS 5 with parts taken from a branch's head: not tested");
        for (const char *part : {"reshade_fxh"})
            if (const Pin *pin = tested.find(part)) {
                set(L"dlss5", widen(part), pin->version);
                set(L"dlss5", widen(part) + L"_sha256", widen(pin->sha256));
                set(L"dlss5", widen(part) + L"_source", pin->url);
            }
        expect(component_as_tested(engine, Component::Dlss5),
               "DLSS 5: every part in its tested version (LumeniteFX, from before, does not count)");
        bool hidden = true, shown = false;
        for (const auto &line : installed_versions(engine)) {
            hidden = hidden && line.find(L"https://") == std::wstring::npos && line.find(widen(sha)) == std::wstring::npos;
            shown = shown || line.find(L"instead_of=") != std::wstring::npos;
        }
        expect(hidden && shown, "installed versions: no addresses or SHA-256s, the stand-in said");
        DeleteFileW(ini.c_str());
        RemoveDirectoryW(engine.c_str());
    }
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
