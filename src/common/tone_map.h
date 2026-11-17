// SPDX-License-Identifier: MIT
// HDR light to an SDR picture, the way Glasir shows HDR to someone without
// an HDR screen: the screenshot's PNG (src/overlay/screenshot.h) and the
// frame previews in a bug report (src/app/frame_preview.h).
#pragma once
#include <algorithm>
#include <cmath>

namespace glasir::tone {

// The reference (paper) white HDR is tone-mapped from: BT.2408's 203 nits,
// as the menu is drawn in HDR.
constexpr float kReferenceWhiteNits = 203.0f;
// Below this share of the SDR white, tone mapping leaves light as it is;
// above it, the brightest channel rolls off toward 1.0.
constexpr float kKnee = 0.75f;

// SMPTE ST 2084 (PQ): a 0-1 signal to nits.
inline float pq_to_nits(float e) {
    constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
    const float p = std::pow(std::clamp(e, 0.0f, 1.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

// 10-bit PQ code to nits (1024 entries), made once.
inline const float *pq_table() {
    struct Table {
        float nits[1024];
        Table() {
            for (int i = 0; i < 1024; ++i) nits[i] = pq_to_nits(i / 1023.0f);
        }
    };
    static const Table table;
    return table.nits;
}

// 10-bit PQ codes with BT.2020 primaries to linear BT.709 light in scRGB
// units (1.0 = 80 nits; ITU-R BT.2087). Colours outside BT.709 stay as they
// are (negative, or above).
inline void pq2020_to_scrgb(unsigned r10, unsigned g10, unsigned b10, float rgb[3]) {
    const float *nits = pq_table();
    const float r = nits[r10 & 1023], g = nits[g10 & 1023], b = nits[b10 & 1023];
    rgb[0] = (1.660491f * r - 0.587641f * g - 0.072850f * b) / 80.0f;
    rgb[1] = (-0.124550f * r + 1.132900f * g - 0.008349f * b) / 80.0f;
    rgb[2] = (-0.018151f * r - 0.100579f * g + 1.118730f * b) / 80.0f;
}

inline float srgb_encode(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

inline unsigned char to_byte(float v) { return static_cast<unsigned char>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); }

// SDR sRGB (0-255, B G R order) of HDR light in scRGB units: the colours
// outside BT.709 brought in toward grey, then everything above
// reference_white_nits * kKnee rolled off smoothly to the SDR white, on the
// brightest channel so that hues and saturation stay.
inline void to_sdr(const float scrgb[3], float reference_white_nits, unsigned char bgr[3]) {
    const float white = reference_white_nits / 80.0f;
    float c[3] = {scrgb[0] / white, scrgb[1] / white, scrgb[2] / white};
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

}  // namespace glasir::tone
