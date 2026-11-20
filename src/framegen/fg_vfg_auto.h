// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// fg_vfg_auto.h - which of NVIDIA Video Frame Generation's models to run
// (VfgQuality=auto): the largest that keeps up, chosen again as the load
// changes. NVIDIA's filter has no such thing of its own (its "Mode" is low,
// medium or high, and stays what it is set to); this is the add-on's, and
// only arithmetic - no Direct3D, nothing of NVIDIA's - so that it can be
// tested by itself (tests/test_vfg_auto.cpp).
//
// What it goes by, once for each real frame:
//   busy      how long the pair took, from its real frame's arrival at Glasir
//             until its frames in between were done. Within the source's
//             interval, the next frame finds the GPU free; beyond it, frames
//             pile up;
//   strained  a frame in between was left out because the screen was behind.
//
// A smaller model is taken when it does not keep up for a few frames nearly
// in a row (one late frame is nothing: DLSS 5 starting, a shader being
// compiled) or has lately been taking nearly all of the interval, and at once
// when mpv drops frames while generating. A larger one is taken when there
// has been time to spare for a while: by what that model took here before,
// when it has run, else only with most of the interval free - trying it costs
// a moment's stutter when it does not fit, and loading it a fraction of a
// second the first time.
//
// What a model takes is learned from its steady frames only, and it is
// remembered as too large only when even its quickest recent frames took
// nearly the whole interval - not after a hitch, where the frames before it
// were quick. One stepped down from is left alone for a while either way,
// longer each time it fails again right after it was taken. What is known is
// believed for a quarter of an hour; after that a model is given one more
// try. It is for one picture size and for DLSS 5 on or off: reset() and the
// `neural` flag say when those change.

#pragma once
#include <algorithm>

namespace fg {

enum class VfgMode { low = 0, medium = 1, high = 2 };  // the filter's "Mode": its model's size

class VfgAuto {
public:
    enum class Why { none, behind, dropped, room };

    // From here on with this model and nothing learned (a new picture size, or the choice just made).
    void reset(VfgMode start) {
        *this = VfgAuto{};
        mode_ = int(start);
    }

    VfgMode mode() const { return VfgMode(mode_); }
    // A smaller model there is to step down to (one that was not found impossible to make).
    bool has_smaller() const {
        for (int m = mode_ - 1; m >= 0; --m)
            if (!unavailable_[m]) return true;
        return false;
    }
    Why why() const { return why_; }

    // The model mode() names could not be made: `running` goes on, and that one is not tried again.
    void failed(VfgMode running) {
        if (mode_ >= 0 && mode_ < 3) unavailable_[mode_] = true;
        mode_ = int(running);
        samples_ = 0;
    }

    // One real frame while the filter makes the frames in between. now, interval (the source's) and busy
    // in seconds; busy < 0: the pair before had no frame in between. strained: this one is left without
    // its frame because the screen is behind. A frame with neither says nothing (generation paused,
    // settling, or the display too slow for it). True: mode() is another now.
    bool frame(double now, double interval, double busy, bool strained, bool neural) {
        if (interval <= 0) return false;
        follow(now, neural);
        if (busy < 0 && !strained) return false;
        if (busy >= 0) {
            // One long frame (a stall of mpv's own) counts as late, not as what the model takes.
            const double took = std::min(busy, 1.5 * interval);
            busy_ = samples_ == 0 ? took : busy_ * 0.9 + took * 0.1;
            recent_[samples_ % kRecent] = took;  // this model's own: samples_ starts at 0 with it
            ++samples_;
        }
        // Over: left out, later than the next frame, or lately so close to it that it will be.
        const bool over = strained || (busy >= 0 && busy > interval) || (samples_ >= kTell && busy_ > kTight * interval);
        // What this model takes here, from enough steady frames.
        if (!over && busy >= 0 && samples_ >= kLearn) known_[mode_][neural_ > 0 ? 1 : 0] = Known{busy_, now};
        if (over) {
            // At most what steps down: a long overload at the smallest model is forgotten as fast as a short one.
            strain_ = std::min(strain_ + 3, kStrain);
            last_over_ = now;
        } else if (strain_ > 0) {
            --strain_;
        }
        if (strain_ >= kStrain && mode_ > 0) return lower(now, interval, Why::behind);
        if (strain_ == 0 && mode_ < 2 && fits_larger(now, interval)) {
            ++mode_;
            raised_at_ = changed_at_ = now;
            samples_ = 0;
            why_ = Why::room;
            return true;
        }
        return false;
    }

    // mpv dropped new frames while generating. True: a smaller model is taken (mode() is another now).
    // False: there is none smaller - the caller pauses generation as it does without this.
    bool dropped(double now, double interval, bool neural) {
        if (interval <= 0) return false;
        follow(now, neural);
        last_over_ = now;
        return mode_ > 0 && lower(now, interval, Why::dropped);
    }

private:
    static constexpr int kStrain = 18;   // six frames that did not keep up, nearly in a row
    static constexpr int kLearn = 30;    // frames before a model's time is known
    static constexpr int kSteady = 90;   // frames at a model before a larger one is thought of
    static constexpr double kQuiet = 5.0;        // seconds without a frame over, and since the last change
    static constexpr int kTell = 10;     // frames before the time lately says anything
    static constexpr int kRecent = 8;    // frames whose quickest says whether a model is too large
    static constexpr double kFits = 0.8;         // of the interval: a model known to take this much fits
    static constexpr double kTight = 0.9;        // of the interval: one that takes this much lately does not
    static constexpr double kRoom = 0.4;         // of the interval busy at most, to try one not known here
    static constexpr double kRemember = 900.0;   // seconds a model's time is believed
    static constexpr double kWait = 60.0, kWaitMost = 900.0;  // before one that failed is tried again

    struct Known {
        double busy = -1, at = 0;
    };

    // DLSS 5 switched on or off: another load altogether.
    void follow(double now, bool neural) {
        const int n = neural ? 1 : 0;
        if (n == neural_) return;
        neural_ = n;
        samples_ = 0;
        strain_ = 0;
        changed_at_ = now;
        for (double &until : blocked_until_) until = 0;
    }

    // A smaller model that can be made; false where there is none (the caller pauses instead).
    bool lower(double now, double interval, Why why) {
        int to = mode_ - 1;
        while (to >= 0 && unavailable_[to]) --to;
        if (to < 0) return false;
        // Taken a moment ago and already too much: longer before the next try.
        const bool at_once = now - raised_at_ < 20.0;
        wait_[mode_] = at_once ? std::min(wait_[mode_] * 2.0, kWaitMost) : kWait;
        blocked_until_[mode_] = now + wait_[mode_];
        blocked_interval_[mode_] = interval;
        // Too large here when even its quickest recent frames took nearly the whole interval. After a
        // hitch the frames before it were quick: the wait above is all there is then.
        const int n = std::min(samples_, kRecent);
        if (n > 0) {
            double quickest = recent_[0];
            for (int i = 1; i < n; ++i) quickest = std::min(quickest, recent_[i]);
            if (quickest > kTight * interval) known_[mode_][neural_ > 0 ? 1 : 0] = Known{quickest, now};
        }
        mode_ = to;
        changed_at_ = now;
        strain_ = 0;
        samples_ = 0;
        why_ = why;
        return true;
    }

    bool fits_larger(double now, double interval) const {
        const int up = mode_ + 1;
        if (unavailable_[up] || samples_ < kSteady) return false;
        if (now - changed_at_ < kQuiet || now - last_over_ < kQuiet) return false;
        // Left alone after it failed - unless the source has slowed down since (a 30 fps game after a 60 fps one).
        if (now < blocked_until_[up] && interval < 1.3 * blocked_interval_[up]) return false;
        const Known &k = known_[up][neural_ > 0 ? 1 : 0];
        // Known here lately: by that. Known long ago: one more try (the load may have changed since).
        if (k.busy >= 0) return now - k.at >= kRemember || std::max(k.busy, busy_) <= kFits * interval;
        return busy_ <= kRoom * interval;
    }

    int mode_ = 1;
    int neural_ = -1;       // 1: ReShade's effects (DLSS 5) run; -1: not known yet
    double busy_ = 0;       // how long a pair takes with this model, lately
    int samples_ = 0;       // pairs counted into busy_
    double recent_[kRecent] = {};  // the last pairs' times (as many as samples_, up to kRecent)
    int strain_ = 0;
    double last_over_ = -1e9, changed_at_ = -1e9, raised_at_ = -1e9;
    Known known_[3][2];
    double blocked_until_[3] = {}, blocked_interval_[3] = {};
    double wait_[3] = {kWait / 2, kWait / 2, kWait / 2};  // doubled before its first use
    bool unavailable_[3] = {};
    Why why_ = Why::none;
};

}  // namespace fg
