// SPDX-License-Identifier: MIT
#include "compositor.h"

#include <cstring>
#include <initializer_list>

#include "composite_ps.h"
#include "composite_vs.h"

namespace glasir::overlay {

StateBackup::StateBackup(ID3D11DeviceContext *c) : c_(c) {
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, &dsv_);
    c->OMGetBlendState(&blend_, blend_factor_, &sample_mask_);
    c->OMGetDepthStencilState(&depth_, &stencil_ref_);
    c->RSGetState(&rs_);
    c->RSGetViewports(&viewport_count_, viewports_);
    c->RSGetScissorRects(&scissor_count_, scissors_);
    c->VSGetShader(&vs_, nullptr, nullptr);
    c->PSGetShader(&ps_, nullptr, nullptr);
    c->GSGetShader(&gs_, nullptr, nullptr);
    c->VSGetConstantBuffers(0, 1, &vs_cb_);
    c->PSGetConstantBuffers(0, 1, &ps_cb_);
    c->PSGetShaderResources(0, 1, &ps_srv_);
    c->PSGetSamplers(0, 1, &ps_sampler_);
    c->IAGetPrimitiveTopology(&topology_);
    c->IAGetInputLayout(&layout_);
    c->IAGetVertexBuffers(0, 1, &vb_, &vb_stride_, &vb_offset_);
    c->IAGetIndexBuffer(&ib_, &ib_format_, &ib_offset_);
}

StateBackup::~StateBackup() {
    c_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, dsv_);
    c_->OMSetBlendState(blend_, blend_factor_, sample_mask_);
    c_->OMSetDepthStencilState(depth_, stencil_ref_);
    c_->RSSetState(rs_);
    c_->RSSetViewports(viewport_count_, viewports_);
    c_->RSSetScissorRects(scissor_count_, scissors_);
    c_->VSSetShader(vs_, nullptr, 0);
    c_->PSSetShader(ps_, nullptr, 0);
    c_->GSSetShader(gs_, nullptr, 0);
    c_->VSSetConstantBuffers(0, 1, &vs_cb_);
    c_->PSSetConstantBuffers(0, 1, &ps_cb_);
    c_->PSSetShaderResources(0, 1, &ps_srv_);
    c_->PSSetSamplers(0, 1, &ps_sampler_);
    c_->IASetPrimitiveTopology(topology_);
    c_->IASetInputLayout(layout_);
    c_->IASetVertexBuffers(0, 1, &vb_, &vb_stride_, &vb_offset_);
    c_->IASetIndexBuffer(ib_, ib_format_, ib_offset_);
    for (auto *v : rtvs_)
        if (v) v->Release();
    for (IUnknown *u : std::initializer_list<IUnknown *>{dsv_, blend_, depth_, rs_, vs_, ps_, gs_, vs_cb_, ps_cb_,
                                                         ps_srv_, ps_sampler_, layout_, vb_, ib_})
        if (u) u->Release();
}

bool Compositor::init(ID3D11Device *device) {
    device_ = device;
    if (FAILED(device->CreateVertexShader(g_composite_vs, sizeof(g_composite_vs), nullptr, &vs_)) ||
        FAILED(device->CreatePixelShader(g_composite_ps, sizeof(g_composite_ps), nullptr, &ps_)))
        return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &params_))) return false;
    D3D11_BLEND_DESC b{};
    auto &rt = b.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;  // the frame keeps its own alpha
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&b, &blend_))) return false;
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&r, &raster_))) return false;
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = FALSE;
    d.StencilEnable = FALSE;
    return SUCCEEDED(device->CreateDepthStencilState(&d, &depth_));
}

void Compositor::release() {
    menu_srv_.Reset();
    menu_rtv_.Reset();
    menu_.Reset();
    depth_.Reset();
    raster_.Reset();
    blend_.Reset();
    params_.Reset();
    ps_.Reset();
    vs_.Reset();
    device_.Reset();
    width_ = height_ = 0;
}

ID3D11RenderTargetView *Compositor::begin_menu(ID3D11DeviceContext *context, UINT width, UINT height) {
    if (!device_) return nullptr;
    if (width != width_ || height != height_ || !menu_) {
        menu_srv_.Reset();
        menu_rtv_.Reset();
        menu_.Reset();
        width_ = height_ = 0;
        D3D11_TEXTURE2D_DESC t{};
        t.Width = width;
        t.Height = height;
        t.MipLevels = 1;
        t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&t, nullptr, &menu_)) ||
            FAILED(device_->CreateRenderTargetView(menu_.Get(), nullptr, &menu_rtv_)) ||
            FAILED(device_->CreateShaderResourceView(menu_.Get(), nullptr, &menu_srv_))) {
            menu_srv_.Reset();
            menu_rtv_.Reset();
            menu_.Reset();
            return nullptr;
        }
        width_ = width;
        height_ = height;
    }
    const FLOAT clear[4] = {0, 0, 0, 0};
    context->ClearRenderTargetView(menu_rtv_.Get(), clear);
    return menu_rtv_.Get();
}

bool Compositor::composite(ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer, Encoding encoding,
                           float paper_nits) {
    if (!menu_srv_) return false;
    D3D11_TEXTURE2D_DESC bb{};
    back_buffer->GetDesc(&bb);
    if (bb.Width != width_ || bb.Height != height_ || bb.SampleDesc.Count != 1) return false;
    // Made for each draw and let go at once: a view kept on the back buffer
    // would hold it, and the swap chain could not be resized.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device_->CreateRenderTargetView(back_buffer, nullptr, &rtv))) return false;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    struct {
        UINT mode;
        float paper;
        float pad[2];
    } p{static_cast<UINT>(encoding), paper_nits, {}};
    memcpy(m.pData, &p, sizeof(p));
    context->Unmap(params_.Get(), 0);

    ID3D11RenderTargetView *target = rtv.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    const FLOAT factor[4] = {};
    context->OMSetBlendState(blend_.Get(), factor, 0xFFFFFFFF);
    context->OMSetDepthStencilState(depth_.Get(), 0);
    context->RSSetState(raster_.Get());
    D3D11_VIEWPORT vp{0, 0, float(bb.Width), float(bb.Height), 0, 1};
    context->RSSetViewports(1, &vp);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(vs_.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView *srv = menu_srv_.Get();
    context->PSSetShaderResources(0, 1, &srv);
    ID3D11Buffer *cb = params_.Get();
    context->PSSetConstantBuffers(0, 1, &cb);
    context->Draw(3, 0);
    ID3D11ShaderResourceView *none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    return true;
}

}  // namespace glasir::overlay
