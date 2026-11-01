// SPDX-License-Identifier: MIT
// The capture formats Glasir passes to mpv as raw frames.
#pragma once
#include <windows.h>
#include <mfapi.h>

#include <cstring>

namespace glasir {

// NV12 and P010: a full-size Y plane, then a half-height plane of interleaved
// U and V (8 bits a sample for NV12, 16 for P010 with the 10 bits on top).
// YUY2 and UYVY: one plane, two bytes a pixel (Y U Y V / U Y V Y).
enum class PixelFormat { NV12, P010, YUY2, UYVY };

inline const wchar_t *format_name(PixelFormat f) {
    switch (f) {
    case PixelFormat::NV12: return L"NV12";
    case PixelFormat::P010: return L"P010";
    case PixelFormat::YUY2: return L"YUY2";
    case PixelFormat::UYVY: return L"UYVY";
    }
    return L"?";
}

// The name mpv's --demuxer-rawvideo-mp-format knows it by.
inline const wchar_t *mpv_format_name(PixelFormat f) {
    switch (f) {
    case PixelFormat::NV12: return L"nv12";
    case PixelFormat::P010: return L"p010";
    case PixelFormat::YUY2: return L"yuyv422";
    case PixelFormat::UYVY: return L"uyvy422";
    }
    return L"";
}

inline GUID format_guid(PixelFormat f) {
    switch (f) {
    case PixelFormat::NV12: return MFVideoFormat_NV12;
    case PixelFormat::P010: return MFVideoFormat_P010;
    case PixelFormat::YUY2: return MFVideoFormat_YUY2;
    case PixelFormat::UYVY: return MFVideoFormat_UYVY;
    }
    return GUID{};
}

inline bool format_from_guid(const GUID &g, PixelFormat &f) {
    for (PixelFormat c : {PixelFormat::NV12, PixelFormat::P010, PixelFormat::YUY2, PixelFormat::UYVY}) {
        if (g == format_guid(c)) {
            f = c;
            return true;
        }
    }
    return false;
}

inline bool is_planar(PixelFormat f) { return f == PixelFormat::NV12 || f == PixelFormat::P010; }

// Bytes in one row of the first (or only) plane.
inline size_t row_bytes(PixelFormat f, UINT32 width) {
    return f == PixelFormat::NV12 ? width : size_t(width) * 2;
}

// Rows of the first plane plus, for the planar formats, the half-height
// second plane, which has the same row size.
inline size_t frame_rows(PixelFormat f, UINT32 height) { return is_planar(f) ? size_t(height) * 3 / 2 : height; }

// A tightly packed frame, as mpv's rawvideo demuxer reads it.
inline size_t frame_size(PixelFormat f, UINT32 width, UINT32 height) {
    return row_bytes(f, width) * frame_rows(f, height);
}

// The luma of pixel i (row * width + column) of a tightly packed frame, as a
// 10-bit code value whatever the format, so one threshold means the same
// everywhere: an 8-bit step is 4.
inline int luma10(PixelFormat f, const BYTE *frame, size_t i) {
    switch (f) {
    case PixelFormat::NV12: return int(frame[i]) << 2;
    case PixelFormat::P010: {
        UINT16 v;
        std::memcpy(&v, frame + i * 2, 2);
        return v >> 6;
    }
    case PixelFormat::YUY2: return int(frame[i * 2]) << 2;
    case PixelFormat::UYVY: return int(frame[i * 2 + 1]) << 2;
    }
    return 0;
}

}  // namespace glasir
