// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// fg_vfg.h - the frame between two real frames, made by NVIDIA's Video Frame
// Generation (the Video Effects SDK's "VideoFrameGeneration" filter) instead
// of fg_interp.h's own motion search. Shared by the ReShade add-on and the
// offline evaluator, as fg_interp.h is.
//
// Nothing of NVIDIA's is built in: the filter's DLLs are loaded at run time
// from a folder on the user's PC (NVVideoEffects.dll, NVCVImage.dll,
// nvVFXVideoFrameGeneration.dll and what they need), and CUDA from the
// driver's nvcuda.dll. Only their public C functions are called, by name.
// It runs on an RTX 40 or 50; create() says why when it cannot.
//
// The way of a frame (vfg.hlsl has the two shaders):
//   PackCS    the picture's part of frames a and b into two Direct3D 11
//             buffers, 32 bits a pixel (8-bit RGBA, or R10 G10 B10 A2);
//   CUDA      maps those buffers and a third as its own memory (CUDA cannot
//             share a 10-bit texture, a buffer it can), the filter reads two
//             and writes the third, and they are handed back;
//   UnpackPS  draws the third into the target, and what lies outside the
//             picture as frame a has it.
// Mapping waits for Direct3D's work on the buffers and unmapping for the
// filter's, so draw() returns when the filter is done: the calling thread
// waits that long (fg_interp.h's draw only queues its work).
//
// NVIDIA's licence does not allow giving results of its software (speed,
// quality, comparisons) to others without its permission: nothing here logs
// a timing, and stage_ms() is for a developer's own look (the add-on's
// GLASIR_VFG_PROFILE), not for logs that users send in.

#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "fg_interp.h"  // Picture, DrawOptions, and the full-screen vertex shader (g_blend_vs)
#include "vfg_pack_cs.h"
#include "vfg_unpack_ps.h"

namespace fg {

enum class VfgMode { low = 0, medium = 1, high = 2 };  // the filter's "Mode": its model's size

class Vfg {
public:
    // The files create() loads from a folder.
    static bool runtime_present(const std::wstring &folder) {
        for (const wchar_t *name : kLibraries)
            if (GetFileAttributesW((folder + L"\\" + name).c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        return true;
    }

    // Whether frames of this format can go to the filter, and as 10-bit.
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

    // width x height: the frames given to draw(). picture: where the picture
    // is in them (one that is not inside the frame, or is all of it: none).
    // auto_cut: the filter's own cut detection, which shows frame a again
    // across a cut - and across some fast motion.
    bool create(ID3D11Device *device, UINT width, UINT height, bool ten_bit, const std::wstring &runtime, VfgMode mode,
                bool auto_cut, std::string &error, const Picture &picture = {}) {
        release();
        frame_w_ = width;
        frame_h_ = height;
        picture_ = Picture{};
        if (picture.x0 < picture.x1 && picture.y0 < picture.y1 && picture.x1 <= width && picture.y1 <= height &&
            (picture.x1 - picture.x0 < width || picture.y1 - picture.y0 < height)) {
            picture_ = picture;
            width = picture.x1 - picture.x0;
            height = picture.y1 - picture.y0;
        }
        width_ = width;
        height_ = height;
        ten_bit_ = ten_bit;
        device_ = device;
        if (!make_d3d(error) || !load(runtime, error) || !open_cuda(device, error) || !make_filter(mode, auto_cut, error)) {
            release();
            return false;
        }
        return true;
    }

    // Lets go of everything made for one size. The filter's DLLs and the CUDA
    // context stay for the next create().
    void release() {
        if (context_) {
            Current current(*this);
            if (effect_) destroy_effect_(effect_);
            for (Shared &s : shared_) {
                if (s.image) image_destroy_(s.image);
                if (s.resource) cu_unregister_(s.resource);
            }
        }
        effect_ = nullptr;
        pair_ready_ = false;
        for (Shared &s : shared_) s = Shared{};
        vs_.Reset();
        pack_cs_.Reset();
        unpack_ps_.Reset();
        cb_.Reset();
        device_.Reset();
        width_ = height_ = frame_w_ = frame_h_ = 0;
        picture_ = Picture{};
    }

    bool ready() const { return effect_ != nullptr; }
    UINT width() const { return frame_w_; }
    UINT height() const { return frame_h_; }
    const Picture &picture() const { return picture_; }
    // "1.3.0", once the filter's DLLs are loaded.
    const std::string &version() const { return version_; }
    // The last draw's time on the calling thread, in milliseconds: until the
    // buffers were mapped (Direct3D's work before them done), the filter,
    // until they were handed back.
    const double *stage_ms() const { return stage_ms_; }

    // Draws the frame between a and b into target - o.index of the
    // o.multiplier - 1 the pair gets, half-way by default - and returns when
    // it is done. a, b and target are width x height; a and b are shader
    // resources, target a render target. With o.same_pair the filter goes on
    // with the pair of the draw before (the frames are not packed again).
    // False: the filter failed (error says how) and target is as it was.
    bool draw(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b,
              ID3D11RenderTargetView *target, const DrawOptions &o, std::string &error) {
        if (!effect_) return error = "not created", false;
        const Params params{picture_.x0, picture_.y0, width_, height_, ten_bit_ ? 1u : 0u,
                            o.marker ? 1.0f : 0.0f, o.marker_level, 0};
        ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &params, 0, 0);

        const bool fresh = !o.same_pair || !pair_ready_;
        if (fresh) {
            ID3D11ShaderResourceView *frames[2] = {a, b};
            ID3D11UnorderedAccessView *no_uav = nullptr;
            ID3D11ShaderResourceView *no_srv = nullptr;
            ctx->CSSetShader(pack_cs_.Get(), nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
            for (int i = 0; i < 2; ++i) {
                ctx->CSSetShaderResources(0, 1, &frames[i]);
                ctx->CSSetUnorderedAccessViews(0, 1, shared_[i].uav.GetAddressOf(), nullptr);
                ctx->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
            }
            ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
            ctx->CSSetShaderResources(0, 1, &no_srv);
        }
        const unsigned multiplier = unsigned(o.multiplier < 2 ? 2 : o.multiplier > 8 ? 8 : o.multiplier);
        const unsigned index = unsigned(o.index < 1 ? 1 : o.index >= int(multiplier) ? int(multiplier) - 1 : o.index);
        pair_ready_ = false;
        if (!run(fresh, multiplier, index, error)) return false;
        pair_ready_ = true;

        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->RSSetState(nullptr);
        const D3D11_VIEWPORT vp{0, 0, float(frame_w_), float(frame_h_), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &target, nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->PSSetShader(unpack_ps_.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ID3D11ShaderResourceView *srvs[2] = {shared_[2].srv.Get(), a};
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *none[2] = {};
        ctx->PSSetShaderResources(0, 2, none);
        return true;
    }

private:
    static constexpr const wchar_t *kLibraries[5] = {L"cudart64_12.dll", L"NVCVImage.dll", L"nvngxruntime.dll",
                                                     L"NVVideoEffects.dll", L"nvVFXVideoFrameGeneration.dll"};
    // NvCVImage's names for the two pixel layouts (nvCVImage.h; RGB10A2 and
    // P32 came after the header NVIDIA publishes) and for where pixels are.
    static constexpr int kRgba = 6, kRgb10a2 = 13, kU8 = 1, kP32 = 11;
    static constexpr unsigned kChunky = 0, kCpu = 0, kGpu = 1;
    static constexpr int kComputeMajor = 75, kComputeMinor = 76;  // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_*
    static constexpr unsigned kMapReadOnly = 1, kMapWriteDiscard = 2;

    struct Params {
        UINT origin_x, origin_y, picture_w, picture_h, ten_bit;
        FLOAT marker, marker_level;
        UINT pad;
    };

    // One buffer of packed pixels: Direct3D's views of it, CUDA's handle to it
    // and the filter's description of it.
    struct Shared {
        ComPtr<ID3D11Buffer> buf;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        void *resource = nullptr;
        void *image = nullptr;
        unsigned long long pointer = 0;  // where CUDA had it at the last mapping
    };

    // The CUDA context on this thread for as long as it lives.
    struct Current {
        Vfg &v;
        bool ok;
        explicit Current(Vfg &vfg) : v(vfg), ok(vfg.cu_push_(vfg.context_) == 0) {}
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

    bool make_d3d(std::string &error) {
        HRESULT hr = S_OK;
        auto check = [&](HRESULT r, const char *what) {
            if (FAILED(r) && SUCCEEDED(hr)) {
                hr = r;
                error = std::string(what) + ": " + hex(r);
            }
        };
        check(device_->CreateVertexShader(g_blend_vs, sizeof(g_blend_vs), nullptr, &vs_), "vertex shader");
        check(device_->CreateComputeShader(g_vfg_pack_cs, sizeof(g_vfg_pack_cs), nullptr, &pack_cs_), "pack shader");
        check(device_->CreatePixelShader(g_vfg_unpack_ps, sizeof(g_vfg_unpack_ps), nullptr, &unpack_ps_), "unpack shader");
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(Params);
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device_->CreateBuffer(&cbd, nullptr, &cb_), "constant buffer");
        const UINT pixels = width_ * height_;
        for (Shared &s : shared_) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = pixels * 4;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            check(device_->CreateBuffer(&d, nullptr, &s.buf), "pixel buffer");
            if (FAILED(hr)) break;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32_UINT;
            sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sd.Buffer.NumElements = pixels;
            check(device_->CreateShaderResourceView(s.buf.Get(), &sd, &s.srv), "pixel buffer view");
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_UINT;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = pixels;
            check(device_->CreateUnorderedAccessView(s.buf.Get(), &ud, &s.uav), "pixel buffer view");
        }
        return SUCCEEDED(hr);
    }

    template <typename T> static bool bind(HMODULE module, const char *name, T &fn) {
        fn = reinterpret_cast<T>(GetProcAddress(module, name));
        return fn != nullptr;
    }

    // The filter's DLLs and the driver's CUDA, once for the process.
    bool load(const std::wstring &runtime, std::string &error) {
        if (loaded_) return true;
        if (load_failed_) return error = load_error_, false;
        load_failed_ = true;  // until all of it is there
        auto fail = [&](const std::string &why) {
            error = load_error_ = why;
            return false;
        };
        if (!runtime_present(runtime)) return fail("NVIDIA's Video Frame Generation is not installed");
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
            !bind(driver, "cuGraphicsD3D11RegisterResource", cu_register_) ||
            !bind(driver, "cuGraphicsUnregisterResource", cu_unregister_) ||
            !bind(driver, "cuGraphicsResourceSetMapFlags_v2", cu_map_flags_) ||
            !bind(driver, "cuGraphicsMapResources", cu_map_) || !bind(driver, "cuGraphicsUnmapResources", cu_unmap_) ||
            !bind(driver, "cuGraphicsResourceGetMappedPointer_v2", cu_pointer_))
            return fail("this NVIDIA driver's CUDA lacks a function the filter needs");
        HMODULE core = nullptr, image = nullptr;
        for (const wchar_t *name : kLibraries) {
            // With the folder first in the search, so each finds the others there.
            const HMODULE dll = LoadLibraryExW((runtime + L"\\" + name).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!dll) {
                char text[96];
                snprintf(text, sizeof(text), "%ls did not load (error %lu)", name, GetLastError());
                return fail(text);
            }
            if (wcscmp(name, L"NVVideoEffects.dll") == 0) core = dll;
            if (wcscmp(name, L"NVCVImage.dll") == 0) image = dll;
        }
        if (!bind(core, "NvVFX_GetVersion", get_version_) || !bind(core, "NvVFX_CreateEffect", create_effect_) ||
            !bind(core, "NvVFX_DestroyEffect", destroy_effect_) || !bind(core, "NvVFX_SetU32", set_u32_) ||
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
    // GPUs in its own order), and a stream for the filter's work.
    bool open_cuda(ID3D11Device *device, std::string &error) {
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
        // NVIDIA: Ada and Blackwell on Windows (compute capability 8.9 and 12.x).
        if (!((major == 8 && minor == 9) || major >= 12)) return error = "needs an RTX 40 or 50 series GPU", false;
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

    // Maps the three buffers as CUDA memory and tells the filter where each
    // is (the address may differ from one mapping to the next).
    bool map(std::string &error) {
        void *resources[3] = {shared_[0].resource, shared_[1].resource, shared_[2].resource};
        if (!cuda(cu_map_(3, resources, stream_), "cuGraphicsMapResources", error)) return false;
        const int format = ten_bit_ ? kRgb10a2 : kRgba, type = ten_bit_ ? kP32 : kU8;
        moved_ = false;
        for (Shared &s : shared_) {
            unsigned long long pointer = 0;
            size_t size = 0;
            if (!cuda(cu_pointer_(&pointer, &size, s.resource), "cuGraphicsResourceGetMappedPointer", error) ||
                !nv(image_init_(s.image, width_, height_, int(width_ * 4), reinterpret_cast<void *>(pointer), format, type,
                                kChunky, kGpu),
                    "NvCVImage_Init", error)) {
                unmap();
                return false;
            }
            moved_ = moved_ || pointer != s.pointer;
            s.pointer = pointer;
        }
        return true;
    }

    void unmap() {
        void *resources[3] = {shared_[0].resource, shared_[1].resource, shared_[2].resource};
        cu_unmap_(3, resources, stream_);
    }

    bool make_filter(VfgMode mode, bool auto_cut, std::string &error) {
        Current current(*this);
        if (!current.ok) return error = "the CUDA context could not be made current", false;
        const int format = ten_bit_ ? kRgb10a2 : kRgba, type = ten_bit_ ? kP32 : kU8;
        for (int i = 0; i < 3; ++i) {
            Shared &s = shared_[i];
            if (!cuda(cu_register_(&s.resource, s.buf.Get(), 0), "cuGraphicsD3D11RegisterResource", error))
                return s.resource = nullptr, false;
            cu_map_flags_(s.resource, i < 2 ? kMapReadOnly : kMapWriteDiscard);
            // A description from the SDK, without the pixels it comes with:
            // map() points it at the buffer.
            if (!nv(image_create_(1, 1, format, type, kChunky, kCpu, 1, &s.image), "NvCVImage_Create", error))
                return s.image = nullptr, false;
            image_dealloc_(s.image);
        }
        if (!map(error)) return false;
        // In the order of NVIDIA's sample: the inputs before the model is
        // loaded, the output and the multiplier after.
        const bool ok = nv(create_effect_("VideoFrameGeneration", &effect_), "NvVFX_CreateEffect", error) &&
                        nv(set_stream_(effect_, "CudaStream", stream_), "CudaStream", error) &&
                        nv(set_u32_(effect_, "InputWidth", width_), "InputWidth", error) &&
                        nv(set_u32_(effect_, "InputHeight", height_), "InputHeight", error) &&
                        nv(set_u32_(effect_, "Mode", unsigned(mode)), "Mode", error) &&
                        nv(set_u32_(effect_, "AutomaticShotChangeDetectionEnabled", auto_cut ? 1u : 0u),
                           "AutomaticShotChangeDetectionEnabled", error) &&
                        nv(set_image_(effect_, "SrcImage0", shared_[0].image), "SrcImage0", error) &&
                        nv(set_image_(effect_, "SrcImage1", shared_[1].image), "SrcImage1", error) &&
                        nv(load_effect_(effect_), "NvVFX_Load", error) &&
                        nv(set_image_(effect_, "DstImage0", shared_[2].image), "DstImage0", error) &&
                        nv(set_u32_(effect_, "FrameMultiplier", 2), "FrameMultiplier", error);
        multiplier_ = 2;
        unmap();
        if (!ok && effect_) {
            destroy_effect_(effect_);
            effect_ = nullptr;
        }
        return ok;
    }

    static double now_ms() {
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
    }

    // The filter on the two packed frames, into the third buffer: frame
    // `index` of the `multiplier - 1` between them. fresh: the frames are a
    // new pair.
    bool run(bool fresh, unsigned multiplier, unsigned index, std::string &error) {
        const double t0 = now_ms();
        Current current(*this);
        if (!current.ok) return error = "the CUDA context could not be made current", false;
        if (!map(error)) return false;
        const double t1 = now_ms();
        // Binding the inputs again is what tells the filter this is a new
        // pair; for the pair's other frames it goes on with what it has
        // (unless CUDA has the buffers somewhere else now).
        bool ok = true;
        if (fresh || moved_)
            ok = nv(set_image_(effect_, "SrcImage0", shared_[0].image), "SrcImage0", error) &&
                 nv(set_image_(effect_, "SrcImage1", shared_[1].image), "SrcImage1", error) &&
                 nv(set_u32_(effect_, "ShotChange", 0), "ShotChange", error);
        // Setting the multiplier clears the index: only when it changes, and before the index.
        if (ok && multiplier != multiplier_) {
            ok = nv(set_u32_(effect_, "FrameMultiplier", multiplier), "FrameMultiplier", error);
            if (ok) multiplier_ = multiplier;
        }
        ok = ok && nv(set_image_(effect_, "DstImage0", shared_[2].image), "DstImage0", error) &&
             nv(set_u32_(effect_, "FrameIndex", index), "FrameIndex", error) &&
             nv(run_effect_(effect_, 0), "NvVFX_Run", error);
        const double t2 = now_ms();
        unmap();
        stage_ms_[0] = t1 - t0;
        stage_ms_[1] = t2 - t1;
        stage_ms_[2] = now_ms() - t2;
        return ok;
    }

    ComPtr<ID3D11Device> device_;
    UINT frame_w_ = 0, frame_h_ = 0;  // the frames given to draw()
    Picture picture_;                 // the picture's place in them (all 0: the whole frame)
    UINT width_ = 0, height_ = 0;     // what the filter works on: the picture
    bool ten_bit_ = false;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11ComputeShader> pack_cs_;
    ComPtr<ID3D11PixelShader> unpack_ps_;
    ComPtr<ID3D11Buffer> cb_;
    Shared shared_[3];  // frame a, frame b, the frame between
    void *effect_ = nullptr;
    unsigned multiplier_ = 2;   // the filter's FrameMultiplier as last set
    bool pair_ready_ = false;   // the buffers hold the pair of the last draw, which worked
    bool moved_ = false;        // the last mapping had a buffer at another address
    double stage_ms_[3] = {};

    // For the process: the DLLs, and the CUDA context of one GPU.
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
