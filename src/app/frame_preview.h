// SPDX-License-Identifier: MIT
// Small copies of the frames a frame generation capture saved (F9, Captures\
// <date-time>\*.png: lossless, 10-20 MB each), for a bug report: one JPEG a
// frame, the whole frame at its size, HDR (16-bit PNG = HDR10 PQ) tone-mapped
// to SDR as the screenshot's PNG is (tone_map.h). Enough to see what was on
// screen; measuring with fg_eval needs the lossless frames, which stay where
// they are.
#pragma once
#include <cstdint>
#include <string>

namespace glasir {

struct FramePreviews {
    int frames = 0;
    unsigned width = 0, height = 0;  // of the frames
    bool hdr = false;                // they were HDR, tone-mapped here
    int quality = 0;                 // the JPEGs' quality, 0-100
    uint64_t bytes = 0;              // the JPEGs together
    uint64_t source_bytes = 0;       // the PNGs together
};

// Writes NAME.jpg into folder to (made if needed) for every NAME.png in
// folder from. Their quality is 90, or lower if they would be more than
// max_bytes together. False (and nothing left in to) if a frame cannot be
// read or written, or there are none.
bool write_frame_previews(const std::wstring &from, const std::wstring &to, uint64_t max_bytes, FramePreviews &out,
                          std::wstring &error);

}  // namespace glasir
