// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// fg_interp.h - the frame between two real frames.
//
// Shared by the add-on (glasir_framegen.cpp) and its offline evaluator
// (eval/fg_eval.cpp), so what is measured is what runs. The caller owns the
// device context and its state: draw() changes pipeline state freely (within
// the slots StateBackup in glasir_framegen.cpp covers), so the add-on saves and
// restores around it.
//
// Two methods:
//   blend   a 50/50 mix of the two frames (blend.hlsl)
//   motion  motion-compensated (flow.hlsl): a luma pyramid of both frames, the
//           motion both ways found coarse to fine, the motion at the frame in
//           between picked from those, and each pixel drawn from both frames
//           along the motion that makes them agree there.

#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>

#include "blend_ps.h"
#include "blend_vs.h"
#include "lumadown_cs.h"
#include "luma0_cs.h"
#include "median_cs.h"
#include "mid_cs.h"
#include "search_cs.h"
#include "stats_cs.h"
#include "ratio_cs.h"
#include "mgain_cs.h"
#include "cut_cs.h"
#include "warp_ps.h"
#include "ldom_cs.h"
#include "lmargin_cs.h"
#include "llink_cs.h"
#include "larea_cs.h"
#include "lselect_cs.h"
#include "lcell_cs.h"
#include "lspread_cs.h"
#include "linit_cs.h"
#include "lgrow_cs.h"
#include "lfinal_cs.h"
#include "ldone_cs.h"
#include "lbg_cs.h"
#include "lclass_cs.h"
#include "lrefine_cs.h"
#include "lpick_cs.h"

namespace fg {

using Microsoft::WRL::ComPtr;

enum class Method { blend, motion };

struct DrawOptions {
    Method method = Method::motion;
    float t = 0.5f;             // 0 = frame a, 1 = frame b
    bool marker = false;        // debug square in the top-left corner
    float marker_level = 0.5f;  // its brightness in the target's encoding
};

class Interpolator {
public:
    static constexpr int kMaxLevels = 7;
    static constexpr int kBlock = 8;
    static constexpr int kMidBlock = 8;  // flow.hlsl's MID_BLOCK (4 was tried: noisier, worse on the clips)

    bool create(ID3D11Device *device, UINT width, UINT height, std::string &error) {
        release();
        device_ = device;
        width_ = width;
        height_ = height;
        HRESULT hr = S_OK;
        auto check = [&](HRESULT r, const char *what) {
            if (FAILED(r) && SUCCEEDED(hr)) {
                hr = r;
                error = std::string(what) + ": " + hex(r);
            }
        };
        check(device->CreateVertexShader(g_blend_vs, sizeof(g_blend_vs), nullptr, &vs_), "vertex shader");
        check(device->CreatePixelShader(g_blend_ps, sizeof(g_blend_ps), nullptr, &blend_ps_), "blend shader");
        check(device->CreatePixelShader(g_warp_ps, sizeof(g_warp_ps), nullptr, &warp_ps_), "warp shader");
        check(device->CreateComputeShader(g_luma0_cs, sizeof(g_luma0_cs), nullptr, &luma0_cs_), "luma shader");
        check(device->CreateComputeShader(g_lumadown_cs, sizeof(g_lumadown_cs), nullptr, &lumadown_cs_), "pyramid shader");
        check(device->CreateComputeShader(g_search_cs, sizeof(g_search_cs), nullptr, &search_cs_), "search shader");
        check(device->CreateComputeShader(g_median_cs, sizeof(g_median_cs), nullptr, &median_cs_), "median shader");
        check(device->CreateComputeShader(g_mid_cs, sizeof(g_mid_cs), nullptr, &mid_cs_), "mid shader");
        check(device->CreateComputeShader(g_stats_cs, sizeof(g_stats_cs), nullptr, &stats_cs_), "stats shader");
        check(device->CreateComputeShader(g_cut_cs, sizeof(g_cut_cs), nullptr, &cut_cs_), "cut shader");
        check(device->CreateComputeShader(g_ratio_cs, sizeof(g_ratio_cs), nullptr, &ratio_cs_), "ratio shader");
        check(device->CreateComputeShader(g_mgain_cs, sizeof(g_mgain_cs), nullptr, &mgain_cs_), "gain shader");
        check(device->CreateComputeShader(g_ldom_cs, sizeof(g_ldom_cs), nullptr, &ldom_cs_), "layer shader");
        check(device->CreateComputeShader(g_lmargin_cs, sizeof(g_lmargin_cs), nullptr, &lmargin_cs_), "layer shader");
        check(device->CreateComputeShader(g_llink_cs, sizeof(g_llink_cs), nullptr, &llink_cs_), "layer shader");
        check(device->CreateComputeShader(g_larea_cs, sizeof(g_larea_cs), nullptr, &larea_cs_), "layer shader");
        check(device->CreateComputeShader(g_lselect_cs, sizeof(g_lselect_cs), nullptr, &lselect_cs_), "layer shader");
        check(device->CreateComputeShader(g_lcell_cs, sizeof(g_lcell_cs), nullptr, &lcell_cs_), "layer shader");
        check(device->CreateComputeShader(g_lspread_cs, sizeof(g_lspread_cs), nullptr, &lspread_cs_), "layer shader");
        check(device->CreateComputeShader(g_linit_cs, sizeof(g_linit_cs), nullptr, &linit_cs_), "layer shader");
        check(device->CreateComputeShader(g_lgrow_cs, sizeof(g_lgrow_cs), nullptr, &lgrow_cs_), "layer shader");
        check(device->CreateComputeShader(g_lfinal_cs, sizeof(g_lfinal_cs), nullptr, &lfinal_cs_), "layer shader");
        check(device->CreateComputeShader(g_ldone_cs, sizeof(g_ldone_cs), nullptr, &ldone_cs_), "layer shader");
        check(device->CreateComputeShader(g_lbg_cs, sizeof(g_lbg_cs), nullptr, &lbg_cs_), "layer shader");
        check(device->CreateComputeShader(g_lclass_cs, sizeof(g_lclass_cs), nullptr, &lclass_cs_), "layer shader");
        check(device->CreateComputeShader(g_lrefine_cs, sizeof(g_lrefine_cs), nullptr, &lrefine_cs_), "layer shader");
        check(device->CreateComputeShader(g_lpick_cs, sizeof(g_lpick_cs), nullptr, &lpick_cs_), "layer shader");
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(Params);
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&cbd, nullptr, &cb_), "constant buffer");
        cbd.ByteWidth = sizeof(BlendParams);
        check(device->CreateBuffer(&cbd, nullptr, &blend_cb_), "constant buffer");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sd, &linear_), "sampler");

        // The pyramid: level 0 at half the frame's resolution, each next one
        // half of that, down to about 16 pixels.
        UINT w = (width + 1) / 2, h = (height + 1) / 2;
        levels_ = 0;
        while (levels_ < kMaxLevels && w >= 16 && h >= 16) {
            Level &l = level_[levels_];
            l.w = w;
            l.h = h;
            l.gw = (w + kBlock - 1) / kBlock;
            l.gh = (h + kBlock - 1) / kBlock;
            for (int f = 0; f < 2; ++f) check(make(w, h, DXGI_FORMAT_R16_FLOAT, l.luma[f]), "luma level");
            check(make(l.gw, l.gh, DXGI_FORMAT_R32G32_FLOAT, l.fwd), "field");
            check(make(l.gw, l.gh, DXGI_FORMAT_R32G32_FLOAT, l.bwd), "field");
            check(make(l.gw, l.gh, DXGI_FORMAT_R32G32_FLOAT, l.raw), "field");
            ++levels_;
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
        // A frame too small for two levels gets the blend only (a window
        // squeezed to a sliver): not an error, it comes back on the next size.
        motion_ok_ = levels_ >= 2;
        if (motion_ok_) {
            mid_w_ = (level_[0].w + kMidBlock - 1) / kMidBlock;
            mid_h_ = (level_[0].h + kMidBlock - 1) / kMidBlock;
            check(make(mid_w_, mid_h_, DXGI_FORMAT_R32G32_FLOAT, mid_), "mid field");
            check(make(mid_w_, mid_h_, DXGI_FORMAT_R32G32_FLOAT, mid_raw_), "mid field");
            check(make(mid_w_, mid_h_, DXGI_FORMAT_R32G32_FLOAT, mid_cost_), "mid cost");
            check(make(level_[0].gw, level_[0].gh, DXGI_FORMAT_R32_FLOAT, ratio_), "ratios");
            check(make(4, 1, DXGI_FORMAT_R32_FLOAT, stats_), "stats");
            // Motion layers.
            check(make(kLayerRecord, 1, DXGI_FORMAT_R32_FLOAT, lrec_), "layer record");
            check(make(mid_w_, mid_h_, DXGI_FORMAT_R32_FLOAT, lcand_), "layer cells");
            check(make(mid_w_, mid_h_, DXGI_FORMAT_R32_UINT, llabel_), "layer labels");
            check(make(mid_w_, mid_h_, DXGI_FORMAT_R32_UINT, lcls_), "layer classes");
            check(make(33, 33, DXGI_FORMAT_R32_FLOAT, lvcost_), "layer motion costs");
            for (auto &x : lbg_) check(make((mid_w_ + 7) / 8, (mid_h_ + 7) / 8, DXGI_FORMAT_R32G32B32A32_FLOAT, x), "layer background");
            for (auto &x : lsum_) check(make(mid_w_, mid_h_, DXGI_FORMAT_R32G32B32A32_FLOAT, x), "layer colours");
            for (auto &x : lref_) check(make(mid_w_, mid_h_, DXGI_FORMAT_R32G32B32A32_FLOAT, x), "layer colours");
            for (auto &x : lmask_) check(make(width, height, DXGI_FORMAT_R32_UINT, x), "layer mask");
            check(make_raw((mid_w_ * mid_h_ * 9 + 32) * 4, false, lacc_), "layer counters");
            check(make_raw(32 * 4, true, largs_), "layer dispatch");
            check(make_raw(32 * 4, true, largs2_), "layer dispatch");
        }
        if (FAILED(hr)) {
            release();
            return false;
        }
        return true;
    }

    void release() {
        vs_.Reset();
        blend_ps_.Reset();
        warp_ps_.Reset();
        luma0_cs_.Reset();
        lumadown_cs_.Reset();
        search_cs_.Reset();
        median_cs_.Reset();
        mid_cs_.Reset();
        stats_cs_.Reset();
        cut_cs_.Reset();
        ratio_cs_.Reset();
        mgain_cs_.Reset();
        for (ComPtr<ID3D11ComputeShader> *c : std::initializer_list<ComPtr<ID3D11ComputeShader> *>{&ldom_cs_, &lmargin_cs_, &llink_cs_, &larea_cs_, &lselect_cs_, &lcell_cs_, &lspread_cs_, &linit_cs_,
                        &lgrow_cs_, &lfinal_cs_, &ldone_cs_, &lbg_cs_, &lclass_cs_, &lrefine_cs_, &lpick_cs_})
            c->Reset();
        lrec_ = Surface{};
        lcand_ = Surface{};
        llabel_ = Surface{};
        lcls_ = Surface{};
        lvcost_ = Surface{};
        for (auto &x : lbg_) x = Surface{};
        for (auto &x : lsum_) x = Surface{};
        for (auto &x : lref_) x = Surface{};
        for (auto &x : lmask_) x = Surface{};
        lacc_ = RawBuffer{};
        largs_ = RawBuffer{};
        largs2_ = RawBuffer{};
        cb_.Reset();
        blend_cb_.Reset();
        linear_.Reset();
        for (auto &l : level_) l = Level{};
        mid_ = Surface{};
        mid_raw_ = Surface{};
        mid_cost_ = Surface{};
        ratio_ = Surface{};
        stats_ = Surface{};
        levels_ = 0;
        motion_ok_ = false;
        device_.Reset();
        width_ = height_ = 0;
    }

    UINT width() const { return width_; }
    // The last motion draw's [brightness gain from a to b, share of cells that
    // matched nothing, the same of cells with detail, share with detail], for
    // the evaluator.
    ID3D11Texture2D *stats_texture() const { return stats_.tex.Get(); }
    // The motion layer's record (flow.hlsl LR_*), kLayerRecord floats, for the evaluator.
    ID3D11Texture2D *layer_texture() const { return lrec_.tex.Get(); }
    static constexpr UINT kLayerRecord = 64;
    ID3D11Buffer *layer_args2() const { return largs2_.buf.Get(); }
    ID3D11Texture2D *layer_vcost() const { return lvcost_.tex.Get(); }  // L_REFINE's costs, 33 x 33 (evaluator debug)
    ID3D11Buffer *layer_counters() const { return lacc_.buf.Get(); }  // ACC_STRIDE uints per cell (component roots)
    UINT grid_w() const { return mid_w_; }
    UINT grid_h() const { return mid_h_; }
    UINT height() const { return height_; }

    // Draws the frame at o.t between a and b into target. a, b and target are
    // width x height; a and b are shader resources, target a render target.
    void draw(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b,
              ID3D11RenderTargetView *target, const DrawOptions &o) {
        if (o.method == Method::motion && motion_ok_) {
            find_motion(ctx, a, b, o.t);
            if (layer_enable) {
                find_layers(ctx, a, b, o.t);
                stage("layers");
            }
            draw_warp(ctx, a, b, target, o);
            stage("warp");
        } else {
            draw_blend(ctx, a, b, target, o);
        }
    }

    // Tuning, for the evaluator.
    int search_radius_fine = 4;    // the two finest levels
    int search_radius_coarse = 8;  // the others
    float search_lambda = 0.02f;   // cost per pixel of offset, in luma SAD units (64 pixels)
    float mid_lambda = 0.05f;      // how much "no motion" must win by at the frame in between
    float occlusion_cost = 0.08f;  // drawing a pixel from one frame alone (RGB difference units)
    float flow_weight = 0.005f;    // per pixel that frame's own motion is off from the one used
    float bad_cost = 0.03f;        // a cell of the frame in between that matched nothing (luma per pixel)
    float cut_fraction = 0.2f;     // this share of such cells is a cut: a is shown again
    float cut_detailed = 0.4f;     // ... or this share of the cells with detail
    float layer_cost = 0.2f;       // near something still, bringing something else where a and b show the same costs this ...
    float persist_max = 0.2f;      // ... and nothing from this difference between a and b there up
    float slow_cost = 1.0f;        // drawing the scenery from one frame onto something slow beside it costs this (0 = off)
    float static_eps = 0.005f;     // a and b this close all around a pixel: it stands still
    float gain_deadzone = 0.04f;   // a brightness change between the frames this small is none (first guess)
    float gain_deadzone_matched = 0.01f;  // the same, measured on matched content
    float soft_temp = 0.04f;       // mixing of a pixel's candidates by cost, at most (0 = the cheapest only)
    float soft_floor = 0.005f;     // ... where the best candidate fits perfectly
    float soft_scale = 1.0f;       // ... per unit of the best candidate's cost
    int debug_view = 0;            // 1 = colour pixels by the options used (evaluator)
    float still_check = 0.03f;     // a one-frame option is ruled out where that frame's pixel stands still (0 = off)
    float reach_plateau = 1.0f;    // a cell's full say in a pixel's mix reaches this far (cells), then fades out
    float soft_wide = 0.15f;       // soft_temp where no option explains the pixel (lightning, sparks)
    float guess_cost = 0.5f;       // search: what the search around no motion finds wins over what the search around
                                   // the coarser estimate found if it fits better by this (luma SAD; <0 = no 2nd search)
    float guess_ratio = 0.5f;      // ... and at most this share as badly
    int warp_taps = 5;             // pixels a candidate is compared on: 1 or 5
    int warp_ring = 3;             // cells whose motion a pixel tries: 2 (2x2) or 3 (3x3)
    // Motion layers (flow.hlsl "Motion layers"): a compact object whose motion the camera's does not explain,
    // grown into a pixel mask and drawn over the frame from one or both frames along its own motion.
    int layer_enable = 1;          // 0 = off (exactly the frame without layers)
    float layer_margin = 0.02f;    // the dominant motion fails on a minority cell by this (luma per pixel)
    float layer_min_bin = 12.0f;   // a second motion: this many cells in one 8 px bin
    float layer_detail = 0.004f;   // flatter cells do not vote in the histogram
    float layer_vdiff = 6.0f;      // a minority cell moves this far from the dominant motion (full-res px)
    float layer_link = 8.0f;       // neighbouring minority cells this close in motion link
    float layer_min_area = 12.0f;  // a layer has this many cells ...
    float layer_max_area = 1500.0f;  // ... at most this many
    float layer_fill = 0.25f;      // ... and fills this share of its box
    float layer_band = 4.0f;       // the mask grows this many cells beyond the layer's
    float layer_colour = 0.04f;    // a pixel joins this much closer to the layer's colour than the background's (RGB sum)
    float layer_bgfail = 0.06f;    // ... and not carried by the dominant motion (its 5-tap cost above this)
    float layer_sup = 100.0f;      // one-frame coverage within this of the other frame's: the nearer frame only (px); >= 100: always
    int layer_both = 2;            // where both frames cover: 0 = a and b along the layer motion, 1 = WarpPS own, 2 = nearer frame
    float layer_kill = 0.3f;       // deletion vote: dies under this times the expected share
    float layer_void = 0.05f;      // ... void under this expected share (the compact-minority prior stands)
    float layer_dark = 0.03f;      // a darker layer (mean luma) is none (black bars)
    float layer_edge = 0.04f;      // layer and background colours this close: no growing there
    int layer_grow_iters = 40;     // flood steps per grow pass
    int layer_grow_passes = 3;     // grow passes (tiles hand the flood on between them)
    bool layer_profile = false;    // evaluator: a timestamp after each layer pass
    int layer_refine = 1;          // the layer's motion searched around its cells' mean (0 = the mean)
    int layer_gate = 7;            // 2 = replace what the frame drew from both frames with the background's motion; 7 = also one-frame options inside both masks for a nearly still layer
    float layer_fit = 0.04f;       // a band pixel the layer's motion carries onto the other frame this well joins (0 = off)
    float layer_attach = 16.0f;    // one-frame coverage farther than this from both frames' coverage is not drawn (px; 0 = off)
    float layer_thin = 4.0f;       // one-frame coverage thicker than 2 x this + 1 px is not drawn (0 = off)
    float layer_bgslack = 1.5f;  // the background's motion tried this many px off too before a pixel counts as not carried by it (0 = off)
    float layer_vmax = 40.0f;  // no layer when it moves faster than this on screen (px per frame): not the character the camera follows (0 = off)
    float cap_fit = 0.0f;   // WarpPS: a both-frames option this cheap explains the pixel (soft_temp, not soft_wide; 0 = off;
                                 // 0.08 thinned the faint orbit outline a little but wiped a fast spear head's ghost - off)
    float cap_cell = 0.0f;  // WarpPS: ... only where the cell's own motion cost is under this (0 = no check; tried 0.03)
    float layer_wmax = 40.0f;  // no layer when it is wider than this many cells (a close-up body, not a head; 0 = off)
    float layer_nbmotion = 1.0f;  // 1 = a rim/band pixel a neighbour cell's motion carries is not the layer's (0 = off)
    float layer_inner_colour = 1.0f;  // 1 = inner layer cells need the layer colour too (0 = all but background-like)
    float layer_holes = 10.0f;  // a pixel a frame's mask encloses within this many px left/right/up/down joins it (0 = off)
    float layer_select_gates = 1.0f;  // 1 = layer_vmax / layer_wmax rule a component out of the choice (the next one may win) instead of killing the layer;
                                     // 0 killed the layer on a fast turn whenever a far pillar's parallax won (20261029-181808-872)
    float layer_still = 10.0f;  // L_SELECT: a component faster than this on screen (px per frame) scores this / its speed: the camera
                               // follows the player, NPCs walking beside him cross the screen (20261029-1854*; 0 = off)
    float layer_still_pow = 2.0f;  // ... to this power
    float layer_still_dx = 100.0f;  // ... counting from this far off the middle across (px, full 150 px further; < 0 = everywhere): in the middle the
                                    // faster part is as often the player's own head running ahead of the camera
    float layer_select_dom = 6.0f;  // L_SELECT: layer_merge, layer_still and layer_hcentre only while the dominant motion is at least this (px per frame;
                                    // the camera follows the player - in fights they pasted blocks into fire and lightning; 0 = always)
    float layer_merge_core = 1.0f;  // L_SELECT: 1 = only a slow core near the middle across takes fragments (0 = any)
    float layer_hcentre = 250.0f;  // L_SELECT: a component farther than this (px) from the middle across scores less (0 = off)
    float layer_merge = 2.0f;  // L_SELECT: components this many empty cells apart moving alike are scored and drawn as one (a head in fragments; 0 = off)
    float layer_refine_edge = 1.0f;  // L_PICK: the cells' mean when 1 = the layer has no inner cells (the search picked v0 - 16 px), 2 = also the best on the search's edge (0 = off)
    float layer_nb_own = 1.0f;  // 1 = layer_nbmotion does not ask the layer's own cells (a head turning on a running body)
    float layer_bg_flat = 0.5f;  // L_BG: a cell with little detail counts this much towards the background's motion (a detailed one 1;
                                 // 0: in a fast turn the blurred floor had none, the head's cells were the 'background' around it)
    float layer_ghost = 1.0f;  // WarpPS: off the layer, an option not moving with it whose sample in a frame lands on the layer there pays this (0 = off)
    float layer_bgnear = 0.3f;  // the layer's gate counts an option within this share of the background's speed past the layer of its motion, if more than layer_vdiff (a fast turn: the model is 10-15 px off; 0 = off)
    // Called after each stage of a motion draw (the evaluator times them).
    std::function<void(const char *)> on_stage;

private:
    // flow.hlsl's constant buffer.
    struct Params {
        INT src_w, src_h, grid_w, grid_h;
        INT radius, has_pred, pred_w, pred_h;
        FLOAT lambda, t, inv_w, inv_h;
        FLOAT marker, marker_level, occlusion_cost, flow_weight;
        INT swap;
        FLOAT bad_cost, cut_fraction, static_eps;
        FLOAT gain_deadzone, soft_temp;
        INT warp_taps, warp_ring;
        FLOAT soft_floor, soft_scale;
        INT debug_view;
        FLOAT still_check;
        FLOAT reach_plateau, soft_wide;
        FLOAT guess_cost, guess_ratio;
        FLOAT cut_detailed, layer_cost, persist_max, slow_cost;
        INT layer_enable;
        FLOAT layer_margin, layer_min_bin, layer_detail;
        FLOAT layer_vdiff, layer_link, layer_min_area, layer_max_area;
        FLOAT layer_fill, layer_band, layer_colour, layer_bgfail;
        FLOAT layer_sup;
        INT layer_both;
        FLOAT layer_kill, layer_void;
        FLOAT layer_dark, layer_edge;
        INT layer_grow_iters;
        INT layer_refine;
        INT layer_gate;
        FLOAT layer_thin;
        FLOAT layer_fit;
        FLOAT layer_attach;
        FLOAT layer_bgslack;
        FLOAT layer_vmax;
        FLOAT cap_fit;
        FLOAT cap_cell;
        FLOAT layer_wmax;
        FLOAT layer_nbmotion;
        FLOAT layer_inner_colour;
        FLOAT layer_holes;
        FLOAT layer_bgnear;
        FLOAT layer_select_gates;
        FLOAT layer_ghost;
        FLOAT layer_bg_flat;
        FLOAT layer_nb_own;
        FLOAT layer_still;
        FLOAT layer_hcentre;
        FLOAT layer_merge;
        FLOAT layer_refine_edge;
        FLOAT layer_still_pow;
        FLOAT layer_still_dx;
        FLOAT layer_select_dom;
        FLOAT layer_merge_core;
        FLOAT pad_end[3];
    };
    static_assert(sizeof(Params) % 16 == 0, "constant buffers come in 16-byte registers");
    // blend.hlsl's.
    struct BlendParams {
        float weight, marker, marker_level, pad;
    };

    struct Surface {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
    };
    struct Level {
        UINT w = 0, h = 0;    // pixels
        UINT gw = 0, gh = 0;  // 8x8 cells
        Surface luma[2];      // frame a, frame b
        Surface fwd, bwd;     // a -> b and b -> a, median-filtered
        Surface raw;          // a field before its median
    };

    static std::string hex(HRESULT hr) {
        char text[16];
        snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
        return text;
    }

    struct RawBuffer {
        ComPtr<ID3D11Buffer> buf;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
    };

    // A raw (byte address) buffer; indirect: also the arguments of DispatchIndirect.
    HRESULT make_raw(UINT bytes, bool indirect, RawBuffer &r) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (indirect ? 0 : D3D11_BIND_SHADER_RESOURCE);
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | (indirect ? D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS : 0);
        HRESULT hr = device_->CreateBuffer(&d, nullptr, &r.buf);
        if (SUCCEEDED(hr)) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = DXGI_FORMAT_R32_TYPELESS;
            u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            u.Buffer.NumElements = bytes / 4;
            u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            hr = device_->CreateUnorderedAccessView(r.buf.Get(), &u, &r.uav);
        }
        if (SUCCEEDED(hr) && !indirect) {
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_R32_TYPELESS;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            v.BufferEx.NumElements = bytes / 4;
            v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            hr = device_->CreateShaderResourceView(r.buf.Get(), &v, &r.srv);
        }
        return hr;
    }

    HRESULT make(UINT w, UINT h, DXGI_FORMAT format, Surface &s) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &s.tex);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(s.tex.Get(), nullptr, &s.srv);
        if (SUCCEEDED(hr)) hr = device_->CreateUnorderedAccessView(s.tex.Get(), nullptr, &s.uav);
        return hr;
    }

    void set_params(ID3D11DeviceContext *ctx, const Params &p) { ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &p, 0, 0); }

    static constexpr UINT kInputs = 12, kOutputs = 6;

    // One compute pass: up to five inputs, two outputs. The outputs are
    // unbound first and the inputs bound before them, so no texture is ever an
    // input and an output at once (D3D11 would quietly drop the input).
    void dispatch(ID3D11DeviceContext *ctx, ID3D11ComputeShader *cs, std::initializer_list<ID3D11ShaderResourceView *> in,
                  std::initializer_list<ID3D11UnorderedAccessView *> out, UINT gx, UINT gy) {
        ID3D11UnorderedAccessView *uavs[kOutputs] = {};
        ctx->CSSetUnorderedAccessViews(0, kOutputs, uavs, nullptr);
        ID3D11ShaderResourceView *srvs[kInputs] = {};
        UINT n = 0;
        for (auto *v : in) srvs[n++] = v;
        ctx->CSSetShaderResources(0, kInputs, srvs);
        n = 0;
        for (auto *u : out) uavs[n++] = u;
        ctx->CSSetUnorderedAccessViews(0, kOutputs, uavs, nullptr);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->Dispatch(gx, gy, 1);
    }

    // The same, with the group counts taken from largs_ at this byte offset (written by the layer passes).
    void dispatch_indirect(ID3D11DeviceContext *ctx, ID3D11ComputeShader *cs, std::initializer_list<ID3D11ShaderResourceView *> in,
                           std::initializer_list<ID3D11UnorderedAccessView *> out, UINT offset, bool box = false) {
        ID3D11UnorderedAccessView *uavs[kOutputs] = {};
        ctx->CSSetUnorderedAccessViews(0, kOutputs, uavs, nullptr);
        ID3D11ShaderResourceView *srvs[kInputs] = {};
        UINT n = 0;
        for (auto *v : in) srvs[n++] = v;
        ctx->CSSetShaderResources(0, kInputs, srvs);
        n = 0;
        for (auto *u : out) uavs[n++] = u;
        ctx->CSSetUnorderedAccessViews(0, kOutputs, uavs, nullptr);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->DispatchIndirect((box ? largs2_ : largs_).buf.Get(), offset);
    }

    static UINT groups(UINT n) { return (n + 7) / 8; }

    void set_layer_params(Params &p) const {
        p.layer_enable = layer_enable;
        p.layer_margin = layer_margin;
        p.layer_min_bin = layer_min_bin;
        p.layer_detail = layer_detail;
        p.layer_vdiff = layer_vdiff;
        p.layer_link = layer_link;
        p.layer_min_area = layer_min_area;
        p.layer_max_area = layer_max_area;
        p.layer_fill = layer_fill;
        p.layer_band = layer_band;
        p.layer_colour = layer_colour;
        p.layer_bgfail = layer_bgfail;
        p.layer_sup = layer_sup;
        p.layer_both = layer_both;
        p.layer_kill = layer_kill;
        p.layer_void = layer_void;
        p.layer_dark = layer_dark;
        p.layer_edge = layer_edge;
        p.layer_grow_iters = layer_grow_iters;
        p.layer_refine = layer_refine;
        p.layer_gate = layer_gate;
        p.layer_thin = layer_thin;
        p.layer_fit = layer_fit;
        p.layer_attach = layer_attach;
        p.layer_bgslack = layer_bgslack;
        p.layer_vmax = layer_vmax;
        p.cap_fit = cap_fit;
        p.cap_cell = cap_cell;
        p.layer_wmax = layer_wmax;
        p.layer_nbmotion = layer_nbmotion;
        p.layer_inner_colour = layer_inner_colour;
        p.layer_holes = layer_holes;
        p.layer_bgnear = layer_bgnear;
        p.layer_select_gates = layer_select_gates;
        p.layer_select_dom = layer_select_dom;
        p.layer_merge_core = layer_merge_core;
        p.layer_ghost = layer_ghost;
        p.layer_bg_flat = layer_bg_flat;
        p.layer_nb_own = layer_nb_own;
        p.layer_still = layer_still;
        p.layer_hcentre = layer_hcentre;
        p.layer_merge = layer_merge;
        p.layer_refine_edge = layer_refine_edge;
        p.layer_still_pow = layer_still_pow;
        p.layer_still_dx = layer_still_dx;
    }

    // Motion layers (flow.hlsl). Every pass after the first takes its group
    // counts from largs_, which the passes before it write: with no second
    // motion (a pan) they are all empty dispatches.
    void find_layers(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b, float t) {
        ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->CSSetSamplers(0, 1, linear_.GetAddressOf());
        const UINT zero[4] = {0, 0, 0, 0};
        ctx->ClearUnorderedAccessViewUint(lacc_.uav.Get(), zero);
        Level &l0 = level_[0];
        Params p{};
        set_layer_params(p);
        p.grid_w = INT(mid_w_);
        p.grid_h = INT(mid_h_);
        p.src_w = INT(l0.w);
        p.src_h = INT(l0.h);
        p.inv_w = 1.0f / float(l0.w);
        p.inv_h = 1.0f / float(l0.h);
        p.t = t;
        set_params(ctx, p);
        dispatch(ctx, ldom_cs_.Get(), {mid_.srv.Get(), mid_cost_.srv.Get()}, {lrec_.uav.Get(), largs_.uav.Get(), largs2_.uav.Get()},
                 1, 1);
        if (layer_profile) stage("L_dom");
        dispatch(ctx, lbg_cs_.Get(), {mid_.srv.Get(), mid_cost_.srv.Get()}, {lbg_[0].uav.Get(), lbg_[1].uav.Get()},
                 (mid_w_ + 7) / 8, (mid_h_ + 7) / 8);
        if (layer_profile) stage("L_bg");
        dispatch_indirect(ctx, lmargin_cs_.Get(),
                          {l0.luma[0].srv.Get(), l0.luma[1].srv.Get(), mid_.srv.Get(), lrec_.srv.Get(), stats_.srv.Get(),
                           lbg_[0].srv.Get(), lbg_[1].srv.Get()},
                          {lcand_.uav.Get(), llabel_.uav.Get()}, 0);
        for (int i = 0; i < 10; ++i) dispatch_indirect(ctx, llink_cs_.Get(), {mid_.srv.Get()}, {llabel_.uav.Get()}, 0);
        dispatch_indirect(ctx, larea_cs_.Get(), {llabel_.srv.Get(), l0.luma[0].srv.Get(), lcand_.srv.Get(), mid_.srv.Get()}, {lacc_.uav.Get()}, 0);
        if (layer_profile) stage("L_margin+link+area");
        dispatch_indirect(ctx, lselect_cs_.Get(),
                          {llabel_.srv.Get(), mid_.srv.Get(), lcand_.srv.Get(), lacc_.srv.Get(), l0.luma[0].srv.Get(),
                           lbg_[0].srv.Get(), lbg_[1].srv.Get()},
                          {lrec_.uav.Get(), largs2_.uav.Get()}, 12);
        if (layer_profile) stage("L_select");
        // From here on, full resolution.
        p.src_w = INT(width_);
        p.src_h = INT(height_);
        p.inv_w = 1.0f / float(width_);
        p.inv_h = 1.0f / float(height_);
        set_params(ctx, p);
        dispatch_indirect(ctx, lclass_cs_.Get(),
                          {lrec_.srv.Get(), llabel_.srv.Get(), mid_.srv.Get(), lcand_.srv.Get(), lbg_[0].srv.Get(), lbg_[1].srv.Get()},
                          {lcls_.uav.Get()}, 24, true);
        if (layer_profile) stage("L_class");
        dispatch_indirect(ctx, lrefine_cs_.Get(), {a, b, stats_.srv.Get(), lrec_.srv.Get(), lcls_.srv.Get()}, {lvcost_.uav.Get()},
                          48, true);
        dispatch_indirect(ctx, lpick_cs_.Get(), {lvcost_.srv.Get()}, {lrec_.uav.Get()}, 60, true);
        if (layer_profile) stage("L_refine+pick");
        dispatch_indirect(ctx, lcell_cs_.Get(),
                          {a, b, stats_.srv.Get(), lrec_.srv.Get(), llabel_.srv.Get(), mid_.srv.Get(), lcand_.srv.Get(),
                           lbg_[0].srv.Get(), lbg_[1].srv.Get(), lcls_.srv.Get()},
                          {nullptr, lsum_[0].uav.Get(), lsum_[1].uav.Get(), lsum_[2].uav.Get(), lsum_[3].uav.Get(), lacc_.uav.Get()},
                          24, true);
        if (layer_profile) stage("L_cell");
        dispatch_indirect(ctx, lspread_cs_.Get(),
                          {lrec_.srv.Get(), lsum_[0].srv.Get(), lsum_[1].srv.Get(), lsum_[2].srv.Get(), lsum_[3].srv.Get()},
                          {lref_[0].uav.Get(), lref_[1].uav.Get(), lref_[2].uav.Get(), lref_[3].uav.Get()}, 24, true);
        dispatch_indirect(ctx, linit_cs_.Get(),
                          {a, b, stats_.srv.Get(), lrec_.srv.Get(), lcls_.srv.Get(), lref_[0].srv.Get(), lref_[1].srv.Get(),
                           lref_[2].srv.Get(), lref_[3].srv.Get(), lbg_[0].srv.Get(), lbg_[1].srv.Get(), mid_.srv.Get()},
                          {lmask_[0].uav.Get(), lacc_.uav.Get()}, 24, true);
        if (layer_profile) stage("L_spread+init");
        int cur = 0;
        for (int i = 0; i < layer_grow_passes; ++i, cur ^= 1)
            dispatch_indirect(ctx, lgrow_cs_.Get(), {lrec_.srv.Get(), lmask_[cur].srv.Get()}, {lmask_[cur ^ 1].uav.Get()}, 36, true);
        dispatch_indirect(ctx, lfinal_cs_.Get(), {lrec_.srv.Get(), lmask_[cur].srv.Get()},
                          {lmask_[cur ^ 1].uav.Get(), lacc_.uav.Get()}, 36, true);
        lmask_final_ = cur ^ 1;
        if (layer_profile) stage("L_grow+final");
        dispatch_indirect(ctx, ldone_cs_.Get(), {lacc_.srv.Get()}, {lrec_.uav.Get()}, 12);

        ID3D11UnorderedAccessView *no_uav[kOutputs] = {};
        ctx->CSSetUnorderedAccessViews(0, kOutputs, no_uav, nullptr);
        ID3D11ShaderResourceView *none[kInputs] = {};
        ctx->CSSetShaderResources(0, kInputs, none);
    }

    void stage(const char *name) {
        if (on_stage) on_stage(name);
    }

    void find_motion(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b, float t) {
        ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->CSSetSamplers(0, 1, linear_.GetAddressOf());

        // Both pyramids.
        ID3D11ShaderResourceView *frames[2] = {a, b};
        for (int f = 0; f < 2; ++f) {
            Level &l0 = level_[0];
            set_params(ctx, Params{INT(width_), INT(height_), INT(l0.w), INT(l0.h)});
            dispatch(ctx, luma0_cs_.Get(), {frames[f]}, {l0.luma[f].uav.Get()}, groups(l0.w), groups(l0.h));
            for (int i = 1; i < levels_; ++i) {
                Level &up = level_[i - 1], &l = level_[i];
                set_params(ctx, Params{INT(up.w), INT(up.h), INT(l.w), INT(l.h)});
                dispatch(ctx, lumadown_cs_.Get(), {up.luma[f].srv.Get()}, {l.luma[f].uav.Get()}, groups(l.w), groups(l.h));
            }
        }
        // Both frames' mean brightness, from the coarsest level.
        {
            Level &top = level_[levels_ - 1];
            Params p{};
            p.src_w = INT(top.w);
            p.src_h = INT(top.h);
            p.gain_deadzone = gain_deadzone;
            set_params(ctx, p);
            dispatch(ctx, stats_cs_.Get(), {top.luma[0].srv.Get(), top.luma[1].srv.Get()}, {stats_.uav.Get()}, 1, 1);
        }
        stage("pyramids");

        // The motion both ways, coarse to fine: a's blocks found in b (fwd),
        // and b's blocks found in a (bwd).
        for (int dir = 0; dir < 2; ++dir) {
            for (int i = levels_ - 1; i >= 0; --i) {
                Level &l = level_[i];
                Surface &out = dir == 0 ? l.fwd : l.bwd;
                Params p{};
                p.src_w = INT(l.w);
                p.src_h = INT(l.h);
                p.grid_w = INT(l.gw);
                p.grid_h = INT(l.gh);
                p.radius = i <= 1 ? search_radius_fine : search_radius_coarse;
                p.lambda = search_lambda;
                p.guess_cost = guess_cost;
                p.guess_ratio = guess_ratio;
                p.swap = dir;
                ID3D11ShaderResourceView *pred = nullptr;
                if (i + 1 < levels_) {
                    Level &coarse = level_[i + 1];
                    p.has_pred = 1;
                    p.pred_w = INT(coarse.gw);
                    p.pred_h = INT(coarse.gh);
                    pred = (dir == 0 ? coarse.fwd : coarse.bwd).srv.Get();
                }
                set_params(ctx, p);
                dispatch(ctx, search_cs_.Get(), {l.luma[dir].srv.Get(), l.luma[1 - dir].srv.Get(), pred, stats_.srv.Get()},
                         {l.raw.uav.Get()}, l.gw, l.gh);
                dispatch(ctx, median_cs_.Get(), {l.raw.srv.Get()}, {out.uav.Get()}, groups(l.gw), groups(l.gh));
            }
            stage(dir == 0 ? "search a->b" : "search b->a");
            if (dir == 0) {
                // With a's motion known, measure the brightness change on
                // content that matches; the search back and the rest use it.
                Level &l0 = level_[0];
                Params p{};
                p.src_w = INT(l0.w);
                p.src_h = INT(l0.h);
                p.grid_w = INT(l0.gw);
                p.grid_h = INT(l0.gh);
                p.gain_deadzone = gain_deadzone_matched;
                set_params(ctx, p);
                dispatch(ctx, ratio_cs_.Get(), {l0.luma[0].srv.Get(), l0.luma[1].srv.Get(), l0.fwd.srv.Get()},
                         {ratio_.uav.Get()}, groups(l0.gw), groups(l0.gh));
                dispatch(ctx, mgain_cs_.Get(), {ratio_.srv.Get()}, {stats_.uav.Get()}, 1, 1);
            }
        }

        // The motion at the frame in between, then its median.
        Level &l0 = level_[0];
        Params p{};
        p.src_w = INT(l0.w);
        p.src_h = INT(l0.h);
        p.grid_w = INT(mid_w_);
        p.grid_h = INT(mid_h_);
        p.pred_w = INT(l0.gw);
        p.pred_h = INT(l0.gh);
        p.lambda = mid_lambda;
        p.t = t;
        p.inv_w = 1.0f / float(l0.w);
        p.inv_h = 1.0f / float(l0.h);
        p.bad_cost = bad_cost;
        set_params(ctx, p);
        dispatch(ctx, mid_cs_.Get(),
                 {l0.luma[0].srv.Get(), l0.luma[1].srv.Get(), l0.fwd.srv.Get(), l0.bwd.srv.Get(), stats_.srv.Get()},
                 {mid_raw_.uav.Get(), mid_cost_.uav.Get()}, groups(mid_w_), groups(mid_h_));
        dispatch(ctx, median_cs_.Get(), {mid_raw_.srv.Get()}, {mid_.uav.Get()}, groups(mid_w_), groups(mid_h_));
        // How much of it matched nothing (a cut).
        dispatch(ctx, cut_cs_.Get(), {mid_cost_.srv.Get()}, {stats_.uav.Get()}, 1, 1);
        stage("motion at t");

        ID3D11UnorderedAccessView *no_uav[kOutputs] = {};
        ctx->CSSetUnorderedAccessViews(0, kOutputs, no_uav, nullptr);
        ID3D11ShaderResourceView *none[kInputs] = {};
        ctx->CSSetShaderResources(0, kInputs, none);
    }

    // Full-screen triangle state shared by the passes that draw into target.
    void begin_fullscreen(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *target) {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->RSSetState(nullptr);  // the default: solid fill, back faces culled (ours faces front), no scissor
        const D3D11_VIEWPORT vp{0, 0, float(width_), float(height_), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &target, nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
    }

    void draw_warp(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b,
                   ID3D11RenderTargetView *target, const DrawOptions &o) {
        Params p{};
        p.src_w = INT(width_);
        p.src_h = INT(height_);
        p.grid_w = INT(mid_w_);
        p.grid_h = INT(mid_h_);
        p.pred_w = INT(level_[0].gw);
        p.pred_h = INT(level_[0].gh);
        p.t = o.t;
        p.inv_w = 1.0f / float(width_);
        p.inv_h = 1.0f / float(height_);
        p.marker = o.marker ? 1.0f : 0.0f;
        p.marker_level = o.marker_level;
        p.occlusion_cost = occlusion_cost;
        p.flow_weight = flow_weight;
        p.cut_fraction = cut_fraction;
        p.cut_detailed = cut_detailed;
        p.layer_cost = layer_cost;
        p.persist_max = persist_max;
        p.slow_cost = slow_cost;
        p.static_eps = static_eps;
        p.soft_temp = soft_temp;
        p.soft_floor = soft_floor;
        p.soft_scale = soft_scale;
        p.debug_view = debug_view;
        p.still_check = still_check;
        p.reach_plateau = reach_plateau;
        p.soft_wide = soft_wide;
        p.warp_taps = warp_taps;
        p.warp_ring = warp_ring;
        set_layer_params(p);
        set_params(ctx, p);
        begin_fullscreen(ctx, target);
        ctx->PSSetShader(warp_ps_.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->PSSetSamplers(0, 1, linear_.GetAddressOf());
        ID3D11ShaderResourceView *srvs[13] = {a, b, mid_.srv.Get(), level_[0].fwd.srv.Get(), level_[0].bwd.srv.Get(),
                                              stats_.srv.Get(), mid_cost_.srv.Get(), lrec_.srv.Get(),
                                              lmask_[lmask_final_].srv.Get(), lcand_.srv.Get(), llabel_.srv.Get(),
                                              lbg_[0].srv.Get(), lbg_[1].srv.Get()};
        ctx->PSSetShaderResources(0, 13, srvs);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *none[13] = {};
        ctx->PSSetShaderResources(0, 13, none);
    }

    void draw_blend(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b,
                    ID3D11RenderTargetView *target, const DrawOptions &o) {
        const BlendParams params{o.t, o.marker ? 1.0f : 0.0f, o.marker_level, 0.0f};
        ctx->UpdateSubresource(blend_cb_.Get(), 0, nullptr, &params, 0, 0);
        begin_fullscreen(ctx, target);
        ctx->PSSetShader(blend_ps_.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, blend_cb_.GetAddressOf());
        ID3D11ShaderResourceView *srvs[2] = {a, b};
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *none[2] = {};
        ctx->PSSetShaderResources(0, 2, none);
    }

    ComPtr<ID3D11Device> device_;
    UINT width_ = 0, height_ = 0;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> blend_ps_, warp_ps_;
    ComPtr<ID3D11ComputeShader> luma0_cs_, lumadown_cs_, search_cs_, median_cs_, mid_cs_, stats_cs_, cut_cs_, ratio_cs_,
        mgain_cs_;
    ComPtr<ID3D11ComputeShader> ldom_cs_, lmargin_cs_, llink_cs_, larea_cs_, lselect_cs_, lcell_cs_, lspread_cs_, linit_cs_,
        lgrow_cs_, lfinal_cs_, ldone_cs_, lbg_cs_, lclass_cs_, lrefine_cs_, lpick_cs_;
    Surface lvcost_;         // L_REFINE: the cost of each offset of the layer's motion
    Surface lbg_[2];         // per block of 8x8 cells: the background's affine motion (x, y)
    Surface lrec_;           // the layer's record (flow.hlsl LR_*)
    Surface lcand_, llabel_, lcls_;  // per cell: minority margin, component label, class
    Surface lsum_[4], lref_[4];      // per cell: layer / background colour sums of a, b; and spread
    Surface lmask_[2];       // full resolution, ping-pong: the masks being grown
    int lmask_final_ = 0;
    RawBuffer lacc_;         // per component: area and box; then the vote counters
    RawBuffer largs_;        // DispatchIndirect arguments: L_DOM's (cell grid, one group)
    RawBuffer largs2_;       // ... L_SELECT's (the layer's box) - a dispatch may not write the buffer it is started from
    ComPtr<ID3D11Buffer> cb_, blend_cb_;
    ComPtr<ID3D11SamplerState> linear_;
    Level level_[kMaxLevels];
    int levels_ = 0;
    Surface mid_, mid_raw_;
    Surface mid_cost_;  // per cell of the frame in between: how well its motion matched
    Surface ratio_;     // per cell of a: brightness of where it went in b over its own (log2)
    Surface stats_;     // 4x1: brightness gain from a to b, shares of cells that matched nothing (CutCS)
    bool motion_ok_ = false;
    UINT mid_w_ = 0, mid_h_ = 0;  // the cells of the motion at the frame in between
};

}  // namespace fg
