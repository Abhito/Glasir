// SPDX-License-Identifier: MIT
// shot_convert IN.png OUT_FOLDER [STEM]: makes the files Glasir's screenshot
// would make of a frame saved by a frame generation capture (Captures\...\
// f01.png: 16-bit PNG = HDR10 PQ, 8-bit = SDR), then reads the .jxr back and
// checks it holds the same light. To try the HDR tone mapping on game
// pictures without the game.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "screenshot.h"

using Microsoft::WRL::ComPtr;
using namespace glasir::overlay;

namespace {

bool load(IWICImagingFactory *wic, const std::wstring &path, shot::Frame &frame) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> source;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              &decoder)) ||
        FAILED(decoder->GetFrame(0, &source)))
        return false;
    WICPixelFormatGUID format{};
    source->GetPixelFormat(&format);
    source->GetSize(&frame.width, &frame.height);
    const size_t n = size_t(frame.width) * frame.height;
    const bool deep = IsEqualGUID(format, GUID_WICPixelFormat64bppRGBA) || IsEqualGUID(format, GUID_WICPixelFormat48bppRGB);
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), deep ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    std::vector<BYTE> pixels(n * (deep ? 8 : 4));
    if (FAILED(converter->CopyPixels(nullptr, frame.width * (deep ? 8 : 4), UINT(pixels.size()), pixels.data())))
        return false;
    frame.pixels.resize(n * 4);
    if (deep) {
        // The capture's 16 bits are the back buffer's 10 bits, repeated.
        frame.format = DXGI_FORMAT_R10G10B10A2_UNORM;
        frame.encoding = Encoding::Pq;
        for (size_t i = 0; i < n; ++i) {
            const UINT16 *c = reinterpret_cast<const UINT16 *>(pixels.data() + i * 8);
            const UINT32 v = UINT32(c[0] >> 6) | UINT32(c[1] >> 6) << 10 | UINT32(c[2] >> 6) << 20 | 3u << 30;
            memcpy(frame.pixels.data() + i * 4, &v, 4);
        }
    } else {
        frame.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        frame.encoding = Encoding::Sdr;
        frame.pixels = std::move(pixels);
    }
    return true;
}

// Reads the .jxr back as linear floats and compares it with the frame.
bool check_jxr(IWICImagingFactory *wic, const std::wstring &path, const shot::Frame &frame) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> source;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              &decoder)) ||
        FAILED(decoder->GetFrame(0, &source)) || FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat128bppRGBAFloat, WICBitmapDitherTypeNone, nullptr,
                                     0, WICBitmapPaletteTypeCustom))) {
        printf("jxr: cannot read it back\n");
        return false;
    }
    UINT w = 0, h = 0;
    source->GetSize(&w, &h);
    std::vector<float> px(size_t(w) * h * 4);
    if (w != frame.width || h != frame.height ||
        FAILED(converter->CopyPixels(nullptr, w * 16, UINT(px.size() * 4), reinterpret_cast<BYTE *>(px.data())))) {
        printf("jxr: wrong size or unreadable\n");
        return false;
    }
    double peak = 0, sum = 0, err = 0, ref_sum = 0;
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        float rgb[3];
        shot::scrgb(frame, i, rgb);
        const double y = 0.2126 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.0722 * px[i * 4 + 2];
        const double ref = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
        peak = std::max(peak, y * 80);
        sum += y * 80;
        ref_sum += ref * 80;
        err += std::abs(y - ref) * 80;
    }
    const double n = double(w) * h;
    printf("jxr: %ux%u, mean %.1f nits (source %.1f), peak %.0f nits, mean error %.2f nits\n", w, h, sum / n,
           ref_sum / n, peak, err / n);
    return err / n < 0.02 * std::max(1.0, ref_sum / n) + 0.5;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: shot_convert IN.png OUT_FOLDER [STEM]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int rc = 1;
    {
        ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        shot::Frame frame;
        if (!wic || !load(wic.Get(), argv[1], frame)) {
            fprintf(stderr, "cannot read %ls\n", argv[1]);
        } else {
            std::vector<std::wstring> files;
            std::string error;
            const DWORD start = GetTickCount();
            if (!shot::save(frame, argv[2], argc > 3 ? argv[3] : L"shot", files, error)) {
                fprintf(stderr, "save failed: %s\n", error.c_str());
            } else {
                printf("%s %ux%u in %lu ms:\n", shot::is_hdr(frame) ? "HDR" : "SDR", frame.width, frame.height,
                       GetTickCount() - start);
                for (const auto &f : files) printf("  %ls\n", f.c_str());
                rc = 0;
                if (shot::is_hdr(frame) && !check_jxr(wic.Get(), files[0], frame)) rc = 1;
            }
        }
    }
    CoUninitialize();
    return rc;
}
