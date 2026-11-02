// SPDX-License-Identifier: MIT
#include "gpu.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <regex>

using Microsoft::WRL::ComPtr;

namespace glasir {

GpuInfo display_gpu() {
    GpuInfo first, primary;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        GpuInfo info{desc.Description, desc.VendorId, true};
        if (!first.found) first = info;
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC out{};
            if (SUCCEEDED(output->GetDesc(&out)) && out.Monitor == monitor) primary = info;
        }
    }
    return primary.found ? primary : first;
}

bool is_rtx50(const GpuInfo &gpu) {
    if (gpu.vendor != kVendorNvidia) return false;
    // GeForce RTX 5050 ... 5090, desktop and laptop.
    static const std::wregex rtx50(L"RTX\\s*50[5-9]0", std::regex::icase);
    return std::regex_search(gpu.name, rtx50);
}

const wchar_t *vsr_kind(const GpuInfo &gpu) {
    if (gpu.vendor == kVendorNvidia && gpu.name.find(L"RTX") != std::wstring::npos) return L"nvidia";
    if (gpu.vendor == kVendorIntel) return L"intel";
    return L"none";
}

}  // namespace glasir
