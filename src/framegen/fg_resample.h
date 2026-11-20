// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// fg_resample.h - a picture drawn at another size (resample.hlsl):
// Catmull-Rom, over all the source's pixels under an output pixel when it is
// made smaller (the whole curve down to four times smaller, which is as far
// as the add-on goes). For the add-on's own scaling (NVIDIA Super Resolution
// with "Scale to": the effect's picture, taken to its place in the window)
// and its offline evaluator.
//
// In two passes, across and then down, with a picture between them that is
// as wide as the place and as tall as the source (16 bits a channel: 88 MB
// for an 8K picture made 1440p). It is made by prepare() or at the first
// draw, and again when the sizes change.

#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <string>

#include "fg_interp.h"  // the full-screen vertex shader (g_blend_vs)
#include "resample_ps.h"

namespace fg {

class Resample {
public:
    bool create(ID3D11Device *device, std::string &error) {
        release();
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(Params);
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateVertexShader(g_blend_vs, sizeof(g_blend_vs), nullptr, &vs_)) ||
            FAILED(device->CreatePixelShader(g_resample_ps, sizeof(g_resample_ps), nullptr, &ps_)) ||
            FAILED(device->CreateBuffer(&cbd, nullptr, &cb_))) {
            release();
            return error = "the shaders that take the picture to its size could not be made", false;
        }
        device_ = device;
        return true;
    }

    void release() {
        vs_.Reset();
        ps_.Reset();
        cb_.Reset();
        let_go();
        device_.Reset();
    }

    // Lets go of the picture between the passes: the next draw makes it again.
    void let_go() {
        between_.Reset();
        between_view_.Reset();
        between_target_.Reset();
        between_w_ = between_h_ = 0;
    }

    bool ready() const { return ps_ != nullptr; }

    // Makes the picture between the passes ahead of the first draw, for a
    // source src_h tall drawn w wide. False: no memory for it.
    bool prepare(UINT w, UINT src_h) { return ready() && make_between(w, src_h); }

    // source (src_w x src_h) drawn w x h into target with its corner at (x, y).
    // False: there was no memory for the picture between the passes, and
    // target is as it was.
    bool draw(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *source, UINT src_w, UINT src_h,
              ID3D11RenderTargetView *target, UINT x, UINT y, UINT w, UINT h) {
        if (!ready() || !make_between(w, src_h)) return false;
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->RSSetState(nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        // Across: the source's rows, each made as wide as the place.
        pass(ctx, source, {{float(src_w), float(src_h)}, {0, 0}, {float(w), float(src_h)}, {1, 0}},
             between_target_.Get(), 0, 0, w, src_h);
        // Down: those rows made as tall as the place. Only the picture's place is drawn: what is
        // around it stays.
        pass(ctx, between_view_.Get(), {{float(w), float(src_h)}, {float(x), float(y)}, {float(w), float(h)}, {0, 1}},
             target, x, y, w, h);
        return true;
    }

private:
    // resample.hlsl's ResampleParams.
    struct Params {
        FLOAT src_size[2], dst_origin[2], dst_size[2], axis[2];
    };

    void pass(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *source, const Params &params,
              ID3D11RenderTargetView *target, UINT x, UINT y, UINT w, UINT h) {
        ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &params, 0, 0);
        const D3D11_VIEWPORT vp{float(x), float(y), float(w), float(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &target, nullptr);
        ctx->PSSetShaderResources(0, 1, &source);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
    }

    // The picture between the passes, at this size.
    bool make_between(UINT w, UINT h) {
        if (between_ && between_w_ == w && between_h_ == h) return true;
        let_go();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &between_)) ||
            FAILED(device_->CreateShaderResourceView(between_.Get(), nullptr, &between_view_)) ||
            FAILED(device_->CreateRenderTargetView(between_.Get(), nullptr, &between_target_))) {
            let_go();
            return false;
        }
        between_w_ = w;
        between_h_ = h;
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> between_;  // the picture between the passes
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> between_view_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> between_target_;
    UINT between_w_ = 0, between_h_ = 0;
};

}  // namespace fg
