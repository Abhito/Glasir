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

// The adapter with the primary monitor (mpv's window opens there), or else
// the first hardware adapter.
GpuInfo display_gpu();

// DLSS 5 neural rendering needs an RTX 50 series GPU.
bool is_rtx50(const GpuInfo &gpu);

}  // namespace glasir
