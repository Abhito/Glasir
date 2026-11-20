// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// fg_nvfx.h - what the effects of NVIDIA's Video Effects SDK that Glasir uses
// have in common: Video Frame Generation (fg_vfg.h) and Video Super
// Resolution (fg_sr.h).
//
// Nothing of NVIDIA's is built in: the SDK's DLLs are loaded at run time from
// a folder on the user's PC (NVVideoEffects.dll, NVCVImage.dll, an effect's
// own DLL and what they need), and CUDA from the driver's nvcuda.dll. Only
// their public C functions are called, by name.
//
// An effect reads and writes rows of packed pixels in CUDA memory, and CUDA
// shares a Direct3D 11 buffer as such memory (a 10-bit texture it cannot
// share at all). So a frame goes: a shader packs it into a buffer, 32 bits a
// pixel (8-bit RGBA, or R10 G10 B10 A2), CUDA maps the buffers as its own
// memory, the effect runs, they are handed back, and a shader draws the
// buffer the effect wrote (vfg.hlsl has both shaders). Mapping waits for
// Direct3D's work on the buffers and unmapping for the effect's.
//
// NVIDIA's licence does not allow giving results of its software (speed,
// quality, comparisons) to others without its permission: nothing here logs
// a timing.

#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>

// vfg.hlsl's two shaders, compiled (the headers have no guard of their own).
#include "vfg_pack_cs.h"
#include "vfg_unpack_ps.h"

namespace fg {

using Microsoft::WRL::ComPtr;

class NvFx {
public:
    // "1.3.0", once the SDK's DLLs are loaded.
    const std::string &version() const { return version_; }

    // Whether frames of this format can go to an effect, and as 10-bit.
    static bool takes(DXGI_FORMAT format, bool &ten_bit) {
        switch (format) {
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            ten_bit = true;
            return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            ten_bit = false;
            return true;
        default:
            return false;  // sRGB views read linear values, scRGB goes past 1
        }
    }

protected:
    // What every effect loads, before its own DLL (in this order: each finds the others in the folder).
    static constexpr const wchar_t *kCommon[4] = {L"cudart64_12.dll", L"NVCVImage.dll", L"nvngxruntime.dll",
                                                  L"NVVideoEffects.dll"};
    // NvCVImage's names for the two pixel layouts (nvCVImage.h; RGB10A2 and
    // P32 came after the header NVIDIA publishes) and for where pixels are.
    static constexpr int kRgba = 6, kRgb10a2 = 13, kU8 = 1, kP32 = 11;
    static constexpr unsigned kChunky = 0, kCpu = 0, kGpu = 1;
    static constexpr int kComputeMajor = 75, kComputeMinor = 76;  // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_*
    static constexpr unsigned kMapReadOnly = 1, kMapWriteDiscard = 2;

    // One buffer of packed pixels, width x height: Direct3D's views of it,
    // CUDA's handle to it and the SDK's description of it.
    struct Shared {
        ComPtr<ID3D11Buffer> buf;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        UINT width = 0, height = 0;
        void *resource = nullptr;
        void *image = nullptr;
        unsigned long long pointer = 0;  // where CUDA had it at the last mapping
    };

    // The CUDA context on this thread for as long as it lives.
    struct Current {
        NvFx &v;
        bool ok;
        explicit Current(NvFx &fx) : v(fx), ok(fx.cu_push_(fx.context_) == 0) {}
        ~Current() {
            void *was = nullptr;
            if (ok) v.cu_pop_(&was);
        }
    };

    static std::string hex(HRESULT hr) {
        char text[16];
        snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
        return text;
    }

    bool cuda(int code, const char *what, std::string &error) {
        if (code == 0) return true;
        error = std::string(what) + ": CUDA error " + std::to_string(code);
        return false;
    }

    bool nv(int code, const char *what, std::string &error) {
        if (code == 0) return true;
        const char *text = error_string_ ? error_string_(code) : nullptr;
        error = std::string(what) + ": " + (text ? text : "error") + " (" + std::to_string(code) + ")";
        return false;
    }

    template <typename T> static bool bind(HMODULE module, const char *name, T &fn) {
        fn = reinterpret_cast<T>(GetProcAddress(module, name));
        return fn != nullptr;
    }

    // kCommon and these files are in the folder.
    static bool files_present(const std::wstring &folder, const wchar_t *const *own, size_t count) {
        for (const wchar_t *name : kCommon)
            if (GetFileAttributesW((folder + L"\\" + name).c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        for (size_t i = 0; i < count; ++i)
            if (GetFileAttributesW((folder + L"\\" + own[i]).c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        return true;
    }

    // The SDK's DLLs - kCommon, then the effect's own - and the driver's
    // CUDA, once for this object. missing: what to say when the files are
    // not in the folder.
    bool load(const std::wstring &runtime, const wchar_t *const *own, size_t count, const char *missing,
              std::string &error) {
        if (loaded_) return true;
        if (load_failed_) return error = load_error_, false;
        load_failed_ = true;  // until all of it is there
        auto fail = [&](const std::string &why) {
            error = load_error_ = why;
            return false;
        };
        if (!files_present(runtime, own, count)) return fail(missing);
        wchar_t system[MAX_PATH];
        const UINT n = GetSystemDirectoryW(system, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return fail("no system folder");
        const HMODULE driver = LoadLibraryExW((std::wstring(system) + L"\\nvcuda.dll").c_str(), nullptr, 0);
        if (!driver) return fail("no NVIDIA driver with CUDA (nvcuda.dll)");
        if (!bind(driver, "cuInit", cu_init_) || !bind(driver, "cuDeviceGetCount", cu_count_) ||
            !bind(driver, "cuDeviceGet", cu_device_) || !bind(driver, "cuDeviceGetLuid", cu_luid_) ||
            !bind(driver, "cuDeviceGetAttribute", cu_attribute_) ||
            !bind(driver, "cuDevicePrimaryCtxRetain", cu_retain_) ||
            !bind(driver, "cuDevicePrimaryCtxRelease_v2", cu_release_) || !bind(driver, "cuCtxPushCurrent_v2", cu_push_) ||
            !bind(driver, "cuCtxPopCurrent_v2", cu_pop_) || !bind(driver, "cuStreamCreate", cu_stream_create_) ||
            !bind(driver, "cuStreamDestroy_v2", cu_stream_destroy_) ||
            !bind(driver, "cuStreamSynchronize", cu_stream_sync_) ||
            !bind(driver, "cuGraphicsD3D11RegisterResource", cu_register_) ||
            !bind(driver, "cuGraphicsUnregisterResource", cu_unregister_) ||
            !bind(driver, "cuGraphicsResourceSetMapFlags_v2", cu_map_flags_) ||
            !bind(driver, "cuGraphicsMapResources", cu_map_) || !bind(driver, "cuGraphicsUnmapResources", cu_unmap_) ||
            !bind(driver, "cuGraphicsResourceGetMappedPointer_v2", cu_pointer_))
            return fail("this NVIDIA driver's CUDA lacks a function the filter needs");
        HMODULE core = nullptr, image = nullptr;
        auto library = [&](const wchar_t *name) {
            // With the folder first in the search, so each finds the others there.
            const HMODULE dll = LoadLibraryExW((runtime + L"\\" + name).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!dll) {
                char text[96];
                snprintf(text, sizeof(text), "%ls did not load (error %lu)", name, GetLastError());
                fail(text);
                return false;
            }
            if (wcscmp(name, L"NVVideoEffects.dll") == 0) core = dll;
            if (wcscmp(name, L"NVCVImage.dll") == 0) image = dll;
            return true;
        };
        for (const wchar_t *name : kCommon)
            if (!library(name)) return false;
        for (size_t i = 0; i < count; ++i)
            if (!library(own[i])) return false;
        if (!bind(core, "NvVFX_GetVersion", get_version_) || !bind(core, "NvVFX_CreateEffect", create_effect_) ||
            !bind(core, "NvVFX_DestroyEffect", destroy_effect_) || !bind(core, "NvVFX_SetU32", set_u32_) ||
            !bind(core, "NvVFX_SetF32", set_f32_) ||
            !bind(core, "NvVFX_SetImage", set_image_) || !bind(core, "NvVFX_SetCudaStream", set_stream_) ||
            !bind(core, "NvVFX_Load", load_effect_) || !bind(core, "NvVFX_Run", run_effect_) ||
            !bind(image, "NvCVImage_Create", image_create_) || !bind(image, "NvCVImage_Dealloc", image_dealloc_) ||
            !bind(image, "NvCVImage_Destroy", image_destroy_) || !bind(image, "NvCVImage_Init", image_init_) ||
            !bind(image, "NvCV_GetErrorStringFromCode", error_string_))
            return fail("the Video Effects SDK in that folder lacks a function the filter needs");
        unsigned version = 0;
        if (get_version_(&version) == 0) {
            char text[32];
            snprintf(text, sizeof(text), "%u.%u.%u", version >> 24, (version >> 16) & 255, (version >> 8) & 255);
            version_ = text;
        }
        const int code = cu_init_(0);
        if (code != 0) return fail("CUDA did not start (error " + std::to_string(code) + ")");
        load_failed_ = false;
        loaded_ = true;
        return true;
    }

    // The CUDA context of the GPU the Direct3D device is on (CUDA numbers the
    // GPUs in its own order), and a stream for the effect's work. from_turing:
    // the effect runs from the RTX 20 series on (compute capability 7.5); else
    // on Ada and Blackwell only (8.9 and 12.x), and `needs` says so.
    bool open_cuda(ID3D11Device *device, bool from_turing, const char *needs, std::string &error) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&desc)))
            return error = "no DXGI adapter", false;
        if (context_ && memcmp(&luid_, &desc.AdapterLuid, sizeof(LUID)) == 0) return true;
        close_cuda();
        if (desc.VendorId != 0x10DE) return error = "not an NVIDIA GPU", false;
        int count = 0;
        if (!cuda(cu_count_(&count), "cuDeviceGetCount", error)) return false;
        int found = -1;
        for (int i = 0; i < count && found < 0; ++i) {
            int dev = 0;
            char luid[8] = {};
            unsigned mask = 0;
            if (cu_device_(&dev, i) == 0 && cu_luid_(luid, &mask, dev) == 0 && mask != 0 &&
                memcmp(luid, &desc.AdapterLuid, sizeof(LUID)) == 0)
                found = dev;
        }
        if (found < 0) return error = "CUDA does not see the GPU the picture is drawn on", false;
        int major = 0, minor = 0;
        cu_attribute_(&major, kComputeMajor, found);
        cu_attribute_(&minor, kComputeMinor, found);
        const bool capable = from_turing ? (major > 7 || (major == 7 && minor >= 5))
                                         : ((major == 8 && minor == 9) || major >= 12);
        if (!capable) return error = needs, false;
        if (!cuda(cu_retain_(&context_, found), "cuDevicePrimaryCtxRetain", error)) return context_ = nullptr, false;
        cuda_device_ = found;
        luid_ = desc.AdapterLuid;
        Current current(*this);
        if (!current.ok) return error = "the CUDA context could not be made current", close_cuda(), false;
        if (!cuda(cu_stream_create_(&stream_, 0), "cuStreamCreate", error)) return stream_ = nullptr, close_cuda(), false;
        return true;
    }

    void close_cuda() {
        if (!context_) return;
        if (stream_) {
            Current current(*this);
            cu_stream_destroy_(stream_);
        }
        cu_release_(cuda_device_);
        stream_ = nullptr;
        context_ = nullptr;
    }

    // A Direct3D 11 buffer of width x height packed pixels, with its views.
    static bool make_buffer(ID3D11Device *device, Shared &s, UINT width, UINT height, std::string &error) {
        const UINT pixels = width * height;
        s.width = width;
        s.height = height;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = pixels * 4;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = device->CreateBuffer(&d, nullptr, &s.buf);
        if (FAILED(hr)) return error = "pixel buffer: " + hex(hr), false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_UINT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = pixels;
        hr = device->CreateShaderResourceView(s.buf.Get(), &sd, &s.srv);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = pixels;
        if (SUCCEEDED(hr)) hr = device->CreateUnorderedAccessView(s.buf.Get(), &ud, &s.uav);
        if (FAILED(hr)) return error = "pixel buffer view: " + hex(hr), false;
        return true;
    }

    // The buffer as CUDA's to map (read only for an input, written whole for
    // an output), and the SDK's description of it. With the CUDA context current.
    bool share(Shared &s, bool output, bool ten_bit, std::string &error) {
        if (!cuda(cu_register_(&s.resource, s.buf.Get(), 0), "cuGraphicsD3D11RegisterResource", error))
            return s.resource = nullptr, false;
        cu_map_flags_(s.resource, output ? kMapWriteDiscard : kMapReadOnly);
        // A description from the SDK, without the pixels it comes with:
        // map() points it at the buffer.
        const int format = ten_bit ? kRgb10a2 : kRgba, type = ten_bit ? kP32 : kU8;
        if (!nv(image_create_(1, 1, format, type, kChunky, kCpu, 1, &s.image), "NvCVImage_Create", error))
            return s.image = nullptr, false;
        image_dealloc_(s.image);
        return true;
    }

    // Lets go of what share() made. With the CUDA context current.
    void unshare(Shared &s) {
        if (s.image) image_destroy_(s.image);
        if (s.resource) cu_unregister_(s.resource);
        s = Shared{};
    }

    // Maps the buffers as CUDA memory and tells the SDK where each is (the
    // address may differ from one mapping to the next: moved_ says whether
    // one did).
    bool map(Shared *shared, int count, bool ten_bit, std::string &error) {
        void *resources[4] = {};
        for (int i = 0; i < count; ++i) resources[i] = shared[i].resource;
        if (!cuda(cu_map_(unsigned(count), resources, stream_), "cuGraphicsMapResources", error)) return false;
        const int format = ten_bit ? kRgb10a2 : kRgba, type = ten_bit ? kP32 : kU8;
        moved_ = false;
        for (int i = 0; i < count; ++i) {
            Shared &s = shared[i];
            unsigned long long pointer = 0;
            size_t size = 0;
            if (!cuda(cu_pointer_(&pointer, &size, s.resource), "cuGraphicsResourceGetMappedPointer", error) ||
                !nv(image_init_(s.image, s.width, s.height, int(s.width * 4), reinterpret_cast<void *>(pointer), format,
                                type, kChunky, kGpu),
                    "NvCVImage_Init", error)) {
                unmap(shared, count);
                return false;
            }
            moved_ = moved_ || pointer != s.pointer;
            s.pointer = pointer;
        }
        return true;
    }

    void unmap(Shared *shared, int count) {
        void *resources[4] = {};
        for (int i = 0; i < count; ++i) resources[i] = shared[i].resource;
        cu_unmap_(unsigned(count), resources, stream_);
    }

    static double now_ms() {
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
    }

    bool moved_ = false;  // the last mapping had a buffer at another address

    // For this object: the DLLs, and the CUDA context of one GPU.
    bool loaded_ = false, load_failed_ = false;
    std::string load_error_, version_;
    void *context_ = nullptr, *stream_ = nullptr;
    int cuda_device_ = 0;
    LUID luid_{};

    // CUDA's driver API (nvcuda.dll): a device is an int, the rest handles.
    int(__stdcall *cu_init_)(unsigned) = nullptr;
    int(__stdcall *cu_count_)(int *) = nullptr;
    int(__stdcall *cu_device_)(int *, int) = nullptr;
    int(__stdcall *cu_luid_)(char *, unsigned *, int) = nullptr;
    int(__stdcall *cu_attribute_)(int *, int, int) = nullptr;
    int(__stdcall *cu_retain_)(void **, int) = nullptr;
    int(__stdcall *cu_release_)(int) = nullptr;
    int(__stdcall *cu_push_)(void *) = nullptr;
    int(__stdcall *cu_pop_)(void **) = nullptr;
    int(__stdcall *cu_stream_create_)(void **, unsigned) = nullptr;
    int(__stdcall *cu_stream_destroy_)(void *) = nullptr;
    int(__stdcall *cu_stream_sync_)(void *) = nullptr;
    int(__stdcall *cu_register_)(void **, ID3D11Resource *, unsigned) = nullptr;
    int(__stdcall *cu_unregister_)(void *) = nullptr;
    int(__stdcall *cu_map_flags_)(void *, unsigned) = nullptr;
    int(__stdcall *cu_map_)(unsigned, void **, void *) = nullptr;
    int(__stdcall *cu_unmap_)(unsigned, void **, void *) = nullptr;
    int(__stdcall *cu_pointer_)(unsigned long long *, size_t *, void *) = nullptr;

    // The Video Effects SDK's C functions (nvVideoEffects.h, nvCVImage.h).
    int(__cdecl *get_version_)(unsigned *) = nullptr;
    int(__cdecl *create_effect_)(const char *, void **) = nullptr;
    void(__cdecl *destroy_effect_)(void *) = nullptr;
    int(__cdecl *set_u32_)(void *, const char *, unsigned) = nullptr;
    int(__cdecl *set_f32_)(void *, const char *, float) = nullptr;
    int(__cdecl *set_image_)(void *, const char *, void *) = nullptr;
    int(__cdecl *set_stream_)(void *, const char *, void *) = nullptr;
    int(__cdecl *load_effect_)(void *) = nullptr;
    int(__cdecl *run_effect_)(void *, int) = nullptr;
    int(__cdecl *image_create_)(unsigned, unsigned, int, int, unsigned, unsigned, unsigned, void **) = nullptr;
    void(__cdecl *image_dealloc_)(void *) = nullptr;
    void(__cdecl *image_destroy_)(void *) = nullptr;
    int(__cdecl *image_init_)(void *, unsigned, unsigned, int, void *, int, int, unsigned, unsigned) = nullptr;
    const char *(__cdecl *error_string_)(int) = nullptr;
};

}  // namespace fg
