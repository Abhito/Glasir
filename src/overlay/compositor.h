// SPDX-License-Identifier: MIT
// The menu's texture, and putting it onto a frame (composite.hlsl).
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace glasir::overlay {

enum class Encoding { Sdr = 0, Pq = 1, ScRgb = 2 };

// Every Direct3D 11 state the menu's drawing touches, put back as it was.
class StateBackup {
public:
    explicit StateBackup(ID3D11DeviceContext *context);
    ~StateBackup();
    StateBackup(const StateBackup &) = delete;
    StateBackup &operator=(const StateBackup &) = delete;

private:
    ID3D11DeviceContext *c_;
    ID3D11RenderTargetView *rtvs_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView *dsv_ = nullptr;
    ID3D11BlendState *blend_ = nullptr;
    FLOAT blend_factor_[4] = {};
    UINT sample_mask_ = 0;
    ID3D11DepthStencilState *depth_ = nullptr;
    UINT stencil_ref_ = 0;
    ID3D11RasterizerState *rs_ = nullptr;
    UINT viewport_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT scissor_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11VertexShader *vs_ = nullptr;
    ID3D11PixelShader *ps_ = nullptr;
    ID3D11GeometryShader *gs_ = nullptr;
    ID3D11Buffer *vs_cb_ = nullptr, *ps_cb_ = nullptr;
    ID3D11ShaderResourceView *ps_srv_ = nullptr;
    ID3D11SamplerState *ps_sampler_ = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout *layout_ = nullptr;
    ID3D11Buffer *vb_ = nullptr, *ib_ = nullptr;
    UINT vb_stride_ = 0, vb_offset_ = 0, ib_offset_ = 0;
    DXGI_FORMAT ib_format_ = DXGI_FORMAT_UNKNOWN;
};

class Compositor {
public:
    bool init(ID3D11Device *device);
    void release();

    // The menu's texture at this size (made again when it changes), as a
    // render target for Dear ImGui; cleared. nullptr if it cannot be made.
    ID3D11RenderTargetView *begin_menu(ID3D11DeviceContext *context, UINT width, UINT height);

    // Draws the menu's texture over back_buffer. The caller backs the state up.
    bool composite(ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer, Encoding encoding, float paper_nits);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> params_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> menu_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> menu_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> menu_srv_;
    UINT width_ = 0, height_ = 0;
};

}  // namespace glasir::overlay
