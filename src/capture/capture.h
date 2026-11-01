// SPDX-License-Identifier: MIT
// Reading frames from the card (Media Foundation) or from a raw file or pipe
// (--input-raw, for testing without a card), and handing them to the slot.
#pragma once
#include <windows.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>

#include "devices.h"
#include "frame_slot.h"
#include "repeat_filter.h"

namespace glasir {

// Opens the device and selects exactly this mode. source is kept so it can be
// shut down: that releases the card and cancels a read still waiting.
HRESULT open_reader(const CaptureDevice &device, const VideoMode &mode,
                    Microsoft::WRL::ComPtr<IMFMediaSource> &source,
                    Microsoft::WRL::ComPtr<IMFSourceReader> &reader);

struct FrameLayout {
    PixelFormat format = PixelFormat::NV12;
    UINT32 width = 0, height = 0;
    size_t bytes() const { return frame_size(format, width, height); }
};

// The card is in use by another app: it opens, but the first read fails.
constexpr HRESULT kCardBusy = static_cast<HRESULT>(0xC00D3704);   // MF_E_HW_MFT_FAILED_START_STREAMING
// Another app took the card while Glasir was reading it.
constexpr HRESULT kCardTaken = static_cast<HRESULT>(0xC00D3EA3);  // MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED
// The card's frames come in a layout Glasir cannot pass on (row pitch, size).
constexpr HRESULT kUnreadableFrames = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x201);

inline bool card_in_use(HRESULT hr) { return hr == kCardBusy || hr == kCardTaken; }

// Hands a frame to the slot, minus repeats when deduplicating (repeats is
// nullptr without dedup) - unless the frame generation add-on asks for them.
void deliver_frame(FrameSlot &slot, RepeatFilter *repeats, const BYTE *data, size_t bytes);

// Both run until the source ends or fails, or the slot is stopped, and then
// stop the slot. repeats: dedup, or nullptr to send every frame. status is
// S_OK when stopped, S_FALSE when the source ended, else the error.
void capture_loop(IMFSourceReader *reader, const FrameLayout &layout, FrameSlot &slot, RepeatFilter *repeats,
                  std::atomic<UINT64> &frames, std::atomic<HRESULT> &status);
void raw_loop(HANDLE in, const FrameLayout &layout, FrameSlot &slot, RepeatFilter *repeats,
              std::atomic<UINT64> &frames, std::atomic<HRESULT> &status);

}  // namespace glasir
