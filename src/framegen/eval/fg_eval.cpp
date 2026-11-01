// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// fg_eval - scores the add-on's in-between frames against the true ones.
//
// Reads the clips make_sequences.sh writes (seq/<clip>/f01.png ... f09.png,
// odd = real frames, even = true in-between frames) - or a folder the add-on's
// capture key saved (f01.png ... f05.png, five real frames in a row: the frame
// made between f01 and f03 is scored against f02) - makes each in-between
// frame on the GPU with fg::Interpolator - the same code the add-on runs - and
// compares it with the true one (PSNR over RGB; higher is better, identical
// is "inf"). "repeat" (show the earlier real frame again) is the baseline any
// method has to beat. GPU time is measured with timestamp queries.
//
//   fg_eval.exe [--clip NAME] [--out DIR] [--methods repeat,blend,motion] SEQ_DIR
//
// With --out, the first in-between frame of each clip and method is saved as
// <clip>_<method>.png for looking at.

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "fg_interp.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

// 16 bits a channel whatever the file had, so 10-bit captures keep theirs.
struct Image {
    UINT w = 0, h = 0;
    std::vector<UINT16> rgba;
};

bool load_png(IWICImagingFactory *wic, const fs::path &path, Image &img) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                              &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&img.w, &img.h)))
        return false;
    img.rgba.resize(size_t(img.w) * img.h * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, img.w * 8, static_cast<UINT>(img.rgba.size() * 2),
                                           reinterpret_cast<BYTE *>(img.rgba.data())));
}

// An 8-bit preview, as BGRA (the order WIC's PNG encoder takes).
bool save_png(IWICImagingFactory *wic, const fs::path &path, const Image &img) {
    std::vector<BYTE> bgra(img.rgba.size());
    for (size_t i = 0; i < bgra.size(); i += 4) {
        bgra[i] = BYTE(img.rgba[i + 2] >> 8);
        bgra[i + 1] = BYTE(img.rgba[i + 1] >> 8);
        bgra[i + 2] = BYTE(img.rgba[i] >> 8);
        bgra[i + 3] = BYTE(img.rgba[i + 3] >> 8);
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(wic->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(img.w, img.h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
           SUCCEEDED(frame->WritePixels(img.h, img.w * 4, static_cast<UINT>(bgra.size()), bgra.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

double psnr(const Image &a, const Image &b) {
    double sum = 0;
    const size_t n = size_t(a.w) * a.h;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
            const double d = double(a.rgba[i * 4 + c]) - double(b.rgba[i * 4 + c]);
            sum += d * d;
        }
    const double mse = sum / (double(n) * 3.0);
    return mse <= 0 ? INFINITY : 10.0 * std::log10(65535.0 * 65535.0 / mse);
}

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Query> disjoint, start, end;
};

bool make_texture(Gpu &gpu, const Image &img, ComPtr<ID3D11Texture2D> &tex, ComPtr<ID3D11ShaderResourceView> &srv) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = img.w;
    desc.Height = img.h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{img.rgba.data(), img.w * 8, 0};
    return SUCCEEDED(gpu.device->CreateTexture2D(&desc, &data, &tex)) &&
           SUCCEEDED(gpu.device->CreateShaderResourceView(tex.Get(), nullptr, &srv));
}

// Times one draw on the GPU, in milliseconds.
double timed(Gpu &gpu, const std::function<void()> &work) {
    gpu.ctx->Begin(gpu.disjoint.Get());
    gpu.ctx->End(gpu.start.Get());
    work();
    gpu.ctx->End(gpu.end.Get());
    gpu.ctx->End(gpu.disjoint.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    while (gpu.ctx->GetData(gpu.disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) YieldProcessor();
    UINT64 t0 = 0, t1 = 0;
    while (gpu.ctx->GetData(gpu.start.Get(), &t0, sizeof(t0), 0) == S_FALSE) YieldProcessor();
    while (gpu.ctx->GetData(gpu.end.Get(), &t1, sizeof(t1), 0) == S_FALSE) YieldProcessor();
    return dj.Disjoint || dj.Frequency == 0 ? -1.0 : double(t1 - t0) * 1000.0 / double(dj.Frequency);
}

struct Result {
    double psnr_sum = 0;
    int psnr_n = 0;
    bool any_inf = false;
    std::vector<double> ms;
};

}  // namespace

int wmain(int argc, wchar_t **argv) {
    std::wstring only_clip, out_dir, methods = L"repeat,blend,motion";
    bool profile = false, show_stats = false, show_layers = false, interp_profile_layers = false;
    // --adjacent: make the frame between each two frames in a row (f02 and f03
    // of a capture are what the add-on worked on) and only save it - there is
    // no true frame between them to score against.
    bool adjacent = false;
    std::vector<std::pair<std::wstring, double>> tuning;  // --set NAME VALUE
    fs::path seq_dir;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--clip" && i + 1 < argc) only_clip = argv[++i];
        else if (a == L"--out" && i + 1 < argc) out_dir = argv[++i];
        else if (a == L"--methods" && i + 1 < argc) methods = argv[++i];
        else if (a == L"--profile") profile = true;
        else if (a == L"--profile-layers") profile = true, interp_profile_layers = true;
        else if (a == L"--stats") show_stats = true;
        else if (a == L"--layers") show_layers = true;
        else if (a == L"--adjacent") adjacent = true;
        else if (a == L"--set" && i + 2 < argc) {
            tuning.emplace_back(argv[i + 1], _wtof(argv[i + 2]));
            i += 2;
        }
        else seq_dir = a;
    }
    if (seq_dir.empty()) {
        fwprintf(stderr, L"usage: fg_eval.exe [--clip NAME] [--out DIR] [--methods repeat,blend,motion] SEQ_DIR\n");
        return 2;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        fwprintf(stderr, L"no WIC\n");
        return 1;
    }

    Gpu gpu;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT dev_flags = GetEnvironmentVariableW(L"FG_D3D_DEBUG", nullptr, 0) ? D3D11_CREATE_DEVICE_DEBUG : 0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, dev_flags, levels, 3, D3D11_SDK_VERSION,
                                 &gpu.device, nullptr, &gpu.ctx))) {
        fwprintf(stderr, L"no D3D11 device\n");
        return 1;
    }
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    gpu.device->CreateQuery(&qd, &gpu.disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    gpu.device->CreateQuery(&qd, &gpu.start);
    gpu.device->CreateQuery(&qd, &gpu.end);
    {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(gpu.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad)))
            wprintf(L"GPU: %s\n", ad.Description);
    }

    std::vector<std::wstring> method_names;
    for (size_t p = 0; p <= methods.size();) {
        size_t q = methods.find(L',', p);
        if (q == std::wstring::npos) q = methods.size();
        if (q > p) method_names.push_back(methods.substr(p, q - p));
        p = q + 1;
    }
    if (!out_dir.empty()) fs::create_directories(out_dir);

    std::vector<fs::path> clips;
    for (auto &e : fs::directory_iterator(seq_dir))
        if (e.is_directory() && (only_clip.empty() || e.path().filename() == only_clip)) clips.push_back(e.path());
    std::sort(clips.begin(), clips.end());

    wprintf(L"\n%-12s", L"clip");
    for (auto &m : method_names) wprintf(L"  %10s dB %8s", m.c_str(), L"ms");
    wprintf(L"\n");

    fg::Interpolator interp;
    interp.layer_profile = interp_profile_layers;
    for (auto &[name, value] : tuning) {
        if (name == L"radius_fine") interp.search_radius_fine = int(value);
        else if (name == L"radius_coarse") interp.search_radius_coarse = int(value);
        else if (name == L"search_lambda") interp.search_lambda = float(value);
        else if (name == L"mid_lambda") interp.mid_lambda = float(value);
        else if (name == L"occlusion_cost") interp.occlusion_cost = float(value);
        else if (name == L"flow_weight") interp.flow_weight = float(value);
        else if (name == L"bad_cost") interp.bad_cost = float(value);
        else if (name == L"cut_fraction") interp.cut_fraction = float(value);
        else if (name == L"cut_detailed") interp.cut_detailed = float(value);
        else if (name == L"layer_cost") interp.layer_cost = float(value);
        else if (name == L"slow_cost") interp.slow_cost = float(value);
        else if (name == L"static_eps") interp.static_eps = float(value);
        else if (name == L"gain_deadzone") interp.gain_deadzone = float(value);
        else if (name == L"gain_deadzone_matched") interp.gain_deadzone_matched = float(value);
        else if (name == L"soft_temp") interp.soft_temp = float(value);
        else if (name == L"soft_floor") interp.soft_floor = float(value);
        else if (name == L"soft_scale") interp.soft_scale = float(value);
        else if (name == L"debug_view") interp.debug_view = int(value);
        else if (name == L"still_check") interp.still_check = float(value);
        else if (name == L"reach_plateau") interp.reach_plateau = float(value);
        else if (name == L"soft_wide") interp.soft_wide = float(value);
        else if (name == L"guess_cost") interp.guess_cost = float(value);
        else if (name == L"guess_ratio") interp.guess_ratio = float(value);
        else if (name == L"persist_max") interp.persist_max = float(value);
        else if (name == L"warp_taps") interp.warp_taps = int(value);
        else if (name == L"warp_ring") interp.warp_ring = int(value);
        else if (name == L"layer_enable") interp.layer_enable = int(value);
        else if (name == L"layer_margin") interp.layer_margin = float(value);
        else if (name == L"layer_min_bin") interp.layer_min_bin = float(value);
        else if (name == L"layer_detail") interp.layer_detail = float(value);
        else if (name == L"layer_vdiff") interp.layer_vdiff = float(value);
        else if (name == L"layer_link") interp.layer_link = float(value);
        else if (name == L"layer_min_area") interp.layer_min_area = float(value);
        else if (name == L"layer_max_area") interp.layer_max_area = float(value);
        else if (name == L"layer_fill") interp.layer_fill = float(value);
        else if (name == L"layer_band") interp.layer_band = float(value);
        else if (name == L"layer_colour") interp.layer_colour = float(value);
        else if (name == L"layer_bgfail") interp.layer_bgfail = float(value);
        else if (name == L"layer_sup") interp.layer_sup = float(value);
        else if (name == L"layer_both") interp.layer_both = int(value);
        else if (name == L"layer_kill") interp.layer_kill = float(value);
        else if (name == L"layer_void") interp.layer_void = float(value);
        else if (name == L"layer_dark") interp.layer_dark = float(value);
        else if (name == L"layer_edge") interp.layer_edge = float(value);
        else if (name == L"layer_grow_iters") interp.layer_grow_iters = int(value);
        else if (name == L"layer_grow_passes") interp.layer_grow_passes = int(value);
        else if (name == L"layer_refine") interp.layer_refine = int(value);
        else if (name == L"layer_gate") interp.layer_gate = int(value);
        else if (name == L"layer_thin") interp.layer_thin = float(value);
        else if (name == L"layer_fit") interp.layer_fit = float(value);
        else if (name == L"layer_attach") interp.layer_attach = float(value);
        else if (name == L"layer_bgslack") interp.layer_bgslack = float(value);
        else if (name == L"layer_vmax") interp.layer_vmax = float(value);
        else if (name == L"cap_fit") interp.cap_fit = float(value);
        else if (name == L"cap_cell") interp.cap_cell = float(value);
        else if (name == L"layer_wmax") interp.layer_wmax = float(value);
        else if (name == L"layer_nbmotion") interp.layer_nbmotion = float(value);
        else if (name == L"layer_inner_colour") interp.layer_inner_colour = float(value);
        else if (name == L"layer_holes") interp.layer_holes = float(value);
        else if (name == L"layer_bgnear") interp.layer_bgnear = float(value);
        else if (name == L"layer_select_gates") interp.layer_select_gates = float(value);
        else if (name == L"layer_ghost") interp.layer_ghost = float(value);
        else if (name == L"layer_bg_flat") interp.layer_bg_flat = float(value);
        else if (name == L"layer_nb_own") interp.layer_nb_own = float(value);
        else if (name == L"layer_still") interp.layer_still = float(value);
        else if (name == L"layer_select_dom") interp.layer_select_dom = float(value);
        else if (name == L"layer_merge_core") interp.layer_merge_core = float(value);
        else if (name == L"layer_hcentre") interp.layer_hcentre = float(value);
        else if (name == L"layer_merge") interp.layer_merge = float(value);
        else if (name == L"layer_refine_edge") interp.layer_refine_edge = float(value);
        else if (name == L"layer_still_pow") interp.layer_still_pow = float(value);
        else if (name == L"layer_still_dx") interp.layer_still_dx = float(value);
        else {
            fwprintf(stderr, L"unknown setting %s\n", name.c_str());
            return 2;
        }
    }
    std::vector<double> totals(method_names.size(), 0.0);
    for (auto &clip : clips) {
        std::vector<Image> frames;
        for (int i = 1;; ++i) {
            wchar_t name[16];
            swprintf(name, 16, L"f%02d.png", i);
            Image img;
            if (!fs::exists(clip / name) || !load_png(wic.Get(), clip / name, img)) break;
            frames.push_back(std::move(img));
        }
        if (frames.size() < 3) continue;
        const UINT w = frames[0].w, h = frames[0].h;

        std::string error;
        if ((interp.width() != w || interp.height() != h) && !interp.create(gpu.device.Get(), w, h, error)) {
            fprintf(stderr, "interpolator: %s\n", error.c_str());
            return 1;
        }
        std::vector<ComPtr<ID3D11Texture2D>> tex(frames.size());
        std::vector<ComPtr<ID3D11ShaderResourceView>> srv(frames.size());
        for (size_t i = 0; i < frames.size(); ++i) make_texture(gpu, frames[i], tex[i], srv[i]);

        D3D11_TEXTURE2D_DESC td{};
        tex[0]->GetDesc(&td);
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> target, staging;
        ComPtr<ID3D11RenderTargetView> rtv;
        gpu.device->CreateTexture2D(&td, nullptr, &target);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        gpu.device->CreateTexture2D(&td, nullptr, &staging);

        wprintf(L"%-12s", clip.filename().c_str());
        for (size_t m = 0; m < method_names.size(); ++m) {
            const std::wstring &method = method_names[m];
            Result r;
            const size_t step = adjacent ? 1 : 2;
            for (size_t i = 0; i + step < frames.size(); i += step) {
                const Image &truth = frames[i + (adjacent ? 0 : 1)];
                const size_t next = i + step;
                Image out;
                out.w = w;
                out.h = h;
                if (method == L"repeat") {
                    out.rgba = frames[i].rgba;
                } else {
                    fg::DrawOptions o;
                    o.method = method == L"motion" ? fg::Method::motion : fg::Method::blend;
                    // A few untimed runs first, then the timed one.
                    for (int warm = 0; warm < 3; ++warm)
                        interp.draw(gpu.ctx.Get(), srv[i].Get(), srv[next].Get(), rtv.Get(), o);
                    r.ms.push_back(timed(gpu, [&] {
                        interp.draw(gpu.ctx.Get(), srv[i].Get(), srv[next].Get(), rtv.Get(), o);
                    }));
                    if (show_stats && o.method == fg::Method::motion && interp.stats_texture()) {
                        D3D11_TEXTURE2D_DESC sd{};
                        interp.stats_texture()->GetDesc(&sd);
                        sd.BindFlags = 0;
                        sd.Usage = D3D11_USAGE_STAGING;
                        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ComPtr<ID3D11Texture2D> st;
                        gpu.device->CreateTexture2D(&sd, nullptr, &st);
                        gpu.ctx->CopyResource(st.Get(), interp.stats_texture());
                        D3D11_MAPPED_SUBRESOURCE sm{};
                        gpu.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &sm);
                        const float *v = static_cast<const float *>(sm.pData);
                        wprintf(L" [pair %zu: gain %.3f, unmatched %.3f, of detailed %.3f, detailed %.3f]", i / 2 + 1, v[0], v[1],
                                v[2], v[3]);
                        gpu.ctx->Unmap(st.Get(), 0);
                    }
                    if (show_layers && o.method == fg::Method::motion && interp.layer_texture()) {
                        D3D11_TEXTURE2D_DESC sd{};
                        interp.layer_texture()->GetDesc(&sd);
                        sd.BindFlags = 0;
                        sd.Usage = D3D11_USAGE_STAGING;
                        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ComPtr<ID3D11Texture2D> st;
                        gpu.device->CreateTexture2D(&sd, nullptr, &st);
                        gpu.ctx->CopyResource(st.Get(), interp.layer_texture());
                        D3D11_MAPPED_SUBRESOURCE sm{};
                        gpu.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &sm);
                        const float *v = static_cast<const float *>(sm.pData);
                        wprintf(L"\n  [layer %zu: active %.0f dom (%.1f,%.1f) peak2 %.0f other %.0f | root %.0f v (%.1f,%.1f) "
                                L"cells %.0f..%.0f x %.0f..%.0f area %.0f fill %.2f margin %.3f luma %.2f bg (%.1f,%.1f) | alive %.0f f %.3f e %.3f px %.0f | %.0f %.0f %.0f %.0f %.0f %.0f | cells k0 %.0f k1 %.0f bg %.0f fgpx %.0f bgpx %.0f | seedA %.0f candA %.0f seedB %.0f candB %.0f | vcost %.3f at v0 %.3f v0 (%.1f,%.1f) | cands %.0f members %.0f]",
                                i + 1, v[0], v[1], v[2], v[3], v[14], v[4], v[5], v[6], v[7], v[9], v[8], v[10], v[11], v[12],
                                v[13], v[23], v[24], v[25], v[15], v[20], v[21], v[22], v[26], v[27], v[28], v[29], v[30], v[31], v[32], v[33], v[34], v[35], v[36], v[37], v[38], v[39], v[40], v[41], v[42], v[43], v[44], v[46], v[45]);
                        gpu.ctx->Unmap(st.Get(), 0);
                        if (getenv("FG_LAYER_COMPS")) {
                            // Every component in the running (area >= 12): area, box, black cells, mean margin, motion.
                            D3D11_BUFFER_DESC bd{};
                            interp.layer_counters()->GetDesc(&bd);
                            bd.BindFlags = 0;
                            bd.MiscFlags = 0;
                            bd.Usage = D3D11_USAGE_STAGING;
                            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            ComPtr<ID3D11Buffer> sb;
                            gpu.device->CreateBuffer(&bd, nullptr, &sb);
                            gpu.ctx->CopyResource(sb.Get(), interp.layer_counters());
                            D3D11_MAPPED_SUBRESOURCE cm{};
                            gpu.ctx->Map(sb.Get(), 0, D3D11_MAP_READ, 0, &cm);
                            const UINT *u = static_cast<const UINT *>(cm.pData);
                            const UINT cells = interp.grid_w() * interp.grid_h();
                            const UINT min_area = std::max(1, atoi(getenv("FG_LAYER_COMPS")));
                            for (UINT c = 0; c < cells; ++c) {
                                const UINT *a = u + size_t(c) * 9;
                                if (a[0] < (min_area > 1 ? min_area : 12u)) continue;
                                const int x0 = int(0xffff - a[1]), y0 = int(0xffff - a[2]), x1 = int(a[3]), y1 = int(a[4]);
                                const float vx = float(int(a[7])) / (16.0f * a[0]) * 2.0f, vy = float(int(a[8])) / (16.0f * a[0]) * 2.0f;
                                const float fill = float(a[0]) / float((x1 - x0 + 1) * (y1 - y0 + 1));
                                wprintf(L"\n    comp %u: area %u cells %d..%d x %d..%d fill %.2f black %u margin %.3f v (%.1f,%.1f)", c, a[0], x0, x1,
                                        y0, y1, fill, a[5], a[6] / 1000.0f / a[0], vx, vy);
                            }
                            gpu.ctx->Unmap(sb.Get(), 0);
                        }
                        if (getenv("FG_LAYER_VCOST")) {
                            // L_REFINE's cost per offset around the cells' mean (every second row and column).
                            D3D11_TEXTURE2D_DESC vd{};
                            interp.layer_vcost()->GetDesc(&vd);
                            vd.BindFlags = 0;
                            vd.Usage = D3D11_USAGE_STAGING;
                            vd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            ComPtr<ID3D11Texture2D> vt;
                            gpu.device->CreateTexture2D(&vd, nullptr, &vt);
                            gpu.ctx->CopyResource(vt.Get(), interp.layer_vcost());
                            D3D11_MAPPED_SUBRESOURCE vm{};
                            gpu.ctx->Map(vt.Get(), 0, D3D11_MAP_READ, 0, &vm);
                            for (UINT y = 0; y < vd.Height; y += 2) {
                                wprintf(L"\n    vc %+3d:", int(y) - 16);
                                const float *row = reinterpret_cast<const float *>(static_cast<BYTE *>(vm.pData) + y * vm.RowPitch);
                                for (UINT x = 0; x < vd.Width; x += 2) wprintf(L" %3.0f", row[x] * 1000.0f);
                            }
                            gpu.ctx->Unmap(vt.Get(), 0);
                        }
                        if (getenv("FG_LAYER_ARGS")) {
                            D3D11_BUFFER_DESC bd{};
                            interp.layer_args2()->GetDesc(&bd);
                            bd.BindFlags = 0;
                            bd.MiscFlags = 0;
                            bd.Usage = D3D11_USAGE_STAGING;
                            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            ComPtr<ID3D11Buffer> sb;
                            gpu.device->CreateBuffer(&bd, nullptr, &sb);
                            gpu.ctx->CopyResource(sb.Get(), interp.layer_args2());
                            gpu.ctx->Map(sb.Get(), 0, D3D11_MAP_READ, 0, &sm);
                            const UINT *u = static_cast<const UINT *>(sm.pData);
                            wprintf(L" args");
                            for (int k = 0; k < 12; ++k) wprintf(L" %u", u[k]);
                            gpu.ctx->Unmap(sb.Get(), 0);
                        }
                    }
                    gpu.ctx->CopyResource(staging.Get(), target.Get());
                    D3D11_MAPPED_SUBRESOURCE map{};
                    gpu.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
                    out.rgba.resize(size_t(w) * h * 4);
                    for (UINT y = 0; y < h; ++y)
                        memcpy(out.rgba.data() + size_t(y) * w * 4, static_cast<BYTE *>(map.pData) + size_t(y) * map.RowPitch,
                               size_t(w) * 8);
                    gpu.ctx->Unmap(staging.Get(), 0);
                }
                const double p = psnr(out, truth);
                if (std::isinf(p))
                    r.any_inf = true;
                else
                    r.psnr_sum += p, ++r.psnr_n;
                if (!out_dir.empty()) {
                    // The first pair as <clip>_<method>.png, the others with their number.
                    std::wstring name = clip.filename().wstring() + L"_" + method;
                    if (adjacent)
                        name += L"_adj" + std::to_wstring(i + 1) + std::to_wstring(i + 2);
                    else if (i > 0)
                        name += L"_" + std::to_wstring(i / 2 + 1);
                    save_png(wic.Get(), fs::path(out_dir) / (name + L".png"), out);
                }
            }
            const double avg = r.psnr_n ? r.psnr_sum / r.psnr_n : INFINITY;
            totals[m] += std::isinf(avg) ? 99.0 : avg;
            std::sort(r.ms.begin(), r.ms.end());
            const double ms = r.ms.empty() ? 0.0 : r.ms[r.ms.size() / 2];
            wprintf(L"  %10.2f dB %8.3f", avg, ms);
        }
        wprintf(L"\n");
        if (profile) {
            // Each stage of one motion draw, from a timestamp after each.
            std::vector<ComPtr<ID3D11Query>> marks;
            std::vector<std::string> names;
            D3D11_QUERY_DESC tq{D3D11_QUERY_TIMESTAMP, 0};
            interp.on_stage = [&](const char *name) {
                ComPtr<ID3D11Query> q;
                gpu.device->CreateQuery(&tq, &q);
                gpu.ctx->End(q.Get());
                marks.push_back(q);
                names.push_back(name);
            };
            fg::DrawOptions o;
            gpu.ctx->Begin(gpu.disjoint.Get());
            gpu.ctx->End(gpu.start.Get());
            interp.draw(gpu.ctx.Get(), srv[0].Get(), srv[2].Get(), rtv.Get(), o);
            gpu.ctx->End(gpu.disjoint.Get());
            interp.on_stage = nullptr;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            while (gpu.ctx->GetData(gpu.disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) YieldProcessor();
            UINT64 prev = 0;
            while (gpu.ctx->GetData(gpu.start.Get(), &prev, sizeof(prev), 0) == S_FALSE) YieldProcessor();
            wprintf(L"%-12s", L"");
            for (size_t i = 0; i < marks.size(); ++i) {
                UINT64 stamp = 0;
                while (gpu.ctx->GetData(marks[i].Get(), &stamp, sizeof(stamp), 0) == S_FALSE) YieldProcessor();
                wprintf(L"  %S %.3f", names[i].c_str(), double(stamp - prev) * 1000.0 / double(dj.Frequency));
                prev = stamp;
            }
            wprintf(L"\n");
        }
    }
    {
        ComPtr<ID3D11InfoQueue> iq;
        if (dev_flags && SUCCEEDED(gpu.device.As(&iq))) {
            const UINT64 nm = iq->GetNumStoredMessages();
            for (UINT64 k = 0; k < nm && k < 20; ++k) {
                SIZE_T len = 0;
                iq->GetMessage(k, nullptr, &len);
                std::vector<char> buf(len);
                auto *m = reinterpret_cast<D3D11_MESSAGE *>(buf.data());
                if (SUCCEEDED(iq->GetMessage(k, m, &len))) printf("D3D: %.*s\n", int(m->DescriptionByteLength), m->pDescription);
            }
        }
    }
    wprintf(L"%-12s", L"mean");
    for (size_t m = 0; m < method_names.size(); ++m) wprintf(L"  %10.2f dB %8s", totals[m] / std::max<size_t>(1, clips.size()), L"");
    wprintf(L"\n");
    return 0;
}
