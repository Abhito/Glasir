// SPDX-License-Identifier: MIT
#include "devices.h"

#include <dshow.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <tuple>

#include "capture.h"
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace glasir {
namespace {

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);

std::wstring subtype_name(const GUID &g) {
    PixelFormat f;
    if (format_from_guid(g, f)) return format_name(f);
    if (g == MFVideoFormat_MJPG) return L"MJPG";
    if (g == MFVideoFormat_RGB24) return L"RGB24";
    if (g == MFVideoFormat_RGB32) return L"RGB32";
    if (g == MFVideoFormat_H264) return L"H264";
    if (g == MFVideoFormat_HEVC) return L"HEVC";
    return std::format(L"0x{:08X}", g.Data1);
}

std::wstring get_string(IMFActivate *a, const GUID &key) {
    WCHAR *value = nullptr;
    UINT32 len = 0;
    std::wstring out;
    if (SUCCEEDED(a->GetAllocatedString(key, &value, &len))) {
        out.assign(value, len);
        CoTaskMemFree(value);
    }
    return out;
}

struct ActivateList {
    IMFActivate **items = nullptr;
    UINT32 count = 0;
    ~ActivateList() {
        for (UINT32 i = 0; i < count; ++i) items[i]->Release();
        CoTaskMemFree(items);
    }
};

HRESULT enum_activates(ActivateList &list) {
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 1);
    if (FAILED(hr)) return hr;
    hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) return hr;
    return MFEnumDeviceSources(attrs.Get(), &list.items, &list.count);
}

HRESULT create_reader(IMFActivate *device, ComPtr<IMFMediaSource> &source, ComPtr<IMFSourceReader> &reader) {
    HRESULT hr = device->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr)) return hr;
    ComPtr<IMFAttributes> attrs;
    hr = MFCreateAttributes(&attrs, 2);
    if (SUCCEEDED(hr)) {
        attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        hr = MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), &reader);
    }
    if (FAILED(hr)) {
        source->Shutdown();
        source.Reset();
    }
    return hr;
}

VideoMode read_mode(IMFMediaType *type) {
    VideoMode m;
    GUID sub{};
    UINT32 num = 0, den = 1;
    type->GetGUID(MF_MT_SUBTYPE, &sub);
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &m.width, &m.height);
    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
    m.fps = den ? double(num) / den : 0.0;
    m.supported = format_from_guid(sub, m.format);
    m.subtype = subtype_name(sub);
    return m;
}

bool same_mode(const VideoMode &a, const VideoMode &b) {
    return a.subtype == b.subtype && a.width == b.width && a.height == b.height && std::fabs(a.fps - b.fps) < 0.001;
}

// Lower is better; -1 = not allowed for this request.
int format_rank(const VideoMode &m, const ModeRequest &r) {
    if (r.hdr) return m.supported && m.format == PixelFormat::P010 ? 0 : -1;
    if (m.supported) {
        switch (m.format) {
        case PixelFormat::NV12: return 0;
        case PixelFormat::YUY2: return 1;
        case PixelFormat::UYVY: return 2;
        case PixelFormat::P010: return -1;  // 10-bit HDR mode; SDR uses an 8-bit one
        }
    }
    return r.supported_only ? -1 : 3;
}

}  // namespace

bool looks_like_camera(const std::wstring &name) {
    for (const wchar_t *word : {L"camera", L"webcam", L"virtual", L"facetime"})
        if (name_contains(name, word)) return true;
    return false;
}

bool names_match(const std::wstring &a, const std::wstring &b) {
    return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

bool name_contains(const std::wstring &haystack, const std::wstring &needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i)
        if (CompareStringOrdinal(haystack.c_str() + i, int(needle.size()), needle.c_str(), int(needle.size()),
                                 TRUE) == CSTR_EQUAL)
            return true;
    return false;
}

std::vector<CaptureDevice> list_devices(HRESULT *error) {
    std::vector<CaptureDevice> out;
    ActivateList list;
    HRESULT hr = enum_activates(list);
    if (error) *error = hr;
    if (FAILED(hr)) return out;
    for (UINT32 i = 0; i < list.count; ++i) {
        CaptureDevice d;
        d.name = get_string(list.items[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        d.symbolic_link = get_string(list.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        ComPtr<IMFMediaSource> source;
        ComPtr<IMFSourceReader> reader;
        if (SUCCEEDED(create_reader(list.items[i], source, reader))) {
            d.opened = true;
            for (DWORD t = 0;; ++t) {
                ComPtr<IMFMediaType> type;
                if (FAILED(reader->GetNativeMediaType(kVideoStream, t, &type))) break;
                VideoMode m = read_mode(type.Get());
                if (m.width && m.height && m.fps > 0 &&
                    std::none_of(d.modes.begin(), d.modes.end(), [&](const VideoMode &o) { return same_mode(o, m); }))
                    d.modes.push_back(m);
            }
        }
        list.items[i]->ShutdownObject();
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<std::wstring> list_directshow_devices() {
    std::vector<std::wstring> out;
    ComPtr<ICreateDevEnum> devenum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devenum))))
        return out;
    ComPtr<IEnumMoniker> monikers;
    if (devenum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) != S_OK) return out;
    for (;;) {
        ComPtr<IMoniker> moniker;
        if (monikers->Next(1, &moniker, nullptr) != S_OK) break;
        ComPtr<IPropertyBag> bag;
        if (FAILED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) continue;
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR) out.emplace_back(v.bstrVal);
        VariantClear(&v);
    }
    return out;
}

std::optional<VideoMode> choose_mode(const std::vector<VideoMode> &modes, const ModeRequest &r) {
    const double target = r.fps > 0 ? r.fps : 60.0;
    const bool auto_size = r.width == 0 || r.height == 0;
    auto rate_class = [&](const VideoMode &m) {
        return r.fps > 0 ? std::fabs(m.fps - r.fps) <= 1.0 : m.fps >= 59.0;
    };
    auto fits = [&](const VideoMode &m) { return r.max_auto_height == 0 || m.height <= r.max_auto_height; };
    // Consoles send 16:9; another shape (a card's 21:9 mode) would stretch it.
    auto wide16x9 = [](const VideoMode &m) { return std::abs(int(m.width) * 9 - int(m.height) * 16) <= int(m.height) / 4; };
    // True when a is the better choice.
    auto better = [&](const VideoMode &a, const VideoMode &b) {
        if (rate_class(a) != rate_class(b)) return rate_class(a);
        if (auto_size) {
            if (fits(a) != fits(b)) return fits(a);
            if (wide16x9(a) != wide16x9(b)) return wide16x9(a);
            const UINT64 area_a = UINT64(a.width) * a.height, area_b = UINT64(b.width) * b.height;
            if (area_a != area_b) return fits(a) ? area_a > area_b : area_a < area_b;
        }
        const double da = std::fabs(a.fps - target), db = std::fabs(b.fps - target);
        if (std::fabs(da - db) > 0.001) return da < db;
        if (std::fabs(a.fps - b.fps) > 0.001) return a.fps > b.fps;
        return format_rank(a, r) < format_rank(b, r);
    };
    std::optional<VideoMode> best;
    for (const VideoMode &m : modes) {
        if (format_rank(m, r) < 0 || m.fps <= 0) continue;
        if (!auto_size && (m.width != r.width || m.height != r.height)) continue;
        if (!best || better(m, *best)) best = m;
    }
    return best;
}

const CaptureDevice *choose_device(const std::vector<CaptureDevice> &devices, const std::wstring &name,
                                   const ModeRequest &request) {
    if (!name.empty()) {
        for (const auto &d : devices)
            if (names_match(d.name, name)) return &d;
        for (const auto &d : devices)
            if (name_contains(d.name, name)) return &d;
        return nullptr;
    }
    const CaptureDevice *best = nullptr;
    std::tuple<bool, bool, UINT64> best_score{};
    for (const auto &d : devices) {
        const auto mode = choose_mode(d.modes, request);
        if (!mode) continue;
        // Capture cards over cameras, then a 60-class mode, then size.
        std::tuple<bool, bool, UINT64> score{!looks_like_camera(d.name), mode->fps >= 59.0,
                                             UINT64(mode->width) * mode->height};
        if (!best || score > best_score) {
            best = &d;
            best_score = score;
        }
    }
    return best;
}

HRESULT open_reader(const CaptureDevice &device, const VideoMode &mode, ComPtr<IMFMediaSource> &source,
                    ComPtr<IMFSourceReader> &reader) {
    ActivateList list;
    HRESULT hr = enum_activates(list);
    if (FAILED(hr)) return hr;
    IMFActivate *match = nullptr;
    for (UINT32 i = 0; i < list.count && !match; ++i)
        if (get_string(list.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK) == device.symbolic_link)
            match = list.items[i];
    if (!match) {
        log(L"capture device \"{}\" is gone", device.name);
        return MF_E_NOT_FOUND;
    }
    hr = create_reader(match, source, reader);
    if (FAILED(hr)) {
        log(L"could not open \"{}\" ({})", device.name, hresult_text(hr));
        return hr;
    }
    hr = MF_E_INVALIDMEDIATYPE;
    for (DWORD t = 0;; ++t) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(kVideoStream, t, &type))) break;
        if (!same_mode(read_mode(type.Get()), mode)) continue;
        reader->SetStreamSelection(kAllStreams, FALSE);
        reader->SetStreamSelection(kVideoStream, TRUE);
        hr = reader->SetCurrentMediaType(kVideoStream, nullptr, type.Get());
        if (FAILED(hr))
            log(L"could not select {} {}x{} ({})", mode.subtype, mode.width, mode.height, hresult_text(hr));
        break;
    }
    if (hr == MF_E_INVALIDMEDIATYPE)
        log(L"\"{}\" no longer offers {} {}x{} @ {:.3f}", device.name, mode.subtype, mode.width, mode.height, mode.fps);
    if (FAILED(hr)) {
        // Do not keep the card open behind an error message.
        reader.Reset();
        source->Shutdown();
        source.Reset();
    }
    return hr;
}

}  // namespace glasir
