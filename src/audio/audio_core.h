// SPDX-License-Identifier: MIT
// Sample conversion and the bounded, resampling sample queue of Glasir's
// audio plugin.
//
// Based on native-audio/audio_core.h of the WASAPI audio plugin by z-er
// (Josh S) for MPV-SW-Capture, https://github.com/z-er/MPV-SW-Capture,
// MIT licence, Copyright (c) 2026 TyRaS-SW. Modified for Glasir.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace glasir::audio {

struct Stereo {
    float left = 0, right = 0;
};

// One sample of a PCM (16/24/32-bit) or float32 stream, as -1..1.
inline float decode(const unsigned char *p, unsigned bits, bool floating) {
    if (floating) {
        float v;
        std::memcpy(&v, p, 4);
        return std::isfinite(v) ? v : 0;
    }
    if (bits == 16) {
        int16_t v;
        std::memcpy(&v, p, 2);
        return v / 32768.0f;
    }
    if (bits == 24) {
        int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
        if (v & 0x800000) v -= 0x1000000;
        return v / 8388608.0f;
    }
    int32_t v;
    std::memcpy(&v, p, 4);
    return float(v / 2147483648.0);
}

// Writes v (clipped to -1..1) as a PCM or float32 sample.
inline void encode(unsigned char *p, float v, unsigned bits, bool floating) {
    v = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0;
    if (floating) {
        std::memcpy(p, &v, 4);
        return;
    }
    if (bits == 16) {
        const int16_t n = int16_t(std::clamp(std::llround(v * 32768.0), -32768LL, 32767LL));
        std::memcpy(p, &n, 2);
    } else if (bits == 24) {
        const int32_t n = int32_t(std::clamp(std::llround(v * 8388608.0), -8388608LL, 8388607LL));
        p[0] = n & 255;
        p[1] = (n >> 8) & 255;
        p[2] = (n >> 16) & 255;
    } else {
        const int32_t n = int32_t(std::clamp(std::llround(v * 2147483648.0), -2147483648LL, 2147483647LL));
        std::memcpy(p, &n, 4);
    }
}

// Owned by the one audio thread. No allocations in push or read.
class Ring {
public:
    explicit Ring(size_t capacity) : data_(std::max(size_t(2), capacity)) {}

    size_t size() const { return count_; }
    size_t capacity() const { return data_.size(); }

    // Drops the oldest samples down to keep (bounds latency after a stall,
    // or when the audio delay is lowered).
    void trim(size_t keep) {
        if (count_ > keep) {
            const size_t n = count_ - keep;
            head_ = (head_ + n) % data_.size();
            count_ -= n;
            dropped += n;
            phase_ = 0;
        }
    }

    void push(Stereo v) {
        if (count_ == data_.size()) trim(count_ - 1);
        data_[(head_ + count_) % data_.size()] = v;
        ++count_;
    }

    // The next output sample, linearly interpolated, advancing by step
    // input samples (input rate / output rate). False when too few are queued.
    bool read(double step, Stereo &v) {
        if (!(step > 0) || !std::isfinite(step)) return false;
        const size_t advance = size_t(phase_ + step);
        if (count_ < 2 || advance >= count_) return false;
        const Stereo a = data_[head_], b = data_[(head_ + 1) % data_.size()];
        v = {float(a.left + (b.left - a.left) * phase_), float(a.right + (b.right - a.right) * phase_)};
        phase_ += step;
        phase_ -= advance;
        head_ = (head_ + advance) % data_.size();
        count_ -= advance;
        return true;
    }

    uint64_t dropped = 0;

private:
    std::vector<Stereo> data_;
    size_t head_ = 0, count_ = 0;
    double phase_ = 0;
};

// Samples to keep queued: what the two devices need to run without gaps
// (base) plus the audio delay the user asked for.
inline size_t queue_target(double input_rate, size_t base, int delay_ms) {
    return base + size_t(std::max(0, delay_ms) * input_rate / 1000.0);
}

// Volume 0-100 %, boost 100-400 %, mute.
inline double output_gain(double volume, int boost_percent, bool muted) {
    if (muted) return 0;
    return std::clamp(volume, 0.0, 100.0) / 100.0 * std::clamp(boost_percent, 100, 400) / 100.0;
}

}  // namespace glasir::audio
