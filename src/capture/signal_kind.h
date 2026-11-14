// SPDX-License-Identifier: MIT
// Tells an SDR signal from an HDR one by looking at a 10-bit capture.
//
// A capture card says nothing about the signal it gets: the same P010 frames
// carry BT.709 SDR or BT.2020 PQ (HDR10), and shown as the other they look
// wrong. The picture itself says which, most of the time:
//
// - SDR puts white near the top of the range (code 940 of 64-940). As PQ that
//   would be 10 000 nits; consoles send a few hundred to a few thousand, so
//   an HDR picture stays far below it.
// - SDR codes colour strongly: the same colour moves the chroma channels
//   three to five times as far from grey as in BT.2020 PQ. This also shows
//   in dark scenes, which have no white.
//
// So a frame with white, or with strong colour, is SDR. A frame that is lit
// with neither is HDR. A black or nearly black frame says nothing, and
// neither does one in between; the answer then stays what it was.
// SignalWatch only changes its answer once one kind of frame has come for a
// while and the other not at all.
//
// Measured (2026-11-14, a console through a Live Gamer Ultra 2.1, the same
// dark cave scene in both modes; "top" is the level the brightest or most coloured 0.1 % of the
// samples reach): HDR luma top 572-576, colour top 60-64; SDR luma top
// 744-760, colour top 280-308. 36 HDR captures of other scenes (after
// DLSS 5): luma top up to 716, colour top up to 186. Nine SDR pictures: luma
// top 911-921, or colour top 263 in the one dark scene.
//
// What it gets wrong: HDR from a console set to a peak of 5000 nits or more,
// in a scene with that much light, reads as SDR; a grey, dim SDR scene
// without any white for five seconds reads as HDR. video.mode = sdr or hdr
// is for those.
#pragma once
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "pixel_format.h"

namespace glasir {

enum class Signal { Unknown, Sdr, Hdr };

inline const wchar_t *signal_name(Signal s) {
    return s == Signal::Sdr ? L"SDR" : s == Signal::Hdr ? L"HDR" : L"unknown";
}

// What one frame's samples look like, in 10-bit codes.
struct SignalStats {
    double mean = 0;     // luma
    int luma_top = 0;    // the luma the brightest 0.1 % of the samples reach
    int colour_top = 0;  // the same for the distance of Cb or Cr from grey (512)
};

// SDR: luma this high is its white (as PQ, about 4700 nits and up) ...
constexpr int kSdrLumaTop = 880;
// ... and chroma this far from grey its colour (in PQ: BT.2020 primaries at
// hundreds of nits, which games do not send).
constexpr int kSdrColourTop = 250;
// HDR: a lit frame whose luma and colour stay below these.
constexpr int kHdrLumaTop = 800;
constexpr int kHdrColourTop = 200;
// Lit: the luma averages this or more. A darker frame (a fade, a loading
// screen) says nothing.
constexpr int kLitMean = 150;

// Measures a tightly packed P010 frame on a sparse grid (every 4th row and
// column of the luma, every 2nd of the half-size chroma).
inline SignalStats measure_signal(const BYTE *frame, UINT32 width, UINT32 height) {
    constexpr int kBins = 256;  // 4 codes each
    std::array<UINT32, kBins> luma{}, colour{};
    UINT64 sum = 0, luma_n = 0, colour_n = 0;
    auto sample = [&](size_t index) {
        UINT16 v;
        std::memcpy(&v, frame + index * 2, 2);
        return int(v >> 6);
    };
    for (UINT32 y = 0; y < height; y += 4) {
        const size_t row = size_t(y) * width;
        for (UINT32 x = 0; x < width; x += 4) {
            const int v = sample(row + x);
            sum += UINT64(v);
            ++luma[size_t(v) >> 2];
            ++luma_n;
        }
    }
    // The second plane: height / 2 rows of width / 2 pairs (U, V).
    const size_t plane = size_t(width) * height;
    for (UINT32 y = 0; y < height / 2; y += 2) {
        const size_t row = plane + size_t(y) * width;
        for (UINT32 x = 0; x + 1 < width; x += 4) {
            const int c = (std::max)(std::abs(sample(row + x) - 512), std::abs(sample(row + x + 1) - 512));
            ++colour[size_t(c) >> 2];
            ++colour_n;
        }
    }
    SignalStats s;
    if (!luma_n || !colour_n) return s;
    auto top = [](const std::array<UINT32, kBins> &bins, UINT64 n) {
        UINT64 above = 0;
        const UINT64 want = (std::max<UINT64>)(1, n / 1000);
        for (int b = kBins - 1; b >= 0; --b) {
            above += bins[size_t(b)];
            if (above >= want) return b * 4;
        }
        return 0;
    };
    s.mean = double(sum) / double(luma_n);
    s.luma_top = top(luma, luma_n);
    s.colour_top = top(colour, colour_n);
    return s;
}

// What one frame says by itself.
inline Signal frame_says(const SignalStats &s) {
    if (s.luma_top >= kSdrLumaTop || s.colour_top >= kSdrColourTop) return Signal::Sdr;
    if (s.mean >= kLitMean && s.luma_top < kHdrLumaTop && s.colour_top < kHdrColourTop) return Signal::Hdr;
    return Signal::Unknown;
}

// Follows the frames and says when the signal is no longer what it is shown
// as. SDR shows itself in most frames and is believed after a second; HDR is
// only the lack of SDR's signs in a lit picture, and is believed after five.
// A frame of the kind shown starts the wait again.
class SignalWatch {
public:
    explicit SignalWatch(Signal shown) : shown_(shown) {}

    // A frame, at now_ms. True when the answer changed (found()).
    bool add(const SignalStats &stats, ULONGLONG now_ms) {
        last_ = stats;
        const Signal says = frame_says(stats);
        ++counts_[size_t(says)];
        if (says == Signal::Unknown) return false;
        if (says == shown_ || says != candidate_) {
            // The signal as shown, or the first frame of the other kind.
            candidate_ = says == shown_ ? Signal::Unknown : says;
            since_ms_ = now_ms;
            run_ = 1;
            return false;
        }
        ++run_;
        const ULONGLONG needed = candidate_ == Signal::Sdr ? kSdrMs : kHdrMs;
        if (run_ < kMinFrames || now_ms - since_ms_ < needed) return false;
        shown_ = candidate_;
        candidate_ = Signal::Unknown;
        return true;
    }

    Signal found() const { return shown_; }
    const SignalStats &last() const { return last_; }
    // Frames that said nothing, SDR and HDR since the last call.
    std::array<UINT32, 3> take_counts() {
        const auto c = counts_;
        counts_ = {};
        return c;
    }

    static constexpr ULONGLONG kSdrMs = 1000, kHdrMs = 5000;
    static constexpr UINT32 kMinFrames = 10;

private:
    Signal shown_;
    Signal candidate_ = Signal::Unknown;
    ULONGLONG since_ms_ = 0;
    UINT32 run_ = 0;
    SignalStats last_;
    std::array<UINT32, 3> counts_{};  // by Signal
};

}  // namespace glasir
