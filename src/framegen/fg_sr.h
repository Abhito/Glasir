// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// fg_sr.h - a picture made larger by NVIDIA's Video Super Resolution (the
// Video Effects SDK's "VideoSuperRes" effect), for the add-on's own scaling
// of the capture (glasir.ini video.scaling = nvidia) and its offline
// evaluator.
//
// It is the same model family as the super resolution of the GPU's video
// processor (video.scaling = vsr, mpv's d3d11vpp filter), but asked for
// through the SDK, which brings what the driver's does not: a choice of
// model, among them the "high-bitrate" ones for clean sources such as a
// capture card's picture (the driver's are for compressed video), and
// G-SYNC, which the driver switches off while its own runs.
//
// Nothing of NVIDIA's is built in (fg_nvfx.h): nvVFXVideoSuperRes.dll and
// nvngx_vsr.dll are loaded from the folder the SDK's other DLLs are in. It
// runs from the RTX 20 series on; create() says why when it cannot.
//
// The way of a picture (vfg.hlsl's two shaders, as fg_vfg.h uses them):
//   PackCS    the source into a Direct3D 11 buffer, 32 bits a pixel;
//   CUDA      maps it and a second buffer of the output's size, the effect
//             reads one and writes the other, and they are handed back;
//   UnpackPS  draws the second into its place in the target.
// Unmapping waits for the effect's work, so draw() returns when the picture
// is done: the calling thread waits that long.
//
// NVIDIA's licence does not allow giving results of its software (speed,
// quality, comparisons) to others without its permission: nothing here logs
// a timing.

#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <string>

#include "fg_interp.h"  // the full-screen vertex shader (g_blend_vs)
#include "fg_nvfx.h"

namespace fg {

// The effect's "QualityLevel": its high-bitrate models, smaller to larger. (0
// is bicubic, 1-4 the models for compressed video, 8-15 denoise and deblur
// at one size, 21 and 23 for streams to be encoded.)
enum class SrLevel { low = 16, medium = 17, high = 18, ultra = 19 };

class Sr : public NvFx {
public:
    // The files create() loads from a folder.
    static bool runtime_present(const std::wstring &folder) { return files_present(folder, kOwn, 2); }

    // in_w x in_h: the picture as it comes. out_w x out_h: as large as it is
    // to be (not smaller; the effect scales up to four times). An effect
    // already made for this device and bit depth is kept and given buffers of
    // the new sizes (a window being resized): its model is not loaded again.
    bool create(ID3D11Device *device, UINT in_w, UINT in_h, UINT out_w, UINT out_h, bool ten_bit,
                const std::wstring &runtime, SrLevel level, std::string &error) {
        if (effect_ && device_.Get() == device && ten_bit_ == ten_bit) {
            level_ = level;
            if (resize(in_w, in_h, out_w, out_h, error)) return true;
        }
        release();
        in_w_ = in_w;
        in_h_ = in_h;
        out_w_ = out_w;
        out_h_ = out_h;
        ten_bit_ = ten_bit;
        level_ = level;
        device_ = device;
        if (!make_d3d(error) || !load(runtime, kOwn, 2, "NVIDIA's Video Super Resolution is not installed", error) ||
            !open_cuda(device, true, "needs an NVIDIA RTX GPU (20 series or later)", error) || !make_effect(error)) {
            release();
            return false;
        }
        return true;
    }

    // Lets go of everything made for one size. The SDK's DLLs and the CUDA
    // context stay for the next create().
    void release() {
        if (context_) {
            Current current(*this);
            if (effect_) destroy_effect_(effect_);
            for (Shared &s : shared_) unshare(s);
        }
        effect_ = nullptr;
        for (Shared &s : shared_) s = Shared{};
        vs_.Reset();
        pack_cs_.Reset();
        unpack_ps_.Reset();
        cb_.Reset();
        device_.Reset();
        in_w_ = in_h_ = out_w_ = out_h_ = 0;
    }

    bool ready() const { return effect_ != nullptr; }
    UINT in_width() const { return in_w_; }
    UINT in_height() const { return in_h_; }
    UINT out_width() const { return out_w_; }
    UINT out_height() const { return out_h_; }
    bool ten_bit() const { return ten_bit_; }
    // Another model, from the next draw on (the effect takes it without loading anew).
    void set_level(SrLevel level) { level_ = level; }

    // The picture in source (a shader resource, in_w x in_h) made out_w x
    // out_h, drawn into target with its corner at (x, y); returns when it is
    // done. False: the effect failed (error says how) and target is as it was.
    bool draw(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *source, ID3D11RenderTargetView *target, UINT x, UINT y,
              std::string &error) {
        if (!effect_) return error = "not created", false;
        Params params{0, 0, in_w_, in_h_, ten_bit_ ? 1u : 0u, 0.0f, 0.0f, 0};
        ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &params, 0, 0);
        ID3D11UnorderedAccessView *no_uav = nullptr;
        ID3D11ShaderResourceView *no_srv = nullptr;
        ctx->CSSetShader(pack_cs_.Get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->CSSetShaderResources(0, 1, &source);
        ctx->CSSetUnorderedAccessViews(0, 1, shared_[0].uav.GetAddressOf(), nullptr);
        ctx->Dispatch((in_w_ + 7) / 8, (in_h_ + 7) / 8, 1);
        ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
        ctx->CSSetShaderResources(0, 1, &no_srv);

        if (!run(error)) return false;

        params = Params{x, y, out_w_, out_h_, ten_bit_ ? 1u : 0u, 0.0f, 0.0f, 0};
        ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &params, 0, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->RSSetState(nullptr);
        // Only the picture's place is drawn: what is around it stays.
        const D3D11_VIEWPORT vp{float(x), float(y), float(out_w_), float(out_h_), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &target, nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->PSSetShader(unpack_ps_.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ID3D11ShaderResourceView *srvs[2] = {shared_[1].srv.Get(), nullptr};
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *none[2] = {};
        ctx->PSSetShaderResources(0, 2, none);
        return true;
    }

private:
    // The effect's own DLLs, after fg_nvfx.h's.
    static constexpr const wchar_t *kOwn[2] = {L"nvVFXVideoSuperRes.dll", L"nvngx_vsr.dll"};

    // vfg.hlsl's VfgParams.
    struct Params {
        UINT origin_x, origin_y, picture_w, picture_h, ten_bit;
        FLOAT marker, marker_level;
        UINT pad;
    };

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
        if (FAILED(hr)) return false;
        return make_buffer(device_.Get(), shared_[0], in_w_, in_h_, error) &&
               make_buffer(device_.Get(), shared_[1], out_w_, out_h_, error);
    }

    bool make_effect(std::string &error) {
        Current current(*this);
        if (!current.ok) return error = "the CUDA context could not be made current", false;
        if (!share(shared_[0], false, ten_bit_, error) || !share(shared_[1], true, ten_bit_, error)) return false;
        if (!map(shared_, 2, ten_bit_, error)) return false;
        // The pixels' encoding before the images (it turns down an image of
        // the other kind), the model before it is loaded.
        const bool ok = nv(create_effect_("VideoSuperRes", &effect_), "NvVFX_CreateEffect", error) &&
                        nv(set_stream_(effect_, "CudaStream", stream_), "CudaStream", error) &&
                        nv(set_u32_(effect_, "ImageEncodingMode", ten_bit_ ? 1u : 0u), "ImageEncodingMode", error) &&
                        nv(set_image_(effect_, "SrcImage0", shared_[0].image), "SrcImage0", error) &&
                        nv(set_image_(effect_, "DstImage0", shared_[1].image), "DstImage0", error) &&
                        nv(set_u32_(effect_, "QualityLevel", unsigned(level_)), "QualityLevel", error) &&
                        nv(set_f32_(effect_, "Strength", 1.0f), "Strength", error) &&
                        nv(load_effect_(effect_), "NvVFX_Load", error);
        set_level_ = level_;
        unmap(shared_, 2);
        if (!ok && effect_) {
            destroy_effect_(effect_);
            effect_ = nullptr;
        }
        return ok;
    }

    // Buffers of other sizes for the effect as it is. False: it has to be made anew.
    bool resize(UINT in_w, UINT in_h, UINT out_w, UINT out_h, std::string &error) {
        Current current(*this);
        if (!current.ok) return error = "the CUDA context could not be made current", false;
        for (Shared &s : shared_) unshare(s);
        in_w_ = in_w;
        in_h_ = in_h;
        out_w_ = out_w;
        out_h_ = out_h;
        if (!make_buffer(device_.Get(), shared_[0], in_w_, in_h_, error) ||
            !make_buffer(device_.Get(), shared_[1], out_w_, out_h_, error) || !share(shared_[0], false, ten_bit_, error) ||
            !share(shared_[1], true, ten_bit_, error) || !map(shared_, 2, ten_bit_, error))
            return false;
        const bool ok = nv(set_image_(effect_, "SrcImage0", shared_[0].image), "SrcImage0", error) &&
                        nv(set_image_(effect_, "DstImage0", shared_[1].image), "DstImage0", error);
        unmap(shared_, 2);
        return ok;
    }

    // The effect on the packed picture, into the second buffer.
    bool run(std::string &error) {
        Current current(*this);
        if (!current.ok) return error = "the CUDA context could not be made current", false;
        if (!map(shared_, 2, ten_bit_, error)) return false;
        bool ok = true;
        // Where the buffers are, when CUDA has them somewhere else now.
        if (moved_)
            ok = nv(set_image_(effect_, "SrcImage0", shared_[0].image), "SrcImage0", error) &&
                 nv(set_image_(effect_, "DstImage0", shared_[1].image), "DstImage0", error);
        if (ok && level_ != set_level_) {
            ok = nv(set_u32_(effect_, "QualityLevel", unsigned(level_)), "QualityLevel", error);
            if (ok) set_level_ = level_;
        }
        ok = ok && nv(run_effect_(effect_, 0), "NvVFX_Run", error);
        // The effect's work may still be under way on the stream: unmapping
        // waits for it, so Direct3D reads the finished buffer.
        unmap(shared_, 2);
        return ok;
    }

    ComPtr<ID3D11Device> device_;
    UINT in_w_ = 0, in_h_ = 0, out_w_ = 0, out_h_ = 0;
    bool ten_bit_ = false;
    SrLevel level_ = SrLevel::medium, set_level_ = SrLevel::medium;  // wanted, and as the effect has it
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11ComputeShader> pack_cs_;
    ComPtr<ID3D11PixelShader> unpack_ps_;
    ComPtr<ID3D11Buffer> cb_;
    Shared shared_[2];  // the picture as it comes, and made larger
    void *effect_ = nullptr;
};

}  // namespace fg
