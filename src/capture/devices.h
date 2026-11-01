// SPDX-License-Identifier: MIT
// Finding capture cards and picking their mode.
#pragma once
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "pixel_format.h"

namespace glasir {

// A mode a card offers. Formats Glasir cannot pass on raw (MJPEG, RGB, ...)
// are kept too, with supported = false, for the list and the DirectShow
// fallback.
struct VideoMode {
    PixelFormat format = PixelFormat::NV12;
    bool supported = false;
    std::wstring subtype;  // "NV12", "MJPG", ...
    UINT32 width = 0, height = 0;
    double fps = 0;
};

struct CaptureDevice {
    std::wstring name;
    std::wstring symbolic_link;  // tells two cards with the same name apart
    std::vector<VideoMode> modes;
    bool opened = false;  // its modes could be read
};

// Media Foundation video capture devices with their modes. Each device is
// opened briefly to read them.
std::vector<CaptureDevice> list_devices(HRESULT *error = nullptr);

// DirectShow video capture devices by name. Includes devices Media
// Foundation does not see (some older or virtual ones).
std::vector<std::wstring> list_directshow_devices();

struct ModeRequest {
    bool hdr = false;
    UINT32 width = 0, height = 0;  // 0 = automatic
    double fps = 0;                // 0 = 60 if offered
    UINT32 max_auto_height = 0;    // automatic size: prefer modes up to this (the display), 0 = any
    bool supported_only = true;    // false: any format (DirectShow fallback)
};

// The best mode for the request, or none. HDR needs P010; SDR takes NV12,
// then YUY2, then UYVY. Automatic picks a 60-class rate first, then the
// largest 16:9 size that fits the display, then the rate nearest 60.
std::optional<VideoMode> choose_mode(const std::vector<VideoMode> &modes, const ModeRequest &request);

// The device named (exact, then a case-insensitive substring), or with an
// empty name the device most like a capture card that can serve the request.
const CaptureDevice *choose_device(const std::vector<CaptureDevice> &devices, const std::wstring &name,
                                   const ModeRequest &request);

// Webcams and virtual cameras, which auto-pick avoids.
bool looks_like_camera(const std::wstring &name);

bool names_match(const std::wstring &a, const std::wstring &b);
bool name_contains(const std::wstring &haystack, const std::wstring &needle);

}  // namespace glasir
