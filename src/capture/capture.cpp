// SPDX-License-Identifier: MIT
#include "capture.h"

#include <mfapi.h>
#include <mferror.h>

#include <cstring>
#include <vector>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace glasir {
namespace {

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

// Copies rows that are pitch bytes apart into a tightly packed frame. The
// second plane of NV12 and P010 follows the first with the same pitch.
void pack_rows(const BYTE *src, LONG pitch, const FrameLayout &layout, std::vector<BYTE> &packed) {
    const size_t row = row_bytes(layout.format, layout.width);
    const size_t rows = frame_rows(layout.format, layout.height);
    packed.resize(row * rows);
    for (size_t r = 0; r < rows; ++r) std::memcpy(packed.data() + r * row, src + r * size_t(pitch), row);
}

}  // namespace

// Hands a frame to mpv's slot, minus repeats when deduplicating - unless the
// add-on asks for them: then a repeat goes out as one (a keepalive does the
// same), which never displaces a new frame mpv has not taken yet.
void deliver_frame(FrameSlot &slot, const FrameSinks &sinks, const BYTE *data, size_t bytes) {
    RepeatFilter *repeats = sinks.repeats;
    if (!repeats) {
        if (sinks.signal) sinks.signal->frame(data);
        slot.publish(data, bytes, true);
        return;
    }
    const bool all = slot.repeats_wanted();
    if (repeats->take(data)) {
        // New pictures, and a still one now and then (the keepalive).
        if (sinks.signal) sinks.signal->frame(data);
        slot.publish(data, bytes, !repeats->keepalive());
    } else if (all) {
        slot.publish(data, bytes, false);
    }
}

SignalMonitor::SignalMonitor(Signal shown, UINT32 width, UINT32 height)
    : found(shown), watch_(shown), width_(width), height_(height) {
    wchar_t value[4] = {};
    verbose_ = GetEnvironmentVariableW(L"GLASIR_SIGNAL_LOG", value, 4) > 0 && value[0] == L'1';
}

void SignalMonitor::frame(const BYTE *data) {
    const ULONGLONG now = GetTickCount64();
    if (now - measured_ms_ < 30) return;  // some thirty frames a second are plenty
    measured_ms_ = now;
    if (watch_.add(measure_signal(data, width_, height_), now)) {
        log(L"signal: it is {} now", signal_name(watch_.found()));
        found = watch_.found();
    }
    if (!window_ms_) window_ms_ = now;
    if (now - window_ms_ < 2000) return;
    window_ms_ = now;
    const auto counts = watch_.take_counts();
    const UINT32 sdr = counts[size_t(Signal::Sdr)], hdr = counts[size_t(Signal::Hdr)];
    const Signal says = sdr > hdr ? Signal::Sdr : hdr > sdr ? Signal::Hdr : Signal::Unknown;
    if (!verbose_ && logged_once_ && says == logged_ && now - logged_ms_ < 60000) return;
    logged_once_ = true;
    logged_ = says;
    logged_ms_ = now;
    const SignalStats &s = watch_.last();
    log(L"signal: shown as {}; the last 2 s' frames say HDR {}, SDR {}, nothing {} (last frame: luma mean {:.0f}, "
        L"top {}; colour top {})",
        signal_name(watch_.found()), hdr, sdr, counts[size_t(Signal::Unknown)], s.mean, s.luma_top, s.colour_top);
}

void raw_loop(HANDLE in, const FrameLayout &layout, FrameSlot &slot, const FrameSinks &sinks,
              std::atomic<UINT64> &frames, std::atomic<HRESULT> &status) {
    status = S_OK;
    const size_t frame_bytes = layout.bytes();
    std::vector<BYTE> frame(frame_bytes);
    while (!slot.stopped()) {
        size_t got = 0;
        while (got < frame_bytes) {
            DWORD n = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(frame_bytes - got, 4u << 20));
            if (!ReadFile(in, frame.data() + got, want, &n, nullptr) || n == 0) {
                log(L"raw input ended after {} frames", frames.load());
                if (!slot.stopped()) status = S_FALSE;
                slot.stop();
                return;
            }
            got += n;
        }
        deliver_frame(slot, sinks, frame.data(), frame_bytes);
        ++frames;
    }
}

void capture_loop(IMFSourceReader *reader, const FrameLayout &layout, FrameSlot &slot, const FrameSinks &sinks,
                  std::atomic<UINT64> &frames, std::atomic<HRESULT> &status) {
    status = S_OK;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const size_t frame_bytes = layout.bytes();
    const LONG packed_pitch = static_cast<LONG>(row_bytes(layout.format, layout.width));
    std::vector<BYTE> packed;
    bool logged_pitch = false;
    // A frame Glasir cannot pass on ends capture with kUnreadableFrames, so
    // the user is told, instead of an empty window that never fills.
    auto unreadable = [&](const std::wstring &why) {
        log(L"unreadable frames: {}", why);
        if (!slot.stopped()) status = kUnreadableFrames;
    };
    while (!slot.stopped()) {
        DWORD index = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(kVideoStream, 0, &index, &flags, &ts, &sample);
        if (FAILED(hr)) {
            if (slot.stopped()) break;  // shut down on purpose
            if (card_in_use(hr))
                log(L"the card is in use by another app ({})", hresult_text(hr));
            else
                log(L"ReadSample failed {}", hresult_text(hr));
            status = hr;
            break;
        }
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
            log(L"capture stream ended (flags 0x{:X})", flags);
            if (!slot.stopped()) status = (flags & MF_SOURCE_READERF_ERROR) ? E_FAIL : S_FALSE;
            break;
        }
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;

        // A 2D buffer tells its row pitch, which can be wider than the
        // picture (padding); mpv needs tightly packed rows.
        ComPtr<IMF2DBuffer> buffer2d;
        BYTE *scan0 = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
            bool ok = true;
            if (pitch == packed_pitch) {
                deliver_frame(slot, sinks, scan0, frame_bytes);
                ++frames;
            } else if (pitch > packed_pitch) {
                if (!logged_pitch) {
                    log(L"rows are {} bytes apart (picture {}): repacking", pitch, packed_pitch);
                    logged_pitch = true;
                }
                pack_rows(scan0, pitch, layout, packed);
                deliver_frame(slot, sinks, packed.data(), frame_bytes);
                ++frames;
            } else {
                // Negative (bottom-up) or narrower than the picture.
                unreadable(std::format(L"row pitch {} for {}-byte rows", pitch, packed_pitch));
                ok = false;
            }
            buffer2d->Unlock2D();
            if (!ok) break;
            continue;
        }

        BYTE *data = nullptr;
        DWORD len = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &len))) continue;
        const bool whole = len >= frame_bytes;
        if (whole) {
            deliver_frame(slot, sinks, data, frame_bytes);
            ++frames;
        }
        buffer->Unlock();
        if (!whole) {
            unreadable(std::format(L"{}-byte frames, expected {}", len, frame_bytes));
            break;
        }
    }
    slot.stop();
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace glasir
