// SPDX-License-Identifier: MIT
// Which GPU drives the display mpv opens on, for the NVIDIA-only features.
#pragma once
#include <string>

namespace glasir {

struct GpuInfo {
    std::wstring name;  // e.g. "NVIDIA GeForce RTX 5070 Ti"
    unsigned vendor = 0;
    bool found = false;
};

constexpr unsigned kVendorNvidia = 0x10DE;
constexpr unsigned kVendorIntel = 0x8086;

// The adapter with the primary monitor (mpv's window opens there), or else
// the first hardware adapter.
GpuInfo display_gpu();

// DLSS 5 neural rendering needs an RTX 50 series GPU.
bool is_rtx50(const GpuInfo &gpu);

// The video super resolution the GPU's video processor may have, as mpv's
// d3d11vpp scaling-mode names it: "nvidia" (RTX), "intel", or "none". The
// driver has the last word: glasir_scaling.lua falls back to FSR 1 if it
// turns it down.
const wchar_t *vsr_kind(const GpuInfo &gpu);

}  // namespace glasir
