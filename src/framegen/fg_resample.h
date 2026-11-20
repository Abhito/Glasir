// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// fg_resample.h - a picture drawn at another size (resample.hlsl): Hermite
// over all the source's pixels when it is made smaller, Catmull-Rom when it
// is made larger. For the add-on's own scaling (NVIDIA Super Resolution with
// "Scale to": the effect's picture, taken to its place in the window) and its
// offline evaluator.

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
        return true;
    }

    void release() {
        vs_.Reset();
        ps_.Reset();
        cb_.Reset();
    }

    bool ready() const { return ps_ != nullptr; }

    // source (src_w x src_h) drawn w x h into target with its corner at (x, y).
    void draw(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *source, UINT src_w, UINT src_h,
              ID3D11RenderTargetView *target, UINT x, UINT y, UINT w, UINT h) {
        const Params params{{float(src_w), float(src_h)}, {float(x), float(y)}, {float(w), float(h)}, {0, 0}};
        ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &params, 0, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->RSSetState(nullptr);
        // Only the picture's place is drawn: what is around it stays.
        const D3D11_VIEWPORT vp{float(x), float(y), float(w), float(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &target, nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->PSSetShaderResources(0, 1, &source);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
    }

private:
    // resample.hlsl's ResampleParams.
    struct Params {
        FLOAT src_size[2], dst_origin[2], dst_size[2], pad[2];
    };

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
};

}  // namespace fg
