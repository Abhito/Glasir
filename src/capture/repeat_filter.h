// SPDX-License-Identifier: MIT
// Dedup: tells new frames from repeats.
//
// A 30 fps game captured at 60 arrives as A A B B; with dedup mpv only gets
// A B, so DLSS 5 runs on real frames only and frame generation sees an even
// 30 fps instead of pairs of identical frames.
//
// A sparse grid of luma samples (every 4th row, every 2nd column) is compared
// with the same grid of the last frame sent, not the last frame captured, so a
// slow fade still goes out once it adds up. Samples are 10-bit code values
// for every format (pixel_format.h), so the threshold means the same.
//
// Some sources are not bit-exact on repeats: the PS5 adds faint noise to
// every refresh (a quarter of the samples move by 1-3 8-bit steps, never in
// the same direction across an area), so one sample moving a little says
// nothing. Two things count as a change instead: a 16x16 pixel block whose
// average moved by more than the threshold (the noise cancels out in it,
// while a fade, a pan or a moving shape moves it) and one sample that moved by
// more than kSpotFactor times the threshold (a small sharp change, like a HUD
// digit, that a block average would hide). A frame is new from kMinChanged
// changes.
//
// A repeat is still sent after keepalive_ms, so a still picture keeps mpv's
// OSD and menus drawing.
#pragma once
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include "log.h"
#include "pixel_format.h"

namespace glasir {

class RepeatFilter {
public:
    RepeatFilter(PixelFormat format, UINT32 width, UINT32 height, int threshold, DWORD keepalive_ms)
        : format_(format), width_(width), threshold_(threshold), keepalive_ms_(keepalive_ms),
          grid_w_((width + kColStep - 1) / kColStep), grid_h_((height + kRowStep - 1) / kRowStep),
          blocks_w_((grid_w_ + kBlockCols - 1) / kBlockCols), blocks_h_((grid_h_ + kBlockRows - 1) / kBlockRows),
          sent_(size_t(grid_w_) * grid_h_), scratch_(sent_.size()), block_sum_(size_t(blocks_w_) * blocks_h_),
          block_samples_(block_sum_.size()) {
        for (UINT32 gy = 0; gy < grid_h_; ++gy)
            for (UINT32 gx = 0; gx < grid_w_; ++gx)
                ++block_samples_[size_t(gy / kBlockRows) * blocks_w_ + gx / kBlockCols];
    }

    // True when the frame (tightly packed) should be sent.
    bool take(const BYTE *frame) {
        std::fill(block_sum_.begin(), block_sum_.end(), 0);
        const int spot = threshold_ * kSpotFactor;
        size_t changed = 0, i = 0;
        int sample_max = 0;
        for (UINT32 gy = 0; gy < grid_h_; ++gy) {
            const size_t row = size_t(gy) * kRowStep * width_;
            int *sums = block_sum_.data() + size_t(gy / kBlockRows) * blocks_w_;
            for (UINT32 gx = 0; gx < grid_w_; ++gx, ++i) {
                const int v = luma10(format_, frame, row + size_t(gx) * kColStep);
                scratch_[i] = static_cast<UINT16>(v);
                const int d = v - int(sent_[i]);
                sums[gx / kBlockCols] += d;
                const int a = std::abs(d);
                sample_max = std::max(sample_max, a);
                if (a > spot) ++changed;
            }
        }
        double block_max = 0.0;
        for (size_t b = 0; b < block_sum_.size(); ++b) {
            const double mean = std::abs(block_sum_[b]) / double(block_samples_[b]);
            block_max = std::max(block_max, mean);
            if (mean > threshold_) ++changed;
        }
        const ULONGLONG now = GetTickCount64();
        const bool fresh = !primed_ || changed >= kMinChanged;
        keepalive_ = !fresh && now - last_sent_ms_ >= keepalive_ms_;
        count(fresh, changed, sample_max, block_max);
        if (!fresh && !keepalive_) return false;
        sent_.swap(scratch_);
        primed_ = true;
        last_sent_ms_ = now;
        return true;
    }

    // Whether the last frame take() let through was a repeat sent only to keep
    // mpv drawing.
    bool keepalive() const { return keepalive_; }

private:
    static constexpr UINT32 kRowStep = 4;
    static constexpr UINT32 kColStep = 2;
    // A block of 4 grid rows by 8 grid columns: 16x16 pixels, 32 samples.
    static constexpr UINT32 kBlockRows = 4;
    static constexpr UINT32 kBlockCols = 8;
    static constexpr int kSpotFactor = 8;
    static constexpr size_t kMinChanged = 4;

    // Logs the rate of new frames whenever it settles somewhere else, so the
    // log shows e.g. "30.0 new of 60.0 captured" for a 30 fps game, with how
    // many changes the repeats had at most and the new frames at least, and
    // how far the repeats' samples and block averages moved (the source's
    // noise). Repeats near the line, or noise near the threshold, mean a
    // threshold to raise.
    void count(bool fresh, size_t changed, int sample_max, double block_max) {
        const auto now = std::chrono::steady_clock::now();
        if (window_frames_ == 0) {
            window_start_ = now;
            repeat_max_ = 0;
            new_min_ = SIZE_MAX;
            noise_sample_ = 0;
            noise_block_ = 0.0;
        }
        ++window_frames_;
        if (fresh) {
            ++window_new_;
            if (primed_) new_min_ = std::min(new_min_, changed);
        } else {
            repeat_max_ = std::max(repeat_max_, changed);
            // Only repeats without a change: one with a small real change
            // under the line (a blinking dot) would read as huge noise.
            if (changed == 0) {
                noise_sample_ = std::max(noise_sample_, sample_max);
                noise_block_ = std::max(noise_block_, block_max);
            }
        }
        const double seconds = std::chrono::duration<double>(now - window_start_).count();
        if (seconds < 2.0) return;
        const double rate = window_new_ / seconds;
        if (std::fabs(rate - logged_rate_) >= 3.0) {
            log(L"dedup: {:.1f} new frames/s of {:.1f} captured (changes: repeats up to {}, new frames from {}, "
                L"new from {}; repeats' noise up to {} a sample, {:.1f} a block; threshold {})",
                rate, window_frames_ / seconds, repeat_max_, new_min_ == SIZE_MAX ? size_t(0) : new_min_,
                kMinChanged, noise_sample_, noise_block_, threshold_);
            logged_rate_ = rate;
        }
        window_frames_ = window_new_ = 0;
    }

    PixelFormat format_;
    UINT32 width_;
    int threshold_;
    DWORD keepalive_ms_;
    UINT32 grid_w_, grid_h_, blocks_w_, blocks_h_;
    std::vector<UINT16> sent_, scratch_;
    std::vector<int> block_sum_, block_samples_;
    bool primed_ = false;
    bool keepalive_ = false;
    ULONGLONG last_sent_ms_ = 0;
    std::chrono::steady_clock::time_point window_start_;
    UINT64 window_frames_ = 0, window_new_ = 0;
    size_t repeat_max_ = 0, new_min_ = SIZE_MAX;
    int noise_sample_ = 0;
    double noise_block_ = 0.0;
    double logged_rate_ = -100.0;
};

}  // namespace glasir
