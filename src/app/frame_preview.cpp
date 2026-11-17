// SPDX-License-Identifier: MIT
#include "frame_preview.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

#include "tone_map.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace glasir {
namespace {

// Tried in turn until the JPEGs fit.
constexpr float kQualities[] = {0.90f, 0.75f, 0.60f};
// Frames converted at a time (each holds its frame twice: about 60 MB at
// 2560x1440, 100 MB at 3840x2160).
constexpr unsigned kMaxThreads = 3;

struct Picture {
    UINT width = 0, height = 0;
    bool hdr = false;
    std::vector<BYTE> bgr;  // 3 bytes a pixel, rows tightly packed
};

HRESULT read_png(IWICImagingFactory *wic, const std::wstring &path, Picture &p) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> source;
    HRESULT hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &source);
    WICPixelFormatGUID format{};
    if (SUCCEEDED(hr)) hr = source->GetPixelFormat(&format);
    if (SUCCEEDED(hr)) hr = source->GetSize(&p.width, &p.height);
    if (FAILED(hr)) return hr;
    if (p.width == 0 || p.height == 0 || p.width > 16384 || p.height > 16384) return WINCODEC_ERR_BADIMAGE;
    const size_t n = size_t(p.width) * p.height;
    // 16 bits a channel is what the capture writes for HDR10 (PQ): the back
    // buffer's 10 bits, repeated.
    p.hdr = IsEqualGUID(format, GUID_WICPixelFormat64bppRGBA) || IsEqualGUID(format, GUID_WICPixelFormat48bppRGB);
    ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(source.Get(), p.hdr ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat24bppBGR,
                                   WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    p.bgr.resize(n * 3);
    if (!p.hdr) return converter->CopyPixels(nullptr, p.width * 3, UINT(p.bgr.size()), p.bgr.data());
    std::vector<BYTE> deep(n * 8);
    hr = converter->CopyPixels(nullptr, p.width * 8, UINT(deep.size()), deep.data());
    if (FAILED(hr)) return hr;
    for (size_t i = 0; i < n; ++i) {
        UINT16 c[4];
        memcpy(c, deep.data() + i * 8, 8);
        float rgb[3];
        tone::pq2020_to_scrgb(c[0] >> 6, c[1] >> 6, c[2] >> 6, rgb);
        tone::to_sdr(rgb, tone::kReferenceWhiteNits, p.bgr.data() + i * 3);
    }
    return S_OK;
}

HRESULT write_jpeg(IWICImagingFactory *wic, const std::wstring &path, Picture &p, float quality) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
    if (SUCCEEDED(hr) && options) {
        // Colour at full resolution (4:4:4): thin coloured edges and small
        // text are what frame generation gets wrong.
        PROPBAG2 names[2]{};
        names[0].pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        names[1].pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
        VARIANT values[2];
        VariantInit(&values[0]);
        VariantInit(&values[1]);
        values[0].vt = VT_R4;
        values[0].fltVal = quality;
        values[1].vt = VT_UI1;
        values[1].bVal = WICJpegYCrCbSubsampling444;
        hr = options->Write(2, names, values);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(p.width, p.height);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(p.height, p.width * 3, UINT(p.bgr.size()), p.bgr.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;
}

struct Result {
    FramePreviews done;
    std::wstring error;
};

// Every frame at this quality, a few at a time.
Result convert(const std::vector<fs::path> &frames, const fs::path &to, float quality) {
    Result r;
    std::mutex m;
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    auto work = [&] {
        // Below normal priority: a game is being played meanwhile.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        {
            ComPtr<IWICImagingFactory> wic;
            HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
            for (size_t i; !failed && (i = next++) < frames.size();) {
                const fs::path jpg = to / frames[i].stem().concat(L".jpg");
                Picture p;
                if (SUCCEEDED(hr)) hr = read_png(wic.Get(), frames[i].wstring(), p);
                const bool read = SUCCEEDED(hr);
                DeleteFileW(jpg.c_str());  // one of a higher quality that was too big
                if (read) hr = write_jpeg(wic.Get(), jpg.wstring(), p, quality);
                std::error_code ec;
                const uint64_t bytes = SUCCEEDED(hr) ? fs::file_size(jpg, ec) : 0;
                std::lock_guard lock(m);
                if (FAILED(hr) || ec) {
                    if (!failed.exchange(true))
                        r.error = std::format(L"could not {} {} (0x{:08X})", read ? L"write the preview of" : L"read",
                                              frames[i].filename().wstring(), static_cast<unsigned long>(hr));
                    break;
                }
                ++r.done.frames;
                r.done.bytes += bytes;
                r.done.hdr = r.done.hdr || p.hdr;
                r.done.width = std::max(r.done.width, unsigned(p.width));
                r.done.height = std::max(r.done.height, unsigned(p.height));
            }
        }
        if (com) CoUninitialize();
    };
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency() / 2);
    const size_t count = std::min({size_t(kMaxThreads), size_t(cores), frames.size()});
    std::vector<std::thread> threads;
    try {
        for (size_t i = 1; i < count; ++i) threads.emplace_back(work);
    } catch (...) {  // no more threads: this one does it
    }
    const int priority = GetThreadPriority(GetCurrentThread());
    work();
    SetThreadPriority(GetCurrentThread(), priority);
    for (auto &t : threads) t.join();
    r.done.quality = int(quality * 100.0f + 0.5f);
    return r;
}

}  // namespace

bool write_frame_previews(const std::wstring &from, const std::wstring &to, uint64_t max_bytes, FramePreviews &out,
                          std::wstring &error) {
    out = {};
    std::error_code ec;
    std::vector<fs::path> frames;
    uint64_t source_bytes = 0;
    for (const fs::directory_entry &e : fs::directory_iterator(from, ec)) {
        if (!e.is_regular_file(ec) || _wcsicmp(e.path().extension().c_str(), L".png") != 0) continue;
        frames.push_back(e.path());
        source_bytes += e.file_size(ec);
    }
    if (frames.empty()) return error = L"the folder has no frames", false;
    std::sort(frames.begin(), frames.end());
    fs::create_directories(to, ec);
    for (const float quality : kQualities) {
        Result r = convert(frames, to, quality);
        if (!r.error.empty()) {
            fs::remove_all(to, ec);
            error = r.error;
            return false;
        }
        out = r.done;
        out.source_bytes = source_bytes;
        if (out.bytes <= max_bytes) break;  // else again, smaller (the last try stays as it is)
    }
    return true;
}

}  // namespace glasir
