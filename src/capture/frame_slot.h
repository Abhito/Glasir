// SPDX-License-Identifier: MIT
// The one frame waiting for mpv. Only the newest frame is kept: if mpv falls
// behind, older frames are dropped instead of queueing, so latency cannot
// build up.
#pragma once
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

#include "../common/framegen_link.h"
#include "log.h"

namespace glasir {

inline LONG64 qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

class FrameSlot {
public:
    explicit FrameSlot(size_t bytes) : pending_(bytes), front_(bytes) {}

    // With a link, new frames are stamped into it, and a new frame that is
    // overwritten before mpv took it is counted as dropped.
    void set_link(GlasirFrameGenLink *link) { link_ = link; }

    // Whether the add-on asks for repeats too (frame generation off). Called
    // by the capture thread only; logs when that changes.
    bool repeats_wanted() {
        const bool wanted = link_ && link_->send_repeats != 0;
        if (wanted != repeats_wanted_) {
            repeats_wanted_ = wanted;
            log(L"{}", wanted ? L"frame generation off: repeats are sent too" : L"frame generation on: new frames only");
        }
        return wanted;
    }

    // new_frame: a new picture, not a repeat sent to keep mpv drawing.
    void publish(const BYTE *data, size_t bytes, bool new_frame) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A keepalive repeats the last frame sent; while that frame is
            // still waiting for mpv it adds nothing and must not displace it.
            if (!new_frame && fresh_ && pending_new_) return;
            if (fresh_ && pending_new_) {
                ++dropped_;
                if (link_) InterlockedIncrement64(&link_->dropped);
            }
            if (new_frame) ++published_;
            std::memcpy(pending_.data(), data, std::min(bytes, pending_.size()));
            fresh_ = true;
            pending_new_ = new_frame;
            if (link_ && new_frame) {
                const LONG64 n = link_->published;
                link_->stamps[n % GlasirFrameGenLink::kStamps] = qpc_now();
                InterlockedIncrement64(&link_->published);  // a full barrier: the stamp is visible first
            }
        }
        cv_.notify_one();
    }

    // Returns nullptr on stop or timeout.
    const std::vector<BYTE> *take(DWORD timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return fresh_ || stop_; }))
            return nullptr;
        if (stop_) return nullptr;
        pending_.swap(front_);
        fresh_ = false;
        return &front_;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
    }

    bool stopped() {
        std::lock_guard<std::mutex> lock(mutex_);
        return stop_;
    }

    // New frames so far, and how many of them mpv never got.
    void counts(UINT64 &published, UINT64 &dropped) {
        std::lock_guard<std::mutex> lock(mutex_);
        published = published_;
        dropped = dropped_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<BYTE> pending_;
    std::vector<BYTE> front_;
    bool fresh_ = false;
    bool pending_new_ = false;
    bool stop_ = false;
    bool repeats_wanted_ = false;
    UINT64 published_ = 0, dropped_ = 0;
    GlasirFrameGenLink *link_ = nullptr;
};

}  // namespace glasir
