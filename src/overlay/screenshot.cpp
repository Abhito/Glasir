// SPDX-License-Identifier: MIT
#include "screenshot.h"

#include <windows.h>
#include <DirectXPackedVector.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace glasir::overlay::shot {
namespace {

// Below this share of the SDR white, tone mapping leaves light as it is;
// above it, the brightest channel rolls off toward 1.0.
constexpr float kKnee = 0.75f;
// GPU copies are read back on a later frame; one not done after this many
// frames is given up.
constexpr int kMaxTries = 120;

// SMPTE ST 2084 (PQ): a 0-1 signal to nits.
float pq_to_nits(float e) {
    constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
    const float p = std::pow(std::clamp(e, 0.0f, 1.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

// 10-bit PQ code to nits, made once.
const float *pq_table() {
    static float table[1024];
    static bool made = false;
    if (!made) {
        for (int i = 0; i < 1024; ++i) table[i] = pq_to_nits(i / 1023.0f);
        made = true;
    }
    return table;
}

float srgb_encode(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

BYTE to_byte(float v) { return BYTE(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); }

std::wstring hr_text(const char *what, HRESULT hr) {
    wchar_t text[96];
    swprintf(text, 96, L"%hs failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return text;
}

std::string narrow(const std::wstring &w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Writes one image with WIC. format: the pixels' format, which the encoder
// must take as it is.
HRESULT write_image(IWICImagingFactory *wic, const std::wstring &path, const GUID &container, WICPixelFormatGUID format,
                    UINT width, UINT height, UINT stride, std::vector<BYTE> &pixels, std::wstring &error) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
    if (SUCCEEDED(hr) && options && container == GUID_ContainerFormatWmp) {
        // JPEG XR: nearly lossless, about a fifth of the lossless size.
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = 0.95f;
        options->Write(1, &option, &value);  // the default quality if it is refused
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    const WICPixelFormatGUID wanted = format;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && !IsEqualGUID(format, wanted)) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(height, stride, UINT(pixels.size()), pixels.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) {
        error = hr_text(container == GUID_ContainerFormatWmp ? "writing the .jxr" : "writing the .png", hr);
        stream.Reset();
        DeleteFileW(path.c_str());  // no half-written file
    }
    return hr;
}

bool exists(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

bool supported(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool is_hdr(const Frame &frame) {
    // Half floats are always linear (scRGB); 10 bits are PQ only when the
    // swap chain says so (an SDR picture on a 10-bit desktop is 10-bit too).
    return frame.format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           (frame.format == DXGI_FORMAT_R10G10B10A2_UNORM && frame.encoding == Encoding::Pq);
}

void scrgb(const Frame &frame, size_t pixel, float rgb[3]) {
    if (frame.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        DirectX::PackedVector::HALF h[4];
        memcpy(h, frame.pixels.data() + pixel * 8, 8);
        for (int c = 0; c < 3; ++c) rgb[c] = DirectX::PackedVector::XMConvertHalfToFloat(h[c]);
        return;
    }
    // PQ, BT.2020 primaries: to linear BT.709 (ITU-R BT.2087), 1.0 = 80 nits.
    const float *nits = pq_table();
    UINT32 v;
    memcpy(&v, frame.pixels.data() + pixel * 4, 4);
    const float r = nits[v & 1023], g = nits[(v >> 10) & 1023], b = nits[(v >> 20) & 1023];
    rgb[0] = (1.660491f * r - 0.587641f * g - 0.072850f * b) / 80.0f;
    rgb[1] = (-0.124550f * r + 1.132900f * g - 0.008349f * b) / 80.0f;
    rgb[2] = (-0.018151f * r - 0.100579f * g + 1.118730f * b) / 80.0f;
}

void tone_map(const float in[3], float reference_white_nits, BYTE bgr[3]) {
    const float white = reference_white_nits / 80.0f;
    float c[3] = {in[0] / white, in[1] / white, in[2] / white};
    const float y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    if (!(y > 0.0f)) {
        bgr[0] = bgr[1] = bgr[2] = 0;
        return;
    }
    // Outside BT.709 (a channel below zero): toward grey of the same
    // luminance, just enough to come inside.
    const float low = std::min({c[0], c[1], c[2]});
    if (low < 0.0f) {
        const float t = y / (y - low);
        for (float &v : c) v = y + (v - y) * t;
    }
    // Above the knee the brightest channel rolls off (its slope stays 1 at
    // the knee) and the others keep their ratio to it.
    const float high = std::max({c[0], c[1], c[2]});
    if (high > kKnee) {
        const float rolled = kKnee + (1.0f - kKnee) * std::tanh((high - kKnee) / (1.0f - kKnee));
        const float scale = rolled / high;
        for (float &v : c) v *= scale;
    }
    bgr[0] = to_byte(srgb_encode(c[2]));
    bgr[1] = to_byte(srgb_encode(c[1]));
    bgr[2] = to_byte(srgb_encode(c[0]));
}

bool save(const Frame &frame, const std::wstring &folder, const std::wstring &stem, std::vector<std::wstring> &files,
          std::string &error) {
    files.clear();
    const size_t n = size_t(frame.width) * frame.height;
    const size_t bytes = frame.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    if (n == 0 || !supported(frame.format) || frame.pixels.size() < n * bytes) {
        error = "nothing to save";
        return false;
    }
    CreateDirectoryW(folder.c_str(), nullptr);
    // A name not in use (two in the same second get _2, _3, ...).
    std::wstring base = folder + L"\\" + stem;
    for (int k = 2; exists(base + L".png") || exists(base + L".jxr"); ++k)
        base = folder + L"\\" + stem + L"_" + std::to_wstring(k);

    const bool hdr = is_hdr(frame);
    const UINT stride = frame.width * 3;
    std::vector<BYTE> sdr(n * 3);
    std::vector<BYTE> half;
    if (hdr) {
        half.resize(n * 8);
        for (size_t i = 0; i < n; ++i) {
            float rgb[3];
            scrgb(frame, i, rgb);
            DirectX::PackedVector::HALF h[4];
            for (int c = 0; c < 3; ++c) h[c] = DirectX::PackedVector::XMConvertFloatToHalf(rgb[c]);
            h[3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
            memcpy(half.data() + i * 8, h, 8);
            tone_map(rgb, kReferenceWhiteNits, sdr.data() + i * 3);
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            const BYTE *px = frame.pixels.data() + i * bytes;
            BYTE *o = sdr.data() + i * 3;
            if (frame.format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                UINT32 v;
                memcpy(&v, px, 4);
                for (int c = 0; c < 3; ++c) o[2 - c] = BYTE((((v >> (10 * c)) & 1023) * 255u + 511u) / 1023u);
            } else if (frame.format == DXGI_FORMAT_B8G8R8A8_UNORM || frame.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                o[0] = px[0], o[1] = px[1], o[2] = px[2];
            } else if (frame.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                // (is_hdr: not reached)
            } else {
                o[0] = px[2], o[1] = px[1], o[2] = px[0];
            }
        }
    }

    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    std::wstring werror;
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> wic;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (FAILED(hr)) werror = hr_text("WIC", hr);
        if (SUCCEEDED(hr) && hdr) {
            hr = write_image(wic.Get(), base + L".jxr", GUID_ContainerFormatWmp, GUID_WICPixelFormat64bppRGBAHalf,
                             frame.width, frame.height, frame.width * 8, half, werror);
            if (SUCCEEDED(hr)) files.push_back(base + L".jxr");
        }
        if (SUCCEEDED(hr)) {
            hr = write_image(wic.Get(), base + L".png", GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR,
                             frame.width, frame.height, stride, sdr, werror);
            if (SUCCEEDED(hr)) files.push_back(base + L".png");
        }
        ok = SUCCEEDED(hr);
    }
    if (com) CoUninitialize();
    if (!ok) error = narrow(werror);
    return ok;
}

bool Grabber::grab(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer, Encoding encoding,
                   const D3D11_BOX &box, std::string &error) {
    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);
    if (!supported(desc.Format) || desc.SampleDesc.Count != 1) {
        error = "the picture's format (" + std::to_string(int(desc.Format)) + ") is not one it can save";
        return false;
    }
    D3D11_TEXTURE2D_DESC copy{};
    copy.Width = box.right - box.left;
    copy.Height = box.bottom - box.top;
    copy.MipLevels = 1;
    copy.ArraySize = 1;
    copy.Format = desc.Format;
    copy.SampleDesc.Count = 1;
    copy.Usage = D3D11_USAGE_STAGING;
    copy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_.Reset();
    const HRESULT hr = device->CreateTexture2D(&copy, nullptr, &staging_);
    if (FAILED(hr)) {
        error = narrow(hr_text("making the copy", hr));
        return false;
    }
    context->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, back_buffer, 0, &box);
    frame_ = Frame{};
    frame_.width = copy.Width;
    frame_.height = copy.Height;
    frame_.format = desc.Format;
    frame_.encoding = encoding;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stem[40];
    swprintf(stem, 40, L"Glasir_%04u-%02u-%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    stem_ = stem;
    tries_ = 0;
    return true;
}

void Grabber::tick(ID3D11DeviceContext *context, void (*done)(bool ok, const std::string &text)) {
    if (!staging_) return;
    D3D11_MAPPED_SUBRESOURCE map{};
    const HRESULT hr = context->Map(staging_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING && ++tries_ < kMaxTries) return;
    if (FAILED(hr)) {
        staging_.Reset();
        done(false, narrow(hr_text("reading the copy back", hr)));
        return;
    }
    const size_t row = size_t(frame_.width) * (frame_.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4);
    frame_.pixels.resize(row * frame_.height);
    for (UINT y = 0; y < frame_.height; ++y)
        memcpy(frame_.pixels.data() + y * row, static_cast<const BYTE *>(map.pData) + size_t(y) * map.RowPitch, row);
    context->Unmap(staging_.Get(), 0);
    staging_.Reset();

    // Converted and written on a thread of its own, which holds a reference
    // to this module so mpv closing meanwhile cannot unload the code under it.
    struct Job {
        Frame frame;
        std::wstring stem;
        void (*done)(bool, const std::string &);
    };
    auto *job = new Job{std::move(frame_), stem_, done};
    frame_ = Frame{};
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&shot::save), &self)) {
        delete job;
        done(false, "could not start saving");
        return;
    }
    auto run = [](LPVOID param) -> DWORD {
        auto *job = static_cast<Job *>(param);
        // Screenshots\ in the app folder (mpv.exe is in engine\).
        wchar_t exe[MAX_PATH] = {}, full[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring folder(exe);
        folder.erase(folder.find_last_of(L"\\/") + 1);
        folder += L"..\\Screenshots";
        if (GetFullPathNameW(folder.c_str(), MAX_PATH, full, nullptr)) folder = full;
        std::vector<std::wstring> files;
        std::string error;
        const bool ok = save(job->frame, folder, job->stem, files, error);
        std::string text;
        if (ok) {
            for (const auto &f : files) text += (text.empty() ? "" : ", ") + narrow(f);
        } else {
            text = error;
        }
        job->done(ok, text);
        delete job;
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&shot::save), &module);
        FreeLibraryAndExitThread(module, 0);
    };
    HANDLE thread = CreateThread(nullptr, 0, run, job, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete job;
        FreeLibrary(self);
        done(false, "could not start saving");
    }
}

}  // namespace glasir::overlay::shot
