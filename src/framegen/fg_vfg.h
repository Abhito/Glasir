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
// Nothing of NVIDIA's is built in: the filter's DLLs are loaded at run time from
// a folder on the user's PC (NVVideoEffects.dll, NVCVImage.dll,
// nvVFXVideoFrameGeneration.dll and what they need), and CUDA from the
// driver's nvcuda.dll (fg_nvfx.h, which fg_sr.h shares). It runs on an RTX 40
// or 50; create() says why when it cannot.
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
#include "fg_nvfx.h"
#include "fg_vfg_auto.h"  // VfgMode

namespace fg {

class Vfg : public NvFx {
public:
    // The files create() loads from a folder.
    static bool runtime_present(const std::wstring &folder) { return files_present(folder, kOwn, 1); }

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
        auto_cut_ = auto_cut;
        device_ = device;
        if (!make_d3d(error) || !load(runtime, kOwn, 1, "NVIDIA's Video Frame Generation is not installed", error) ||
            !open_cuda(device, false, "needs an RTX 40 or 50 series GPU", error) || !share_buffers(error) ||
            !make_filter(mode, error)) {
            release();
            return false;
        }
        at_ = int(mode);
        return true;
    }

    // Another model from the next draw on. Each is a filter of its own on the
    // same buffers. keep: those made so far stay loaded, so that going back
    // to one costs nothing (VfgQuality=auto; each holds video memory); else
    // the others are let go. False: the model could not be made (error says
    // why) and the one in use goes on.
    bool use(VfgMode mode, bool keep, std::string &error) {
        if (!ready()) return error = "not created", false;
        const int m = int(mode);
        if (m != at_) {
            if (!effects_[m] && !make_filter(mode, error)) return false;
            at_ = m;
            pair_ready_ = false;  // the next draw gives it its frames
        }
        if (!keep) {
            Current current(*this);
            for (int i = 0; i < 3; ++i)
                if (i != at_ && effects_[i]) {
                    destroy_effect_(effects_[i]);
                    effects_[i] = nullptr;
                }
        }
        return true;
    }
    VfgMode mode() const { return VfgMode(at_); }

    // Lets go of everything made for one size. The filter's DLLs and the CUDA
    // context stay for the next create().
    void release() {
        if (context_) {
            Current current(*this);
            for (void *effect : effects_)
                if (effect) destroy_effect_(effect);
            for (Shared &s : shared_) unshare(s);
        }
        for (void *&effect : effects_) effect = nullptr;
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

    bool ready() const { return effects_[at_] != nullptr; }
    UINT width() const { return frame_w_; }
    UINT height() const { return frame_h_; }
    const Picture &picture() const { return picture_; }
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
        if (!ready()) return error = "not created", false;
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
    // The filter's own DLL, after fg_nvfx.h's.
    static constexpr const wchar_t *kOwn[1] = {L"nvVFXVideoFrameGeneration.dll"};

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
        for (Shared &s : shared_)
            if (!make_buffer(device_.Get(), s, width_, height_, error)) return false;
        return true;
    }

    bool share_buffers(std::string &error) {
        Current current(*this);
        if (!current.ok) return error = "the CUDA context could not be made current", false;
        for (int i = 0; i < 3; ++i)
            if (!share(shared_[i], i == 2, ten_bit_, error)) return false;
        return true;
    }

    // The filter with this model, on the buffers.
    bool make_filter(VfgMode mode, std::string &error) {
        Current current(*this);
        if (!current.ok) return error = "the CUDA context could not be made current", false;
        if (!map(shared_, 3, ten_bit_, error)) return false;
        // In the order of NVIDIA's sample: the inputs before the model is
        // loaded, the output and the multiplier after.
        void *effect = nullptr;
        const bool ok = nv(create_effect_("VideoFrameGeneration", &effect), "NvVFX_CreateEffect", error) &&
                        nv(set_stream_(effect, "CudaStream", stream_), "CudaStream", error) &&
                        nv(set_u32_(effect, "InputWidth", width_), "InputWidth", error) &&
                        nv(set_u32_(effect, "InputHeight", height_), "InputHeight", error) &&
                        nv(set_u32_(effect, "Mode", unsigned(mode)), "Mode", error) &&
                        nv(set_u32_(effect, "AutomaticShotChangeDetectionEnabled", auto_cut_ ? 1u : 0u),
                           "AutomaticShotChangeDetectionEnabled", error) &&
                        nv(set_image_(effect, "SrcImage0", shared_[0].image), "SrcImage0", error) &&
                        nv(set_image_(effect, "SrcImage1", shared_[1].image), "SrcImage1", error) &&
                        nv(load_effect_(effect), "NvVFX_Load", error) &&
                        nv(set_image_(effect, "DstImage0", shared_[2].image), "DstImage0", error) &&
                        nv(set_u32_(effect, "FrameMultiplier", 2), "FrameMultiplier", error);
        unmap(shared_, 3);
        if (!ok) {
            if (effect) destroy_effect_(effect);
            return false;
        }
        effects_[int(mode)] = effect;
        multipliers_[int(mode)] = 2;
        return true;
    }

    // The filter on the two packed frames, into the third buffer: frame
    // `index` of the `multiplier - 1` between them. fresh: the frames are a
    // new pair.
    bool run(bool fresh, unsigned multiplier, unsigned index, std::string &error) {
        const double t0 = now_ms();
        Current current(*this);
        if (!current.ok) return error = "the CUDA context could not be made current", false;
        if (!map(shared_, 3, ten_bit_, error)) return false;
        const double t1 = now_ms();
        void *const effect = effects_[at_];
        unsigned &set_multiplier = multipliers_[at_];
        // Binding the inputs again is what tells the filter this is a new
        // pair; for the pair's other frames it goes on with what it has
        // (unless CUDA has the buffers somewhere else now).
        bool ok = true;
        if (fresh || moved_)
            ok = nv(set_image_(effect, "SrcImage0", shared_[0].image), "SrcImage0", error) &&
                 nv(set_image_(effect, "SrcImage1", shared_[1].image), "SrcImage1", error) &&
                 nv(set_u32_(effect, "ShotChange", 0), "ShotChange", error);
        // Setting the multiplier clears the index: only when it changes, and before the index.
        if (ok && multiplier != set_multiplier) {
            ok = nv(set_u32_(effect, "FrameMultiplier", multiplier), "FrameMultiplier", error);
            if (ok) set_multiplier = multiplier;
        }
        ok = ok && nv(set_image_(effect, "DstImage0", shared_[2].image), "DstImage0", error) &&
             nv(set_u32_(effect, "FrameIndex", index), "FrameIndex", error) &&
             nv(run_effect_(effect, 0), "NvVFX_Run", error);
        const double t2 = now_ms();
        unmap(shared_, 3);
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
    bool auto_cut_ = true;
    Shared shared_[3];  // frame a, frame b, the frame between
    void *effects_[3] = {};               // a filter for each model made so far (by VfgMode)
    unsigned multipliers_[3] = {2, 2, 2}; // each one's FrameMultiplier as last set
    int at_ = 1;                          // the one in use
    bool pair_ready_ = false;   // the buffers hold the pair of the last draw, which worked
    double stage_ms_[3] = {};
};

}  // namespace fg
