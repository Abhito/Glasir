// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// flow.hlsl - the motion between two frames, and the frame in between made
// from it. Each pass is its own entry point, compiled with its macro defined
// (shaders.cmd); fg_interp.h runs them.
//
// The motion search follows the design AMD documents for FidelityFX Optical
// Flow (MIT; docs/techniques/optical-flow.md in the FidelityFX SDK): a
// luminance pyramid, then from the coarsest level down a full search of each
// 8x8 block around the estimate from the level above, and a 3x3 vector median.
// It is written for shader model 5 (no wave operations). Unlike FidelityFX,
// both directions are found, and they are turned into the motion at the
// in-between frame (MidCS) so each output pixel can take the motion that
// carries both frames onto it (WarpPS).
//
// Units: luma levels and fields are in "level 0" pixels (half the frame's
// resolution); the frame in between is drawn at full resolution.
//
// Frames that do not simply move. A fade or flash changes the brightness of
// the whole picture, so every comparison of a with b is made after scaling a
// by how much the picture brightened or darkened (StatsCS; GainFrom). A cut leaves
// nothing to match: CutCS counts the cells of the frame in between where
// nothing matched, and when most did not, WarpPS shows a again instead of a
// mosaic of both scenes.

cbuffer Params : register(b0)
{
    int2 src_size;    // the image read (pixels)
    int2 grid_size;   // what is written (pixels or cells)
    int radius;       // block search radius, in level pixels
    int has_pred;     // SearchCS: a coarser estimate exists
    int2 pred_size;   // its size (cells)
    float lambda;     // cost per pixel of search offset: keeps flat areas still
    float t;          // where the frame in between sits: 0 = a, 1 = b
    float2 inv_size;  // 1 / size of the texture sampled with coordinates
    float marker;     // 1 = debug square on the frame in between
    float marker_level;
    float occlusion_cost;  // WarpPS: drawing a pixel from one frame alone
    float flow_weight;     // WarpPS: per pixel that frame's own motion is off
    int swap;              // SearchCS: following b's blocks into a
    float bad_cost;        // CutCS: mean luma difference per pixel of a cell that did not match
    float cut_fraction;    // WarpPS: share of such cells that means a cut
    float static_eps;      // WarpPS: a and b this close around a pixel = it stands still
    float gain_deadzone;   // StatsCS: a brightness change this small is none
    float soft_temp;       // WarpPS: how softly candidates are mixed by cost (0 = the best one only)
    int warp_taps;         // WarpPS: pixels each candidate is compared on (1 or 5)
    int warp_ring;         // WarpPS: cells whose motion is tried (2 = the 2x2 around, 3 = the 3x3)
    float soft_floor;      // WarpPS: the softness where the best option fits perfectly
    float soft_scale;      // WarpPS: softness per unit of the best option's cost (up to soft_temp)
    int debug_view;        // WarpPS: 1 = colour each pixel by the options used (evaluator only)
    float still_check;     // WarpPS: StandsStill's limit (0 = off)
    float reach_plateau;   // WarpPS: a cell keeps its full say out to this distance (cells)
    float soft_wide;       // WarpPS: soft_temp where no option explains the pixel (effects)
    float guess_cost;      // SearchCS: the search around no motion wins if it fits better by this (luma SAD; <0 = off)
    float guess_ratio;     // SearchCS: ... and at most this share as badly
    float cut_detailed;    // WarpPS: cut_fraction for the cells with detail (CutCS)
    float layer_cost;      // WarpPS: bringing something else where a and b show the same costs this
    float persist_max;     // WarpPS: ... nothing from this difference between a and b there up
    float slow_cost;       // WarpPS: drawing from one frame onto something slow beside the scenery costs this (0 = off)
    // Motion layers (L_* passes, WarpPS composite). See "Motion layers" below.
    int layer_enable;      // 0 = off: WarpPS exactly as without layers
    float layer_margin;    // a cell is a minority cell when its own motion fits by this much better than the dominant (luma/px)
    float layer_min_bin;   // a second motion needs at least this many cells in one 8 px histogram bin
    float layer_detail;    // cells flatter than this do not vote in the histogram
    float layer_vdiff;     // a minority cell's motion is this far from the dominant (full-res px)
    float layer_link;      // neighbouring minority cells link when their motions are this close (full-res px)
    float layer_min_area;  // a layer has at least this many cells ...
    float layer_max_area;  // ... and at most this many
    float layer_fill;      // ... and fills at least this share of its box
    float layer_band;      // the pixel mask grows this many cells beyond the component
    float layer_colour;    // a band pixel joins when it is this much closer to the layer's colour than the background's
    float layer_bgfail;    // ... and the dominant motion does not carry it (5-tap cost above this)
    float layer_sup;       // one-frame coverage within this many px of the other frame's: only the preferred frame draws
    int layer_both;        // where both frames cover: 0 = A/B along the layer motion, 1 = leave WarpPS, 2 = the nearer frame
    float layer_kill;      // deletion vote: the layer dies when its deletion share is under this times the expected share
    float layer_void;      // ... the vote is void (prior: compact minority in front) when the expected share is under this
    float layer_dark;      // a component darker than this (mean luma) is no layer (black bars)
    float layer_edge;      // a cell's layer and background colours closer than this: only seed pixels there
    int layer_grow_iters;  // flood steps per grow pass (in a 32x32 tile)
    int layer_refine;      // 1 = the layer's motion searched (L_REFINE), 0 = the mean of its cells' motions
    int layer_gate;        // 1/2 = the layer replaces only what WarpPS drew with the background's motion (2: from both frames), 3/4/5 stronger inside both masks, 7 = 3 for a nearly still layer, else 2
    float layer_thin;      // one-frame coverage thicker than 2 x this + 1 px is not drawn (0 = off)
    float layer_fit;       // a band pixel the layer's motion carries onto the other frame this well (5-tap) joins (0 = off)
    float layer_attach;    // one-frame coverage farther than this (px) from where both frames' masks cover is not drawn (0 = off)
    float layer_bgslack;      // the background's motion is tried this many px off too before a pixel counts as not carried by it (0 = off)
    float layer_vmax;         // a layer moving faster than this on screen (px per frame) is not the character the camera follows (0 = off)
    float cap_fit;            // WarpPS: a both-frames option this cheap explains the pixel (soft_temp, not soft_wide; 0 = off)
    float cap_cell;           // WarpPS: ... only where the cell's own motion cost (mid_cost) is under this (0 = no check)
    float layer_wmax;         // a layer wider than this many cells (16 px) is a close-up body, not a head: no layer (0 = off)
    float layer_nbmotion;     // 1 = a rim/band pixel a 3x3 neighbour cell's motion carries is not the layer's (0 = off)
    float layer_inner_colour; // 1 = inner layer cells need the layer colour (or its motion fit) too (0 = all but background-like)
    float layer_holes;        // a pixel a frame's mask encloses within this many px on all 4 sides joins it (0 = off)
    float layer_bgnear;       // WarpPS: an option within this share of |background - layer motion| of the background's motion moves with it (if more than layer_vdiff; 0 = off)
    float layer_select_gates; // 1 = layer_vmax / layer_wmax rule components out of the choice (the next one down can win), 0 = they kill the layer
    float layer_ghost;        // WarpPS: an option not moving with the layer whose sample in a frame lands on the layer there pays this (0 = off)
    float layer_bg_flat;      // L_BG: a cell with little detail counts this much towards the background's motion (a detailed one 1)
    float layer_nb_own;       // 1 = layer_nbmotion does not ask the layer's own cells (a head turning on a running body)
    float layer_still;        // L_SELECT: a component moving faster than this on screen (px per frame) scores this / its speed (0 = off)
    float layer_hcentre;      // L_SELECT: one farther than this from the picture's middle across (px) scores less (0 = off)
    float layer_merge;        // L_SELECT: components this many empty cells apart that move alike (layer_link) count as one (0 = off)
    float layer_refine_edge;  // L_PICK: keep the cells' mean when 1 = no inner cells, 2 = also the best offset on the search's edge (0 = off)
    float layer_still_pow;    // L_SELECT: layer_still's factor to this power
    float layer_still_dx;     // L_SELECT: layer_still counts from this far off the middle across (px; full 150 px further; < 0 = everywhere)
    float layer_select_dom;   // L_SELECT: layer_merge, layer_still and layer_hcentre only while the dominant motion is at least this (px per frame:
                              // the camera follows the player; 0 = always)
    float layer_merge_core;   // L_SELECT: 1 = only a slow core near the middle across (layer_still, layer_still_dx) takes fragments (0 = any)
    float3 pad_end;
};

// [0] how much brighter b is than a (StatsCS), [1] share of cells that did
// not match, [2] share of the cells with detail that did not, [3] share of
// cells with detail (CutCS).
#define STATS_GAIN 0
#define STATS_BAD 1
#define STATS_BAD_DETAILED 2


// What a's brightness is scaled by to compare with b's.
float GainFrom(Texture2D<float> stats) { return stats.Load(int3(STATS_GAIN, 0, 0)); }

#define BLOCK 8
// The cells of the motion at the frame in between, in level-0 pixels (16 of
// the frame's). Finer cells (4) were tried: their 8x8 cost window still spans
// the edge, the match gets noisier, and the clips came out worse. Must match
// kMidBlock in fg_interp.h.
#define MID_BLOCK 8

float Luma(float3 c)
{
    // On the encoded values (PQ for HDR, sRGB for SDR): closer to what the
    // eye compares than linear light, which is all the search needs.
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// ---- Luma pyramid --------------------------------------------------------------

#if defined(LUMA0)
// Level 0: half resolution, from the frame's colours.
Texture2D<float4> color_in : register(t0);
RWTexture2D<float> luma_out : register(u0);

[numthreads(8, 8, 1)]
void LumaFromColorCS(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy);
    if (any(p >= grid_size)) return;
    const int2 m = src_size - 1;
    const int2 s = p * 2;
    const float l = Luma(color_in.Load(int3(min(s, m), 0)).rgb) +
                    Luma(color_in.Load(int3(min(s + int2(1, 0), m), 0)).rgb) +
                    Luma(color_in.Load(int3(min(s + int2(0, 1), m), 0)).rgb) +
                    Luma(color_in.Load(int3(min(s + int2(1, 1), m), 0)).rgb);
    luma_out[p] = l * 0.25;
}
#endif

#if defined(LUMADOWN)
// Each further level: half the one above.
Texture2D<float> luma_in : register(t0);
RWTexture2D<float> luma_out : register(u0);

[numthreads(8, 8, 1)]
void LumaDownCS(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy);
    if (any(p >= grid_size)) return;
    const int2 m = src_size - 1;
    const int2 s = p * 2;
    luma_out[p] = 0.25 * (luma_in.Load(int3(min(s, m), 0)) + luma_in.Load(int3(min(s + int2(1, 0), m), 0)) +
                          luma_in.Load(int3(min(s + int2(0, 1), m), 0)) + luma_in.Load(int3(min(s + int2(1, 1), m), 0)));
}
#endif

// ---- Mean brightness, and cuts -------------------------------------------------------

#if defined(STATS) || defined(MGAIN)
// How much brighter b is than a: the median of b's luma over a's across many
// patches. A fade dims every patch alike and the median is that factor. Within
// gain_deadzone of 1 it is taken as 1; with too few patches bright enough to
// tell, 1.
//   StatsCS  before any motion is known: the patches are the pixels of the
//            coarsest luma level (64x64 of the frame each), a and b at the
//            same place. Good enough to steer the search; in a fast pan the
//            content under a patch changes and the median drifts.
//   GainCS   once a's motion into b is known: the patches are a's 8x8 cells
//            and the cells they moved to in b (RatioCS), so a pan cannot pass
//            for a fade. This one is used from then on.
#define MAX_PATCHES 4096
RWTexture2D<float> stats_out : register(u0);
groupshared float g_ratio[MAX_PATCHES];  // log2 of b over a, or 1e9 when too dark
groupshared uint g_count[64];

uint CountBelow(uint gi, int n, float limit)
{
    uint c = 0;
    for (int i = int(gi); i < n; i += 64) c += g_ratio[i] < limit ? 1u : 0u;
    g_count[gi] = c;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 32; step > 0; step >>= 1)
    {
        if (gi < step) g_count[gi] += g_count[gi + step];
        GroupMemoryBarrierWithGroupSync();
    }
    const uint total = g_count[0];
    GroupMemoryBarrierWithGroupSync();
    return total;
}

void WriteMedianGain(uint gi, int n)
{
    GroupMemoryBarrierWithGroupSync();
    const uint valid = CountBelow(gi, n, 1e8);
    float lo = -1.0, hi = 1.0;  // log2: a gain between 0.5 and 2
    [loop] for (int k = 0; k < 16; ++k)
    {
        const float m = 0.5 * (lo + hi);
        if (CountBelow(gi, n, m) * 2 < valid)
            lo = m;
        else
            hi = m;
    }
    float gain = exp2(0.5 * (lo + hi));
    if (valid < 16 || abs(gain - 1.0) < gain_deadzone) gain = 1.0;
    if (gi == 0) stats_out[int2(STATS_GAIN, 0)] = gain;
}
#endif

#if defined(STATS)
Texture2D<float> luma_a : register(t0);  // the coarsest level
Texture2D<float> luma_b : register(t1);

[numthreads(64, 1, 1)]
void StatsCS(uint gi : SV_GroupIndex)
{
    const int n = min(src_size.x * src_size.y, MAX_PATCHES);
    for (int i = int(gi); i < n; i += 64)
    {
        const int3 q = int3(i % src_size.x, i / src_size.x, 0);
        const float la = luma_a.Load(q), lb = luma_b.Load(q);
        g_ratio[i] = (la > 0.05 && lb > 0.05) ? log2(lb / la) : 1e9;
    }
    WriteMedianGain(gi, n);
}
#endif

#if defined(RATIO)
// Per 8x8 cell of a at level 0: log2 of the mean luma of where it went in b
// over its own, or 1e9 when either is too dark to tell.
Texture2D<float> luma_a : register(t0);
Texture2D<float> luma_b : register(t1);
Texture2D<float2> fwd : register(t2);
RWTexture2D<float> ratio_out : register(u0);

[numthreads(8, 8, 1)]
void RatioCS(uint3 id : SV_DispatchThreadID)
{
    const int2 cell = int2(id.xy);
    if (any(cell >= grid_size)) return;
    const int2 origin = cell * BLOCK;
    const int2 moved = origin + int2(round(fwd.Load(int3(cell, 0))));
    const int2 maxp = src_size - 1;
    float sa = 0, sb = 0;
    [loop] for (int y = 0; y < BLOCK; ++y)
    {
        [unroll] for (int x = 0; x < BLOCK; ++x)
        {
            sa += luma_a.Load(int3(clamp(origin + int2(x, y), 0, maxp), 0));
            sb += luma_b.Load(int3(clamp(moved + int2(x, y), 0, maxp), 0));
        }
    }
    const float la = sa / (BLOCK * BLOCK), lb = sb / (BLOCK * BLOCK);
    const bool inside = all(moved >= 0) && all(moved + BLOCK <= src_size);
    ratio_out[cell] = (inside && la > 0.05 && lb > 0.05) ? log2(lb / la) : 1e9;
}
#endif

#if defined(MGAIN)
Texture2D<float> ratio_in : register(t0);  // RatioCS, grid_size cells

[numthreads(64, 1, 1)]
void GainCS(uint gi : SV_GroupIndex)
{
    // Every stride-th cell, so at most MAX_PATCHES of them.
    const int cells = grid_size.x * grid_size.y;
    const int stride = (cells + MAX_PATCHES - 1) / MAX_PATCHES;
    const int n = (cells + stride - 1) / stride;
    for (int i = int(gi); i < n; i += 64)
    {
        const int c = i * stride;
        g_ratio[i] = ratio_in.Load(int3(c % grid_size.x, c / grid_size.x, 0));
    }
    WriteMedianGain(gi, n);
}
#endif

#if defined(CUT)
// The share of the frame in between's cells whose best motion still left a
// and b far apart (MidCS's cost per pixel above bad_cost) - of all cells, and
// of those with detail. A cell without detail matches under any motion, even
// across a cut: two shots with much flat dark sky in common leave only a fifth
// of all cells unmatched, but half of those with detail. The second share is
// only taken when at least a tenth of the cells have detail (a dark scene with
// one small effect is no cut).
Texture2D<float2> cost_in : register(t0);  // MidCS: (cost, detail) per cell
RWTexture2D<float> stats_out : register(u0);
groupshared uint g_bad[256];
groupshared uint g_seen[256];
#define DETAIL_MIN 0.015

[numthreads(256, 1, 1)]
void CutCS(uint gi : SV_GroupIndex)
{
    uint bad = 0, seen = 0;
    const int n = grid_size.x * grid_size.y;
    for (int i = int(gi); i < n; i += 256)
    {
        const float2 c = cost_in.Load(int3(i % grid_size.x, i / grid_size.x, 0));
        bad += c.x > bad_cost ? 1u : 0u;
        seen += (c.x > bad_cost || c.y > DETAIL_MIN) ? 1u : 0u;
    }
    g_bad[gi] = bad;
    g_seen[gi] = seen;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 128; step > 0; step >>= 1)
    {
        if (gi < step)
        {
            g_bad[gi] += g_bad[gi + step];
            g_seen[gi] += g_seen[gi + step];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
    {
        stats_out[int2(STATS_BAD, 0)] = float(g_bad[0]) / float(max(n, 1));
        stats_out[int2(STATS_BAD_DETAILED, 0)] =
            g_seen[0] * 10u >= uint(n) ? float(g_bad[0]) / float(max(g_seen[0], 1u)) : 0.0;
        stats_out[int2(3, 0)] = float(g_seen[0]) / float(max(n, 1));  // for fg_eval --stats
    }
}
#endif

// ---- Block search, one level -----------------------------------------------------

#if defined(SEARCH)
#define MAX_RADIUS 8
#define WIN (BLOCK + 2 * MAX_RADIUS)
#define MAX_SIDE (2 * MAX_RADIUS + 1)

Texture2D<float> luma_from : register(t0);  // the frame whose blocks are followed
Texture2D<float> luma_to : register(t1);    // the frame they are looked for in
Texture2D<float2> pred : register(t2);      // the field one level coarser
Texture2D<float> stats : register(t3);
RWTexture2D<float2> field_out : register(u0);

groupshared float g_block[BLOCK * BLOCK];
groupshared float g_window[WIN * WIN];
groupshared float g_cost[MAX_SIDE * MAX_SIDE];
groupshared uint g_best;
groupshared float3 g_found[2];  // per search: the motion found (xy) and its cost (z)

// One group per 8x8 block. Every offset within the radius around the coarser
// estimate is tried (sum of absolute differences, plus lambda per pixel of
// offset so flat areas keep the estimate); the best one is refined to a
// fraction of a pixel by a parabola through its neighbours' costs.
//
// An object only a few blocks across is lost on the coarser levels: there its
// blocks also hold the scenery around it, and they take the scenery's motion.
// A character the camera follows or turns around stays (nearly) still on
// screen while the scenery sweeps past, 90 pixels a frame in a fast turn; its
// head then has the scenery's estimate, and the radius around that never
// reaches the head's own motion. So the radius around no motion is searched
// as well, and what it finds is taken if it fits clearly better than what the
// search around the estimate found: by guess_cost, and at most guess_ratio as
// badly. Both are judged after their search: the estimate as it stands can be
// a few pixels off, and on even stone a few pixels off fits worse than no
// motion does, though the search around it finds the stone's real motion.
// Where both fit alike (a flat wall) or neither does (new scenery a pan brings
// in at the edge of the screen, a cut), the estimate's stays. (The estimates
// of the neighbouring blocks were tried as starting points too and dropped:
// on a repeating pattern, stairs, a neighbour's wrong estimate one step off
// fits as well, and spread.)
[numthreads(BLOCK, BLOCK, 1)]
void SearchCS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const int2 cell = int2(gid.xy);
    const int2 origin = cell * BLOCK;
    const int2 maxp = src_size - 1;
    const float gain = swap ? 1.0 / GainFrom(stats) : GainFrom(stats);  // from's brightness to to's
    g_block[gi] = gain * luma_from.Load(int3(clamp(origin + int2(tid.xy), 0, maxp), 0));

    float2 estimate = 0;
    if (has_pred) estimate = pred.Load(int3(min(cell / 2, pred_size - 1), 0)) * 2.0;
    const int r = min(radius, MAX_RADIUS);
    const int win = BLOCK + 2 * r;
    const int side = 2 * r + 1;
    // Around the estimate, then around no motion (unless the estimate is none).
    const int searches = has_pred && guess_cost >= 0.0 ? 2 : 1;
    for (int s = 0; s < searches; ++s)
    {
        const int2 shift = s == 0 ? int2(round(estimate)) : int2(0, 0);
        const bool run = s == 0 || any(int2(round(estimate)) != 0);
        GroupMemoryBarrierWithGroupSync();  // g_block written, the last search's window and costs read
        if (run)
        {
            for (int i = int(gi); i < win * win; i += BLOCK * BLOCK)
            {
                const int2 w = int2(i % win, i / win);
                g_window[w.y * WIN + w.x] = luma_to.Load(int3(clamp(origin + shift - r + w, 0, maxp), 0));
            }
        }
        if (gi == 0) g_best = 0xffffffffu;
        GroupMemoryBarrierWithGroupSync();

        if (run)
        {
            for (int j = int(gi); j < side * side; j += BLOCK * BLOCK)
            {
                const int2 at = int2(j % side, j / side);
                float sad = 0;
                [loop] for (int y = 0; y < BLOCK; ++y)
                {
                    [unroll] for (int x = 0; x < BLOCK; ++x)
                        sad += abs(g_block[y * BLOCK + x] - g_window[(y + at.y) * WIN + x + at.x]);
                }
                const float cost = sad + lambda * float(abs(at.x - r) + abs(at.y - r));
                g_cost[j] = cost;
                InterlockedMin(g_best, (min(uint(cost * 4096.0), 0x7fffffu) << 9) | uint(j));
            }
        }
        GroupMemoryBarrierWithGroupSync();
        if (gi == 0)
        {
            if (!run)
            {
                g_found[s] = float3(0, 0, 1e30);
            }
            else
            {
                const int k = int(g_best & 511u);
                const int2 off = int2(k % side, k / side);
                float2 sub = 0;
                if (off.x > 0 && off.x < side - 1)
                {
                    const float c0 = g_cost[k - 1], c1 = g_cost[k], c2 = g_cost[k + 1];
                    const float den = c0 - 2.0 * c1 + c2;
                    if (den > 1e-6) sub.x = clamp(0.5 * (c0 - c2) / den, -0.5, 0.5);
                }
                if (off.y > 0 && off.y < side - 1)
                {
                    const float c0 = g_cost[k - side], c1 = g_cost[k], c2 = g_cost[k + side];
                    const float den = c0 - 2.0 * c1 + c2;
                    if (den > 1e-6) sub.y = clamp(0.5 * (c0 - c2) / den, -0.5, 0.5);
                }
                g_found[s] = float3(float2(shift + off - r) + sub, g_cost[k]);
            }
        }
    }
    if (gi != 0) return;
    float3 found = g_found[0];
    if (searches == 2 && g_found[1].z + guess_cost <= guess_ratio * found.z) found = g_found[1];
    field_out[cell] = found.xy;
}
#endif

// ---- 3x3 vector median ------------------------------------------------------------

#if defined(MEDIAN)
Texture2D<float2> field_in : register(t0);
RWTexture2D<float2> field_out : register(u0);

// Keeps, of the nine vectors around a cell, the one closest to all the others:
// a lone wrong match gives way to its neighbours, an edge between two motions
// stays sharp.
[numthreads(8, 8, 1)]
void MedianCS(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy);
    if (any(p >= grid_size)) return;
    float2 v[9];
    [unroll] for (int i = 0; i < 9; ++i)
        v[i] = field_in.Load(int3(clamp(p + int2(i % 3 - 1, i / 3 - 1), 0, grid_size - 1), 0));
    float2 best = v[4];
    float best_d = 1e30;
    [unroll] for (int a = 0; a < 9; ++a)
    {
        float d = 0;
        [unroll] for (int b = 0; b < 9; ++b) d += abs(v[a].x - v[b].x) + abs(v[a].y - v[b].y);
        if (d < best_d)
        {
            best_d = d;
            best = v[a];
        }
    }
    field_out[p] = best;
}
#endif

// ---- Motion at the frame in between --------------------------------------------------

#if defined(MID)
Texture2D<float> luma_a : register(t0);
Texture2D<float> luma_b : register(t1);
Texture2D<float2> fwd : register(t2);  // a -> b, by a's cells
Texture2D<float2> bwd : register(t3);  // b -> a, by b's cells
Texture2D<float> stats : register(t4);
SamplerState linear_clamp : register(s0);
RWTexture2D<float2> field_out : register(u0);
RWTexture2D<float2> cost_out : register(u1);  // the best cost per pixel, and the cell's detail, for CutCS

static float g_gain;

// How badly v carries a onto b across this cell of the frame in between: a is
// sampled where the content was (t of the way back along v), b where it went.
float BilateralCost(int2 origin, float2 v)
{
    float sum = 0;
    [loop] for (int y = 0; y < BLOCK; ++y)
    {
        [unroll] for (int x = 0; x < BLOCK; ++x)
        {
            const float2 q = float2(origin + int2(x, y)) + 0.5;
            sum += abs(g_gain * luma_a.SampleLevel(linear_clamp, (q - t * v) * inv_size, 0) -
                       luma_b.SampleLevel(linear_clamp, (q + (1.0 - t) * v) * inv_size, 0));
        }
    }
    return sum;
}

// How much the picture varies across this cell (mean distance from its mean
// luma): a cell without any matches under every motion, so its match says
// nothing about whether the frames show the same scene.
float Detail(Texture2D<float> luma, int2 origin)
{
    const int2 maxp = src_size - 1;
    float sum = 0;
    [loop] for (int y = 0; y < BLOCK; ++y)
        [unroll] for (int x = 0; x < BLOCK; ++x) sum += luma.Load(int3(clamp(origin + int2(x, y), 0, maxp), 0));
    const float mean = sum / float(BLOCK * BLOCK);
    float dev = 0;
    [loop] for (int y2 = 0; y2 < BLOCK; ++y2)
        [unroll] for (int x2 = 0; x2 < BLOCK; ++x2)
            dev += abs(luma.Load(int3(clamp(origin + int2(x2, y2), 0, maxp), 0)) - mean);
    return dev / float(BLOCK * BLOCK);
}

// pred_size: the size of the search's fields (BLOCK cells); grid_size: this
// pass's cells (MID_BLOCK).
float2 FwdAt(float2 pos) { return fwd.Load(int3(clamp(int2(floor(pos / BLOCK)), 0, pred_size - 1), 0)); }
float2 BwdAt(float2 pos) { return -bwd.Load(int3(clamp(int2(floor(pos / BLOCK)), 0, pred_size - 1), 0)); }


// The motion of each cell of the frame in between, picked from the motions
// found for a and b: the cell's own and its neighbours' in both directions,
// plus those found where each of those says the content came from and went to
// (an object moving further than a cell between the frames is not at the same
// cell in a, b and the frame in between), plus none.
[numthreads(8, 8, 1)]
void MidCS(uint3 id : SV_DispatchThreadID)
{
    const int2 cell = int2(id.xy);
    if (any(cell >= grid_size)) return;
    const float2 center = float2(cell * MID_BLOCK) + MID_BLOCK * 0.5;
    const int2 origin = int2(center) - BLOCK / 2;  // the 8x8 window the cost is taken on
    const int2 search_cell = int2(floor(center / BLOCK));
    g_gain = GainFrom(stats);

    float2 best = 0;
    float best_cost = BilateralCost(origin, 0) + lambda;  // none, slightly discouraged
    [loop] for (uint i = 0; i < 9; ++i)
    {
        const int2 n = clamp(search_cell + int2(int(i % 3u) - 1, int(i / 3u) - 1), 0, pred_size - 1);
        const float2 f = fwd.Load(int3(n, 0));
        const float2 b = -bwd.Load(int3(n, 0));
        float2 cand[4] = {f, b, FwdAt(center - t * f), BwdAt(center + (1.0 - t) * b)};
        [unroll] for (int c = 0; c < 4; ++c)
        {
            const float cost = BilateralCost(origin, cand[c]);
            if (cost < best_cost)
            {
                best_cost = cost;
                best = cand[c];
            }
        }
    }
    field_out[cell] = best;
    cost_out[cell] = float2(best_cost / float(BLOCK * BLOCK), max(Detail(luma_a, origin), Detail(luma_b, origin)));
}
#endif

// ---- Motion layers -----------------------------------------------------------------
//
// A thin part of a foreground object (a spike of hair) sits in 16 px cells the
// background dominates; the background's motion carries plain floor onto it with
// an almost perfect match, and the hair itself deforms, so no per-pixel cost
// can bring it back. The object as a whole is different: a compact group of
// cells whose own motion fits clearly better than the camera's. So, without any
// depth:
//   L_DOM     the dominant motion of the frame in between (histogram of mid);
//             with no clear second motion (a pan) nothing below runs.
//   L_MARGIN  minority cells: motion away from the dominant one, and the
//             dominant motion fails there by layer_margin (the stair gate).
//   L_LINK    connect them (min-label propagation with pointer jumping),
//   L_AREA    and measure each component (area, box);
//   L_SELECT  keep the largest compact one (one layer), its motion and box.
//   L_CELL    per cell of the box: layer cell / band; colour sums of the layer
//             and of the background, in each frame at the layer's position;
//   L_SPREAD  local layer and background colours for every band cell;
//   L_INIT    per pixel and frame: seed (layer cells), candidate (band pixels
//             closer to the layer's colour and not carried by the dominant
//             motion), or nothing;
//   L_GROW    4-connected flood from the seeds through candidates (no closing);
//   L_FINAL   the masks, dilated by layer_sup for the double-tip rule, and the
//             deletion vote (does the background disappear under the layer?);
//   L_DONE    the kill switch.
// The masks are kept at the frame in between's position along the layer's
// motion: mask a at y says frame a's pixel at y - t v is the layer's, mask b at
// y says b's pixel at y + (1 - t) v is. WarpPS draws the covered pixels from
// those samples, over its own result.
#if defined(L_DOM) || defined(L_MARGIN) || defined(L_LINK) || defined(L_AREA) || defined(L_SELECT) || \
    defined(L_CELL) || defined(L_SPREAD) || defined(L_INIT) || defined(L_GROW) || defined(L_FINAL) || defined(L_DONE) || \
    defined(WARP) || defined(L_BG) || defined(L_CLASS) || defined(L_REFINE) || defined(L_PICK)
#define LR_ACTIVE 0   // a second motion exists
#define LR_VDOM 1     // 1, 2: the dominant motion (full-res px)
#define LR_PEAK2 3    // cells in the fullest other bin
#define LR_ROOT 4     // the chosen component's label, -1 = none
#define LR_VK 5       // 5, 6: its motion (full-res px)
#define LR_CBOX 7     // 7..10: its box in cells (x0, y0, x1, y1)
#define LR_AREA 11
#define LR_FILL 12
#define LR_MARGIN 13
#define LR_OTHER 14   // cells away from the dominant motion
#define LR_ALIVE 15   // the layer is drawn
#define LR_BOX 16     // 16..19: the box with the band, in cells, inclusive
#define LR_F 20       // deletion share
#define LR_E 21       // expected share
#define LR_MASKPX 22
#define LR_LUMA 23
#define LR_VBG 24     // 24, 25: the background's motion at the layer (full-res px)
#define LR_NGROUP 45  // the components drawn as the layer (layer_merge): how many,
#define LR_GROUP 48   // 48..63: their labels
#define LR_CANDS 46   // L_SELECT's candidates (evaluator)
#define GROUP_MAX 16
#define LR_COUNT 64
#define NO_LABEL 0xffffffffu
#define ACC_STRIDE 9  // per component root: area, 0xffff - x0, 0xffff - y0, x1, y1, black cells, margin sum (x1000),
                      // motion x and y sums (level-0 px x 16, two's complement)
#define VSEARCH 16    // L_REFINE: the layer's motion is searched this far around the cells' mean (full-res px)
#define VSIDE (2 * VSEARCH + 1)
#define LR_VCOST 41   // the best and the cells' mean motion's cost (L_PICK)
#define LR_V0 43      // 43, 44: the cells' mean motion
#define LAYER_CELL 16 // full-res pixels per cell of the frame in between
#define SUP_MAX 16
#define LAYER_STILL 8.0  // WarpPS, layer_gate 7: a layer moving at most this far (px per frame) stands nearly still

uint2 CellOf(uint l) { return uint2(l % uint(grid_size.x), l / uint(grid_size.x)); }

// The offsets (full-res, whole pixels) from the frame in between's position to
// each frame's, along the layer's motion.
int2 OffsetA(float2 vk) { return int2(round(-t * vk)); }
int2 OffsetB(float2 vk) { return int2(round((1.0 - t) * vk)); }
#endif

#if defined(L_AREA) || defined(L_SELECT) || defined(L_CELL) || defined(L_CLASS)
// A label's root, after a few more pointer jumps than L_LINK made.
uint Resolve(Texture2D<uint> lab, uint l)
{
    [unroll] for (int k = 0; k < 3; ++k) l = min(l, lab.Load(int3(CellOf(l), 0)));
    return l;
}
#endif

#if defined(L_CELL) || defined(L_SPREAD) || defined(L_INIT) || defined(L_GROW) || defined(L_FINAL) || defined(L_CLASS) || \
    defined(L_REFINE)
float LR(Texture2D<float> r, int i) { return r.Load(int3(i, 0, 0)); }
int4 BoxCells(Texture2D<float> r) { return int4(LR(r, LR_BOX), LR(r, LR_BOX + 1), LR(r, LR_BOX + 2), LR(r, LR_BOX + 3)); }
// The box in full-res pixels: [xy, zw).
int4 BoxPixels(Texture2D<float> r)
{
    const int4 b = BoxCells(r);
    return int4(b.xy * LAYER_CELL, min((b.zw + 1) * LAYER_CELL, src_size));
}
bool InBox(int4 bp, int2 q) { return all(q >= bp.xy) && all(q < bp.zw); }
#endif

#if defined(L_CELL) || defined(L_INIT)
// How badly the dominant motion carries own's pixel at pos onto other (5-tap
// cross, sum of RGB differences): small where pos is background both frames show.
float BgCost(Texture2D<float4> own, Texture2D<float4> other, SamplerState s, float2 pos, float2 v, float g_own,
             float g_other)
{
    const float2 taps[5] = {float2(0, 0), float2(1.5, 0), float2(-1.5, 0), float2(0, 1.5), float2(0, -1.5)};
    float3 d = 0;
    [unroll] for (int k = 0; k < 5; ++k)
    {
        const float3 a = g_own * own.SampleLevel(s, (pos + taps[k]) * inv_size, 0).rgb;
        const float3 b = g_other * other.SampleLevel(s, (pos + v + taps[k]) * inv_size, 0).rgb;
        d += abs(a - b);
    }
    return (d.r + d.g + d.b) / 5.0;
}
float SumAbs(float3 d) { return abs(d.r) + abs(d.g) + abs(d.b); }
// Not carried by the background's motion v: its cost above layer_bgfail - and,
// with layer_bgslack, at v moved that far each way (8 directions) too. The local
// background model is an affine fit; on a sharp edge of the scenery (a kerb, a
// stripe) it is a pixel or two off, the edge fails at v exactly, and a dark edge
// joined the layer as a thread.
bool BgFails(Texture2D<float4> own, Texture2D<float4> other, SamplerState s, float2 pos, float2 v, float g_own, float g_other)
{
    bool fail = BgCost(own, other, s, pos, v, g_own, g_other) > layer_bgfail;
    [branch] if (fail && layer_bgslack > 0.0)
    {
        [loop] for (int k = 0; k < 8; ++k)
        {
            const float2 o = float2(k < 3 ? -1 : (k < 5 ? 0 : 1), k < 3 ? float(k - 1) : (k < 5 ? float(k * 2 - 7) : float(k - 6))) * layer_bgslack;
            if (fail) fail = BgCost(own, other, s, pos, v + o, g_own, g_other) > layer_bgfail;
        }
    }
    return fail;
}
#endif

#if defined(L_MARGIN) || defined(L_CELL) || defined(L_INIT) || defined(L_SELECT) || defined(L_CLASS) || defined(WARP)
// The background's motion at a cell (level-0 px): the affine model L_BG fitted
// around the cell's block.
float2 BgAt(Texture2D<float4> bgx, Texture2D<float4> bgy, int2 cell)
{
    const int2 blocks = (grid_size + 7) / 8;
    const int2 b = clamp(cell / 8, 0, blocks - 1);
    const float2 d = (float2(cell) + 0.5 - (float2(b * 8) + 4.0)) / 8.0;
    const float4 px = bgx.Load(int3(b, 0)), py = bgy.Load(int3(b, 0));
    return float2(px.x + px.y * d.x + px.z * d.y, py.x + py.y * d.x + py.z * d.y);
}
#endif

#if defined(L_BG)
// The background's motion, locally: per block of 8x8 cells, the most common
// motion of the cells within two blocks around it (a 40x40-cell window, 640
// px: larger than a character), then an affine model fitted to the cells near
// it (a floor under a moving camera does not move as one translation - near
// the camera faster than far away). A compact object is a minority in that
// window and stays out of the fit.
Texture2D<float2> mid : register(t0);       // level-0 px
Texture2D<float2> mid_cost : register(t1);  // (cost, detail)
RWTexture2D<float4> bg_x : register(u0);    // vx = x + y dx + z dy (dx, dy: cells from the block centre / 8), w = cells fitted
RWTexture2D<float4> bg_y : register(u1);
#define NB 40
groupshared uint g_hist[NB * NB];
groupshared uint g_key;
groupshared float4 g_r[3 * 256];

void GroupSum12(uint gi, inout float4 a, inout float4 b, inout float4 c)
{
    g_r[gi] = a;
    g_r[256 + gi] = b;
    g_r[512 + gi] = c;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 128; s > 0; s >>= 1)
    {
        if (gi < s)
        {
            g_r[gi] += g_r[gi + s];
            g_r[256 + gi] += g_r[256 + gi + s];
            g_r[512 + gi] += g_r[512 + gi + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    a = g_r[0];
    b = g_r[256];
    c = g_r[512];
    GroupMemoryBarrierWithGroupSync();
}

[numthreads(256, 1, 1)]
void BgCS(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const int2 blk = int2(gid.xy);
    const int2 c0 = max((blk - 2) * 8, 0), c1 = min((blk + 3) * 8, grid_size);
    const int2 span = c1 - c0;
    const int n = span.x * span.y;
    const float2 centre = float2(blk * 8) + 4.0;
    for (uint j = gi; j < NB * NB; j += 256) g_hist[j] = 0;
    if (gi == 0) g_key = 0;
    GroupMemoryBarrierWithGroupSync();
    for (int i = int(gi); i < n; i += 256)
    {
        const int3 c = int3(c0 + int2(i % span.x, i / span.x), 0);
        const uint wh = mid_cost.Load(c).y >= layer_detail ? 2u : uint(round(2.0 * layer_bg_flat));
        if (wh > 0u)
        {
            const int2 b = clamp(int2(floor(mid.Load(c) / 4.0)) + NB / 2, 0, NB - 1);
            InterlockedAdd(g_hist[b.y * NB + b.x], wh);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j2 = gi; j2 < NB * NB; j2 += 256)
    {
        const int bx = int(j2 % NB), by = int(j2 / NB);
        uint s = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const int2 q = int2(bx + k % 3 - 1, by + k / 3 - 1);
            if (all(q >= 0) && all(q < NB)) s += g_hist[q.y * NB + q.x];
        }
        InterlockedMax(g_key, (s << 11) | j2);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint peak = g_key & 2047u;
    float2 m = (float2(peak % NB, peak / NB) - NB / 2 + 0.5) * 4.0;
    [unroll] for (int it = 0; it < 2; ++it)
    {
        const float win = it == 0 ? 4.0 : 2.0;
        float4 a = 0, b = 0, cc = 0;
        for (int i2 = int(gi); i2 < n; i2 += 256)
        {
            const int3 c = int3(c0 + int2(i2 % span.x, i2 / span.x), 0);
            const float2 v = mid.Load(c);
            const float wc = mid_cost.Load(c).y >= layer_detail ? 1.0 : layer_bg_flat;
            if (wc > 0.0 && all(abs(v - m) <= win)) a += float4(v, 1, 0) * wc;
        }
        GroupSum12(gi, a, b, cc);
        if (a.z > 0) m = a.xy / a.z;
    }
    float3 ax = float3(m.x, 0, 0), ay = float3(m.y, 0, 0);
    float fitted = 0;
    [loop] for (int it2 = 0; it2 < 3; ++it2)
    {
        const float lim = it2 == 0 ? 3.0 : 2.5;
        // Normal equations of the weighted fit: M = sum p p^T (p = 1, dx, dy), r = sum p v.
        float4 s0 = 0, s1 = 0, s2 = 0;
        for (int i3 = int(gi); i3 < n; i3 += 256)
        {
            const int2 cl = c0 + int2(i3 % span.x, i3 / span.x);
            const int3 c = int3(cl, 0);
            const float2 v = mid.Load(c);
            const float2 d = (float2(cl) + 0.5 - centre) / 8.0;
            const float2 pred = float2(dot(ax, float3(1, d)), dot(ay, float3(1, d)));
            // layer_bg_flat: in a fast turn the floor blurs into little detail, and the character's
            // own cells were most of the detailed ones near it - the background "stood still" there.
            const float wc = mid_cost.Load(c).y >= layer_detail ? 1.0 : layer_bg_flat;
            if (wc > 0.0 && length(v - pred) <= lim)
            {
                s0 += float4(1, d.x, d.y, d.x * d.x) * wc;
                s1 += float4(d.x * d.y, d.y * d.y, v.x, v.x * d.x) * wc;
                s2 += float4(v.x * d.y, v.y, v.y * d.x, v.y * d.y) * wc;
            }
        }
        GroupSum12(gi, s0, s1, s2);
        const float3x3 M = float3x3(s0.x, s0.y, s0.z, s0.y, s0.w, s1.x, s0.z, s1.x, s1.y);
        const float3 rx = float3(s1.z, s1.w, s2.x), ry = float3(s2.y, s2.z, s2.w);
        const float det = determinant(M);
        fitted = s0.x;
        if (s0.x >= 12.0 && det > 1e-3 * s0.x * s0.x * s0.x * 0.01)
        {
            // Cramer's rule.
            const float3 c0v = M[0], c1v = M[1], c2v = M[2];  // symmetric: rows are columns
            ax = float3(determinant(float3x3(rx, c1v, c2v)), determinant(float3x3(c0v, rx, c2v)),
                        determinant(float3x3(c0v, c1v, rx))) / det;
            ay = float3(determinant(float3x3(ry, c1v, c2v)), determinant(float3x3(c0v, ry, c2v)),
                        determinant(float3x3(c0v, c1v, ry))) / det;
        }
        else if (s0.x > 0)
        {
            ax = float3(rx.x / s0.x, 0, 0);
            ay = float3(ry.x / s0.x, 0, 0);
        }
    }
    if (gi == 0)
    {
        bg_x[blk] = float4(ax, fitted);
        bg_y[blk] = float4(ay, 0);
    }
}
#endif

#if defined(L_DOM)
Texture2D<float2> mid : register(t0);       // level-0 px
Texture2D<float2> mid_cost : register(t1);  // (cost, detail)
RWTexture2D<float> lrec : register(u0);
RWByteAddressBuffer args : register(u1);
RWByteAddressBuffer args2 : register(u2);  // L_SELECT's, emptied here
#define NB 40  // bins of 4 level-0 px (8 full-res) a side: +-160 full-res px
groupshared uint g_hist[NB * NB];
groupshared uint g_key;
groupshared float3 g_acc[1024];

float3 GroupSum3(uint gi, float3 v)
{
    g_acc[gi] = v;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 512; s > 0; s >>= 1)
    {
        if (gi < s) g_acc[gi] += g_acc[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    const float3 r = g_acc[0];
    GroupMemoryBarrierWithGroupSync();
    return r;
}

[numthreads(1024, 1, 1)]
void DomCS(uint gi : SV_GroupIndex)
{
    for (uint j = gi; j < NB * NB; j += 1024) g_hist[j] = 0;
    if (gi == 0) g_key = 0;
    GroupMemoryBarrierWithGroupSync();
    const int n = grid_size.x * grid_size.y;
    for (int i = int(gi); i < n; i += 1024)
    {
        const int3 c = int3(i % grid_size.x, i / grid_size.x, 0);
        if (mid_cost.Load(c).y >= layer_detail)
        {
            const int2 b = clamp(int2(floor(mid.Load(c) / 4.0)) + NB / 2, 0, NB - 1);
            InterlockedAdd(g_hist[b.y * NB + b.x], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // The peak of the bins summed 3x3, then two mean-shift steps on the vectors.
    for (uint j2 = gi; j2 < NB * NB; j2 += 1024)
    {
        const int bx = int(j2 % NB), by = int(j2 / NB);
        uint s = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const int2 q = int2(bx + k % 3 - 1, by + k / 3 - 1);
            if (all(q >= 0) && all(q < NB)) s += g_hist[q.y * NB + q.x];
        }
        InterlockedMax(g_key, (s << 11) | j2);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint peak = g_key & 2047u;
    float2 vdom = (float2(peak % NB, peak / NB) - NB / 2 + 0.5) * 4.0;
    [unroll] for (int it = 0; it < 2; ++it)
    {
        const float win = it == 0 ? 4.0 : 2.0;
        float3 acc = 0;
        for (int i2 = int(gi); i2 < n; i2 += 1024)
        {
            const int3 c = int3(i2 % grid_size.x, i2 / grid_size.x, 0);
            const float2 v = mid.Load(c);
            if (mid_cost.Load(c).y >= layer_detail && all(abs(v - vdom) <= win)) acc += float3(v, 1);
        }
        const float3 tot = GroupSum3(gi, acc);
        if (tot.z > 0) vdom = tot.xy / tot.z;
    }
    // The second motion: the fullest single bin clear of the dominant one.
    if (gi == 0) g_key = 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint j3 = gi; j3 < NB * NB; j3 += 1024)
    {
        const float2 centre = (float2(j3 % NB, j3 / NB) - NB / 2 + 0.5) * 4.0;
        if (length(centre - vdom) > 5.0) InterlockedMax(g_key, (g_hist[j3] << 11) | j3);
    }
    float other = 0;
    for (int i3 = int(gi); i3 < n; i3 += 1024)
    {
        const int3 c = int3(i3 % grid_size.x, i3 / grid_size.x, 0);
        if (mid_cost.Load(c).y >= layer_detail && length(mid.Load(c) - vdom) * 2.0 > layer_vdiff) other += 1;
    }
    const float3 o = GroupSum3(gi, float3(other, 0, 0));
    const float peak2 = float(g_key >> 11);
    if (gi == 0)
    {
        const bool active = layer_enable != 0 && peak2 >= layer_min_bin;
        [loop] for (int r = 0; r < LR_COUNT; ++r) lrec[int2(r, 0)] = 0;
        lrec[int2(LR_ACTIVE, 0)] = active ? 1.0 : 0.0;
        lrec[int2(LR_VDOM, 0)] = vdom.x * 2.0;
        lrec[int2(LR_VDOM + 1, 0)] = vdom.y * 2.0;
        lrec[int2(LR_PEAK2, 0)] = peak2;
        lrec[int2(LR_OTHER, 0)] = o.x;
        lrec[int2(LR_ROOT, 0)] = -1;
        const uint3 grid = active ? uint3((uint2(grid_size) + 7u) / 8u, 1u) : uint3(0, 0, 0);
        args.Store3(0, grid);
        args.Store3(12, active ? uint3(1, 1, 1) : uint3(0, 0, 0));
        args2.Store3(24, uint3(0, 0, 0));
        args2.Store3(36, uint3(0, 0, 0));
        args2.Store3(48, uint3(0, 0, 0));
        args2.Store3(60, uint3(0, 0, 0));
    }
}
#endif

#if defined(L_MARGIN)
Texture2D<float> luma_a : register(t0);
Texture2D<float> luma_b : register(t1);
Texture2D<float2> mid : register(t2);
Texture2D<float> lrec : register(t3);
Texture2D<float> stats : register(t4);
Texture2D<float4> bgx : register(t5);
Texture2D<float4> bgy : register(t6);
SamplerState linear_clamp : register(s0);
RWTexture2D<float> cand_out : register(u0);  // s_dom - s_own where a minority cell, else -1
RWTexture2D<uint> label_out : register(u1);

// MidCS's BilateralCost: how badly v carries a onto b across the 8x8 window.
float WindowCost(int2 origin, float2 v, float gain)
{
    float sum = 0;
    [loop] for (int y = 0; y < BLOCK; ++y)
    {
        [unroll] for (int x = 0; x < BLOCK; ++x)
        {
            const float2 q = float2(origin + int2(x, y)) + 0.5;
            sum += abs(gain * luma_a.SampleLevel(linear_clamp, (q - t * v) * inv_size, 0) -
                       luma_b.SampleLevel(linear_clamp, (q + (1.0 - t) * v) * inv_size, 0));
        }
    }
    return sum / float(BLOCK * BLOCK);
}

[numthreads(8, 8, 1)]
void MarginCS(uint3 id : SV_DispatchThreadID)
{
    const int2 cell = int2(id.xy);
    if (any(cell >= grid_size)) return;
    const float2 vdom = BgAt(bgx, bgy, cell);
    const float2 center = float2(cell * MID_BLOCK) + MID_BLOCK * 0.5;
    const int2 origin = int2(center) - BLOCK / 2;
    const float2 v = mid.Load(int3(cell, 0));
    const float gain = GainFrom(stats);
    bool cand = false;
    float margin = -1;
    [branch] if (length(v - vdom) * 2.0 > layer_vdiff)
    {
        margin = WindowCost(origin, vdom, gain) - WindowCost(origin, v, gain);
        cand = margin > layer_margin;
    }
    cand_out[cell] = cand ? margin : -1.0;
    label_out[cell] = cand ? uint(cell.y * grid_size.x + cell.x) : NO_LABEL;
}
#endif

#if defined(L_LINK)
Texture2D<float2> mid : register(t0);
RWTexture2D<uint> label : register(u0);

[numthreads(8, 8, 1)]
void LinkCS(uint3 id : SV_DispatchThreadID)
{
    const int2 cell = int2(id.xy);
    if (any(cell >= grid_size)) return;
    uint l = label[cell];
    if (l == NO_LABEL) return;
    const float2 v = mid.Load(int3(cell, 0));
    const int2 offs[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 nb = cell + offs[k];
        if (all(nb >= 0) && all(nb < grid_size))
        {
            const uint ln = label[nb];
            if (ln != NO_LABEL)
                if (length(mid.Load(int3(nb, 0)) - v) * 2.0 <= layer_link) l = min(l, ln);
        }
    }
    label[cell] = min(l, label[int2(CellOf(l))]);
}
#endif

#if defined(L_AREA)
Texture2D<uint> label : register(t0);
Texture2D<float> luma_a : register(t1);  // level 0
Texture2D<float> cand : register(t2);
Texture2D<float2> mid : register(t3);
RWByteAddressBuffer acc : register(u0);

[numthreads(8, 8, 1)]
void AreaCS(uint3 id : SV_DispatchThreadID)
{
    const int2 cell = int2(id.xy);
    if (any(cell >= grid_size)) return;
    const uint l = label.Load(int3(cell, 0));
    if (l == NO_LABEL) return;
    const uint base = Resolve(label, l) * ACC_STRIDE * 4;
    uint dummy;
    acc.InterlockedAdd(base, 1u, dummy);
    acc.InterlockedMax(base + 4, 0xffffu - uint(cell.x), dummy);
    acc.InterlockedMax(base + 8, 0xffffu - uint(cell.y), dummy);
    acc.InterlockedMax(base + 12, uint(cell.x), dummy);
    acc.InterlockedMax(base + 16, uint(cell.y), dummy);
    // Black (the bars beside a 16:9 picture): the edge of the picture in a fast
    // turn is a still, compact minority too.
    float darkest = 1;
    [unroll] for (int k = 0; k < 4; ++k)
        darkest = min(darkest, luma_a.Load(int3(clamp(cell * MID_BLOCK + int2(k & 1, k >> 1) * (MID_BLOCK - 1), 0, src_size - 1), 0)));
    if (darkest < 0.02) acc.InterlockedAdd(base + 20, 1u, dummy);
    acc.InterlockedAdd(base + 24, uint(saturate(cand.Load(int3(cell, 0))) * 1000.0), dummy);
    const float2 mv = mid.Load(int3(cell, 0));
    acc.InterlockedAdd(base + 28, asuint(int(round(mv.x * 16.0))), dummy);
    acc.InterlockedAdd(base + 32, asuint(int(round(mv.y * 16.0))), dummy);
}
#endif

#if defined(L_SELECT)
Texture2D<uint> label : register(t0);
Texture2D<float2> mid : register(t1);
Texture2D<float> cand : register(t2);
ByteAddressBuffer acc : register(t3);
Texture2D<float> luma_a : register(t4);  // level 0
Texture2D<float4> bgx : register(t5);
Texture2D<float4> bgy : register(t6);
RWTexture2D<float> lrec : register(u0);
RWByteAddressBuffer args : register(u1);
groupshared uint g_key;
groupshared float4 g_acc4[1024];

float4 GroupSum4(uint gi, float4 v)
{
    g_acc4[gi] = v;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 512; s > 0; s >>= 1)
    {
        if (gi < s) g_acc4[gi] += g_acc4[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    const float4 r = g_acc4[0];
    GroupMemoryBarrierWithGroupSync();
    return r;
}

// L_SELECT's candidates: every component of at least SEL_MIN cells near enough
// the middle (the score needs central > 0), so a component can be scored with
// the fragments beside it (layer_merge).
#define SEL_MAX 512
#define SEL_MIN 3
groupshared uint g_n;
groupshared uint g_root[SEL_MAX];
groupshared uint g_b0[SEL_MAX];    // x0 | y0 << 16 (cells)
groupshared uint g_b1[SEL_MAX];    // x1 | y1 << 16
groupshared uint g_ab[SEL_MAX];    // area | black cells << 16
groupshared float g_m[SEL_MAX];    // margin sum
groupshared float2 g_v[SEL_MAX];   // mean motion (full-res px)
groupshared uint g_win;
#define FRAG_MAX 128
groupshared uint g_nf;
groupshared uint g_frag[FRAG_MAX];  // candidates that are Fragment()s
groupshared uint g_nmem;
groupshared uint g_mem[GROUP_MAX];
groupshared int g_box[4];

int4 SelBox(uint k) { return int4(g_b0[k] & 0xffffu, g_b0[k] >> 16, g_b1[k] & 0xffffu, g_b1[k] >> 16); }

// A candidate that may join another (layer_merge): a small fragment the background's motion
// clearly fails on (a piece of the head top against the floor: 7-18 cells, margin 0.17-0.20).
// Linking minority cells that far apart instead ran from the head down the body and the legs
// into the floor (needle 13/15); large or weak pieces let a body pick up its scraps and beat
// the head of an orbit by a hair.
bool Fragment(uint j)
{
    const float aj = float(g_ab[j] & 0xffffu);
    return aj <= 2.0 * layer_min_area && g_m[j] >= 0.1 * aj;
}

// Fragment j joins candidate k: within layer_merge empty cells of the box gathered so far
// (bk) and moving like k (layer_link).
bool Near(int4 bk, uint k, uint j)
{
    const int4 bj = SelBox(j);
    const int d = max(max(bj.x - bk.z, bk.x - bj.z), max(bj.y - bk.w, bk.y - bj.w));
    return j != k && d <= int(layer_merge) + 1 && length(g_v[j] - g_v[k]) <= layer_link;
}

// layer_merge_core: candidate k may take fragments only when it is the kind of core the merge is
// for - the player's head in pieces, slow on the screen near the middle across. A fast part
// off the middle picking up scraps pasted a doorway beside a shoulder (20261026-020715-337) and a
// square at a crossguard (20261028-210851-361).
bool MergeCore(uint k)
{
    if (layer_merge_core <= 0.0) return true;
    const int4 b = SelBox(k);
    const float dx = abs(float(b.x + b.z + 1) * 0.5 - float(grid_size.x) * 0.5) * LAYER_CELL;
    return length(g_v[k]) <= max(layer_still, 1.0) && dx <= max(layer_still_dx, 0.0) + 150.0;
}

// The box of k and its fragments: gathered twice, so a fragment on the far side of the head
// (the core is often one side of it) joins through the ones between.
int4 GroupBox(uint k)
{
    int4 reach = SelBox(k);
    [loop] for (int round = 0; round < 2; ++round)
    {
        int4 ub = SelBox(k);
        uint members = 1;
        [loop] for (uint f = 0; f < g_nf; ++f)
        {
            const uint j = g_frag[f];
            [branch] if (members < GROUP_MAX && Near(reach, k, j))
            {
                const int4 bj = SelBox(j);
                ub = int4(min(ub.xy, bj.xy), max(ub.zw, bj.zw));
                members += 1;
            }
        }
        reach = ub;
    }
    return reach;
}

[numthreads(1024, 1, 1)]
void SelectCS(uint gi : SV_GroupIndex)
{
    if (gi == 0)
    {
        g_key = 0;
        g_n = 0;
        g_nf = 0;
        g_nmem = 0;
        g_win = 0;
        g_box[0] = 0x7fffffff;
        g_box[1] = 0x7fffffff;
        g_box[2] = -1;
        g_box[3] = -1;
    }
    GroupMemoryBarrierWithGroupSync();
    const int n = grid_size.x * grid_size.y;
    // The candidates.
    for (int i = int(gi); i < n; i += 1024)
    {
        const int3 c = int3(i % grid_size.x, i / grid_size.x, 0);
        if (label.Load(c) == uint(i))
        {
            const uint base = uint(i) * ACC_STRIDE * 4;
            const uint area = acc.Load(base);
            if (area >= SEL_MIN)
            {
                const int x0 = int(0xffffu - acc.Load(base + 4)), y0 = int(0xffffu - acc.Load(base + 8));
                const int x1 = int(acc.Load(base + 12)), y1 = int(acc.Load(base + 16));
                const float2 off = (float2(x0 + x1 + 1, y0 + y1 + 1) * 0.5 - float2(grid_size) * 0.5) / (float2(grid_size) * 0.5);
                if (1.0 - 1.5 * dot(off, off) > 0.0)
                {
                    uint k;
                    InterlockedAdd(g_n, 1u, k);
                    if (k < SEL_MAX)
                    {
                        g_root[k] = uint(i);
                        g_b0[k] = uint(x0) | (uint(y0) << 16);
                        g_b1[k] = uint(x1) | (uint(y1) << 16);
                        g_ab[k] = min(area, 0xffffu) | (min(acc.Load(base + 20), 0xffffu) << 16);
                        g_m[k] = float(acc.Load(base + 24)) / 1000.0;
                        g_v[k] = float2(asint(acc.Load(base + 28)), asint(acc.Load(base + 32))) / (16.0 * float(area)) * 2.0;
                    }
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint cands = min(g_n, uint(SEL_MAX));
    // layer_select_dom: merging fragments, stillness and the middle across are about a camera
    // following the player. With the camera nearly still (a fight, a menu) the next layer down
    // is as likely an effect or a weapon, and they changed the choice there for the worse
    // (blocks in a fire ring, foliage torn across lightning) - the plain score decides.
    const bool follow = layer_select_dom <= 0.0 || length(float2(lrec[int2(LR_VDOM, 0)], lrec[int2(LR_VDOM + 1, 0)])) >= layer_select_dom;
    if (layer_merge > 0.0 && follow)
    {
        for (uint k0 = gi; k0 < cands; k0 += 1024)
        {
            if (Fragment(k0))
            {
                uint f;
                InterlockedAdd(g_nf, 1u, f);
                if (f < FRAG_MAX) g_frag[f] = k0;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) g_nf = min(g_nf, uint(FRAG_MAX));
    GroupMemoryBarrierWithGroupSync();
    // The largest compact component (with the fragments beside it).
    for (uint k = gi; k < cands; k += 1024)
    {
        float area = float(g_ab[k] & 0xffffu), blacks = float(g_ab[k] >> 16), msum = g_m[k];
        float2 vsum = g_v[k] * area;
        int4 ub = SelBox(k);
        [branch] if (layer_merge > 0.0 && g_nf > 0 && MergeCore(k))
        {
            // The fragments within reach of the box GroupBox gathered.
            const int4 reach = GroupBox(k);
            uint members = 1;
            [loop] for (uint f = 0; f < g_nf; ++f)
            {
                const uint j = g_frag[f];
                [branch] if (members < GROUP_MAX && Near(reach, k, j))
                {
                    const float aj = float(g_ab[j] & 0xffffu);
                    area += aj;
                    blacks += float(g_ab[j] >> 16);
                    msum += g_m[j];
                    vsum += g_v[j] * aj;
                    const int4 bj = SelBox(j);
                    ub = int4(min(ub.xy, bj.xy), max(ub.zw, bj.zw));
                    members += 1;
                }
            }
        }
        if (area >= layer_min_area && area <= layer_max_area)
        {
            const int x0 = ub.x, y0 = ub.y, x1 = ub.z, y1 = ub.w;
            const float w = float(x1 - x0 + 1), h = float(y1 - y0 + 1);
            const int4 kb = SelBox(k);
            const float core_area = float(g_ab[k] & 0xffffu);
            // Fragments with gaps between them fill less of their joint box than one piece: the
            // component itself filling its own box is enough.
            const bool filled = area >= layer_fill * w * h || core_area >= layer_fill * float((kb.z - kb.x + 1) * (kb.w - kb.y + 1));
            // Larger wins, and nearer the middle of the picture: the character the camera
            // follows, not a HUD panel along an edge.
            const float2 off = (float2(x0 + x1 + 1, y0 + y1 + 1) * 0.5 - float2(grid_size) * 0.5) / (float2(grid_size) * 0.5);
            const float central = saturate(1.0 - 1.5 * dot(off, off));
            const bool black = blacks > 0.1 * area;
            // ... and the clearer the background's motion fails on it (a head against the floor,
            // rather than legs swinging through their own motion).
            const float margin = msum / area;
            float score = area * central * saturate(margin / 0.1);
            const float2 vc = vsum / area;
            // The camera follows the player: he is the one standing nearly still on the screen,
            // while people walking beside him cross it at 8-25 px a frame (layer_still) - a
            // factor, not a gate, so a running player the camera lags still wins when alone. Only
            // off the middle across (layer_still_dx, full 150 px further): the NPCs beside him stand
            // 260-450 px off it, while in the middle the faster part is as often the player's own
            // head (running ahead of the camera: 25-50 px, a dodge: 15 px) as a cape swinging past it.
            const float dx = abs(float(x0 + x1 + 1) * 0.5 - float(grid_size.x) * 0.5) * LAYER_CELL;
            if (layer_still > 0.0 && follow)
            {
                const float side = layer_still_dx < 0.0 ? 1.0 : saturate((dx - layer_still_dx) / 150.0);
                score *= pow(layer_still / max(length(vc), layer_still), layer_still_pow * side);
            }
            // ... and near the middle across (an NPC beside him is 300-500 px off it).
            if (layer_hcentre > 0.0 && follow)
            {
                const float dxp = max(dx - layer_hcentre, 0.0) / (0.6 * layer_hcentre);
                score /= 1.0 + dxp * dxp;
            }
            // layer_select_gates: too fast or too wide to be the character the camera follows
            // (an ally running past, a near pillar's parallax) - the next one down may be; but
            // not one far off the middle (a HUD bar, the minimap - still against a fast turn).
            const bool fits = layer_select_gates == 0.0 ||
                              ((layer_vmax <= 0.0 || length(vc) <= layer_vmax) && (layer_wmax <= 0.0 || w <= layer_wmax) && central >= 0.25);
            if (filled && w < 0.6 * float(grid_size.x) && h < 0.8 * float(grid_size.y) && !black && fits)
                InterlockedMax(g_key, (min(uint(score * 16.0), 0x1ffffu) << 15) | g_root[k]);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint key = g_key;
    const uint root = key == 0 ? NO_LABEL : (key & 0x7fffu);
    // The winner's fragments and their joint box.
    for (uint k2 = gi; k2 < cands; k2 += 1024)
        if (g_root[k2] == root) g_win = k2;
    GroupMemoryBarrierWithGroupSync();
    if (root != NO_LABEL && gi == 0)
    {
        // The same members as its score counted, in the same order.
        int4 ub = SelBox(g_win);
        g_mem[0] = root;
        uint m = 1;
        [branch] if (layer_merge > 0.0 && g_nf > 0 && MergeCore(g_win))
        {
            const int4 reach = GroupBox(g_win);
            [loop] for (uint f = 0; f < g_nf; ++f)
            {
                const uint j2 = g_frag[f];
                [branch] if (m < GROUP_MAX && Near(reach, g_win, j2))
                {
                    g_mem[m] = g_root[j2];
                    const int4 bj = SelBox(j2);
                    ub = int4(min(ub.xy, bj.xy), max(ub.zw, bj.zw));
                    m += 1;
                }
            }
        }
        g_nmem = m;
        g_box[0] = ub.x;
        g_box[1] = ub.y;
        g_box[2] = ub.z;
        g_box[3] = ub.w;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint nmem = min(g_nmem, uint(GROUP_MAX));
    float4 s = 0, s2 = 0;
    for (int i2 = int(gi); i2 < n; i2 += 1024)
    {
        const int3 c = int3(i2 % grid_size.x, i2 / grid_size.x, 0);
        const uint l = label.Load(c);
        if (root != NO_LABEL && l != NO_LABEL)
        {
            const uint r = Resolve(label, l);
            bool member = false;
            [loop] for (uint m2 = 0; m2 < nmem; ++m2) member = member || g_mem[m2] == r;
            if (member)
            {
                s += float4(mid.Load(c), 1, cand.Load(c));
                s2 += float4(luma_a.Load(int3(clamp(c.xy * MID_BLOCK + MID_BLOCK / 2, 0, src_size - 1), 0)), float2(c.xy), 0);
            }
        }
    }
    const float4 tot = GroupSum4(gi, s);
    const float4 tot2 = GroupSum4(gi, s2);
    if (gi == 0 && root != NO_LABEL && tot.z > 0)
    {
        const int x0 = g_box[0], y0 = g_box[1], x1 = g_box[2], y1 = g_box[3];
        lrec[int2(LR_NGROUP, 0)] = float(nmem);
        lrec[int2(LR_CANDS, 0)] = float(g_n);  // evaluator: candidates before the SEL_MAX cut
        [loop] for (uint m3 = 0; m3 < nmem; ++m3) lrec[int2(LR_GROUP + m3, 0)] = float(g_mem[m3]);
        const float luma = tot2.x / tot.z;
        // The camera follows the character: the layer is slow on the screen (layer_vmax) and
        // head-sized (layer_wmax). When the clearest compact minority is a fast effect, a weapon
        // swung or a close-up body, the picture is not that kind of scene: no layer at all
        // (the next one down the list is a fragment of the same effect, not the character).
        const float2 vmean = tot.xy / tot.z * 2.0;
        const bool follows = (layer_vmax <= 0.0 || length(vmean) <= layer_vmax) && (layer_wmax <= 0.0 || float(x1 - x0 + 1) <= layer_wmax);
        const bool alive = luma >= layer_dark && follows;
        const int band = int(layer_band);
        const int bx0 = max(x0 - band, 0), by0 = max(y0 - band, 0);
        const int bx1 = min(x1 + band, grid_size.x - 1), by1 = min(y1 + band, grid_size.y - 1);
        lrec[int2(LR_ROOT, 0)] = float(root);
        lrec[int2(LR_VK, 0)] = tot.x / tot.z * 2.0;
        lrec[int2(LR_VK + 1, 0)] = tot.y / tot.z * 2.0;
        lrec[int2(LR_V0, 0)] = tot.x / tot.z * 2.0;
        lrec[int2(LR_V0 + 1, 0)] = tot.y / tot.z * 2.0;
        lrec[int2(LR_CBOX, 0)] = x0;
        lrec[int2(LR_CBOX + 1, 0)] = y0;
        lrec[int2(LR_CBOX + 2, 0)] = x1;
        lrec[int2(LR_CBOX + 3, 0)] = y1;
        lrec[int2(LR_AREA, 0)] = tot.z;
        lrec[int2(LR_FILL, 0)] = tot.z / float((x1 - x0 + 1) * (y1 - y0 + 1));
        lrec[int2(LR_MARGIN, 0)] = tot.w / tot.z;
        lrec[int2(LR_LUMA, 0)] = luma;
        const float2 vbg = BgAt(bgx, bgy, int2(tot2.yz / tot.z)) * 2.0;
        lrec[int2(LR_VBG, 0)] = vbg.x;
        lrec[int2(LR_VBG + 1, 0)] = vbg.y;
        lrec[int2(LR_ALIVE, 0)] = alive ? 1.0 : 0.0;
        lrec[int2(LR_BOX, 0)] = bx0;
        lrec[int2(LR_BOX + 1, 0)] = by0;
        lrec[int2(LR_BOX + 2, 0)] = bx1;
        lrec[int2(LR_BOX + 3, 0)] = by1;
        const uint bw = uint(bx1 - bx0 + 1), bh = uint(by1 - by0 + 1);
        args.Store3(24, alive ? uint3(bw, bh, 1) : uint3(0, 0, 0));
        args.Store3(36, alive ? uint3((bw * LAYER_CELL + 31) / 32, (bh * LAYER_CELL + 31) / 32, 1) : uint3(0, 0, 0));
        args.Store3(48, alive ? uint3(VSIDE, VSIDE, 1) : uint3(0, 0, 0));
        args.Store3(60, alive ? uint3(1, 1, 1) : uint3(0, 0, 0));
    }
}
#endif

#if defined(L_CELL)
Texture2D<float4> color_a : register(t0);
Texture2D<float4> color_b : register(t1);
Texture2D<float> stats : register(t2);
Texture2D<float> lrec : register(t3);
Texture2D<uint> label : register(t4);
Texture2D<float2> mid : register(t5);
Texture2D<float> cand : register(t6);
Texture2D<float4> bgx : register(t7);
Texture2D<float4> bgy : register(t8);
SamplerState linear_clamp : register(s0);
Texture2D<uint> cls : register(t9);  // L_CLASS
RWTexture2D<float4> sum_fa : register(u1);  // per cell: layer colour sum (rgb) and count, frame a
RWTexture2D<float4> sum_ba : register(u2);  // background, frame a
RWTexture2D<float4> sum_fb : register(u3);
RWTexture2D<float4> sum_bb : register(u4);
RWByteAddressBuffer dbg_acc : register(u5);
groupshared uint g_sum[16];

void Add(int slot, float3 c)
{
    const uint3 q = uint3(saturate(c) * 1024.0 + 0.5);
    InterlockedAdd(g_sum[slot * 4 + 0], q.r);
    InterlockedAdd(g_sum[slot * 4 + 1], q.g);
    InterlockedAdd(g_sum[slot * 4 + 2], q.b);
    InterlockedAdd(g_sum[slot * 4 + 3], 1u);
}

[numthreads(16, 16, 1)]
void CellCS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const int4 bc = BoxCells(lrec);
    const int2 cell = bc.xy + int2(gid.xy);
    if (gi < 16) g_sum[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    const uint cl = cls.Load(int3(cell, 0));
    const uint klass = cl & 3u;
    const bool background = (cl & 4u) != 0u;
    const float2 vdom = BgAt(bgx, bgy, cell) * 2.0;
    const float gain = GainFrom(stats);
    const float2 vk = float2(LR(lrec, LR_VK), LR(lrec, LR_VK + 1));
    const int2 y = cell * LAYER_CELL + int2(tid.xy);
    const int2 pa = y + OffsetA(vk), pb = y + OffsetB(vk);
    const int2 maxp = src_size - 1;
    if (klass <= 1u || background)
    {
        const float3 ca = gain * color_a.Load(int3(clamp(pa, 0, maxp), 0)).rgb;
        const float3 cb = color_b.Load(int3(clamp(pb, 0, maxp), 0)).rgb;
        const bool ina = all(pa >= 0) && all(pa <= maxp), inb = all(pb >= 0) && all(pb <= maxp);
        const bool fa = BgFails(color_a, color_b, linear_clamp, float2(pa) + 0.5, vdom, gain, 1.0);
        const bool fb = BgFails(color_b, color_a, linear_clamp, float2(pb) + 0.5, -vdom, 1.0, gain);
        if (klass <= 1u)
        {
            if (ina && fa) Add(0, ca);
            if (inb && fb) Add(2, cb);
        }
        else
        {
            if (ina && !fa) Add(1, ca);
            if (inb && !fb) Add(3, cb);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        uint dummy;
        const uint dbase = uint(grid_size.x * grid_size.y) * ACC_STRIDE * 4 + 24;
        if (klass == 0u) dbg_acc.InterlockedAdd(dbase + 0, 1u, dummy);
        if (klass == 1u) dbg_acc.InterlockedAdd(dbase + 4, 1u, dummy);
        if (background) dbg_acc.InterlockedAdd(dbase + 8, 1u, dummy);
        dbg_acc.InterlockedAdd(dbase + 12, g_sum[3], dummy);
        dbg_acc.InterlockedAdd(dbase + 16, g_sum[7], dummy);
        sum_fa[cell] = float4(float3(g_sum[0], g_sum[1], g_sum[2]) / 1024.0, g_sum[3]);
        sum_ba[cell] = float4(float3(g_sum[4], g_sum[5], g_sum[6]) / 1024.0, g_sum[7]);
        sum_fb[cell] = float4(float3(g_sum[8], g_sum[9], g_sum[10]) / 1024.0, g_sum[11]);
        sum_bb[cell] = float4(float3(g_sum[12], g_sum[13], g_sum[14]) / 1024.0, g_sum[15]);
    }
}
#endif

#if defined(L_CLASS)
Texture2D<float> lrec : register(t0);
Texture2D<uint> label : register(t1);
Texture2D<float2> mid : register(t2);
Texture2D<float> cand : register(t3);
Texture2D<float4> bgx : register(t4);
Texture2D<float4> bgy : register(t5);
RWTexture2D<uint> cls_out : register(u0);  // bits 0-1: 0 inner layer cell, 1 layer cell, 2 band, 3 outside; bit 2: background cell

bool InComp(int2 q, uint root)
{
    if (any(q < 0) || any(q >= grid_size)) return false;
    const uint l = label.Load(int3(q, 0));
    if (l == NO_LABEL) return false;
    const uint r = Resolve(label, l);
    const int members = int(LR(lrec, LR_NGROUP));
    bool member = r == root;
    [loop] for (int m = 0; m < members; ++m) member = member || uint(LR(lrec, LR_GROUP + m)) == r;
    return member;
}

// Per box cell: inside the layer (all four neighbours too: inner), on it, in
// the band around it (Chebyshev distance up to layer_band), or out; and
// whether it is plain background (moves with the background's motion).
[numthreads(1, 1, 1)]
void ClassCS(uint3 gid : SV_GroupID)
{
    const int4 bc = BoxCells(lrec);
    const int2 cell = bc.xy + int2(gid.xy);
    const uint root = uint(LR(lrec, LR_ROOT));
    const int band = int(layer_band);
    uint dist = 99;
    [loop] for (int dy = -band; dy <= band; ++dy)
        [loop] for (int dx = -band; dx <= band; ++dx)
            if (InComp(cell + int2(dx, dy), root)) dist = min(dist, uint(max(abs(dx), abs(dy))));
    const bool inner = InComp(cell, root) && InComp(cell + int2(1, 0), root) && InComp(cell + int2(-1, 0), root) &&
                       InComp(cell + int2(0, 1), root) && InComp(cell + int2(0, -1), root);
    const uint klass = dist == 0 ? (inner ? 0u : 1u) : (dist <= uint(band) ? 2u : 3u);
    const float2 vdom = BgAt(bgx, bgy, cell) * 2.0;
    const bool background = cand.Load(int3(cell, 0)) < 0 && length(mid.Load(int3(cell, 0)) * 2.0 - vdom) <= layer_vdiff;
    cls_out[cell] = klass | (background ? 4u : 0u);
}
#endif

#if defined(L_REFINE)
Texture2D<float4> color_a : register(t0);
Texture2D<float4> color_b : register(t1);
Texture2D<float> stats : register(t2);
Texture2D<float> lrec : register(t3);
Texture2D<uint> cls : register(t4);
RWTexture2D<float> vcost : register(u0);  // VSIDE x VSIDE
groupshared float g_c[256];
groupshared float g_n[256];

// The layer's own motion. The mean of its cells' motions is only a start
// (cells on its edge carry some of the background's); one group per offset
// within VSEARCH of it: how well a's inner layer cells (sampled every 4 px)
// land on b. Inner cells only, so the background around the object does not
// pull the motion towards its own.
[numthreads(256, 1, 1)]
void RefineCS(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const int4 cb = int4(LR(lrec, LR_CBOX), LR(lrec, LR_CBOX + 1), LR(lrec, LR_CBOX + 2), LR(lrec, LR_CBOX + 3));
    const float2 v0 = float2(LR(lrec, LR_V0), LR(lrec, LR_V0 + 1));
    const float2 v = round(v0) + float2(gid.xy) - VSEARCH;
    const int2 oa = int2(round(-t * v0));
    const float gain = GainFrom(stats);
    const int2 span = cb.zw - cb.xy + 1;
    const int n = span.x * span.y * 16;
    const int2 maxp = src_size - 1;
    float c = 0, cnt = 0;
    // A large layer (a character the camera turns around fills a quarter of the
    // screen) is sampled sparser: at most about 2048 samples per offset.
    const int step = max(1, n / 2048);
    for (int i = int(gi) * step; i < n; i += 256 * step)
    {
        const int ci = i / 16, si = i % 16;
        const int2 cell = cb.xy + int2(ci % span.x, ci / span.x);
        if ((cls.Load(int3(cell, 0)) & 3u) == 0u)
        {
            const int2 pa = clamp(cell * LAYER_CELL + int2(si % 4, si / 4) * 4 + 2 + oa, 0, maxp);
            const float3 d = abs(gain * color_a.Load(int3(pa, 0)).rgb - color_b.Load(int3(clamp(pa + int2(v), 0, maxp), 0)).rgb);
            c += d.r + d.g + d.b;
            cnt += 1;
        }
    }
    g_c[gi] = c;
    g_n[gi] = cnt;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 128; s > 0; s >>= 1)
    {
        if (gi < s)
        {
            g_c[gi] += g_c[gi + s];
            g_n[gi] += g_n[gi + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) vcost[int2(gid.xy)] = g_n[0] > 0 ? g_c[0] / g_n[0] : 1e9;
}
#endif

#if defined(L_PICK)
Texture2D<float> vcost : register(t0);
RWTexture2D<float> lrec : register(u0);
groupshared uint g_key;

// The cheapest offset (a small distance penalty keeps flat layers near the
// start), refined to a fraction of a pixel by a parabola each way.
[numthreads(256, 1, 1)]
void PickCS(uint gi : SV_GroupIndex)
{
    if (gi == 0) g_key = 0xffffffffu;
    GroupMemoryBarrierWithGroupSync();
    for (int i = int(gi); i < VSIDE * VSIDE; i += 256)
    {
        const int2 o = int2(i % VSIDE, i / VSIDE);
        const float c = vcost.Load(int3(o, 0)) + 0.0005 * length(float2(o - VSEARCH));
        InterlockedMin(g_key, (min(uint(c * 65536.0), 0x7fffffu) << 9) | 0u);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint best = g_key;
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) g_key = 0xffffffffu;
    GroupMemoryBarrierWithGroupSync();
    // The index of an offset with that cost (the lowest index among equals).
    for (int i2 = int(gi); i2 < VSIDE * VSIDE; i2 += 256)
    {
        const int2 o = int2(i2 % VSIDE, i2 / VSIDE);
        const float c = vcost.Load(int3(o, 0)) + 0.0005 * length(float2(o - VSEARCH));
        if (((min(uint(c * 65536.0), 0x7fffffu) << 9) | 0u) == best) InterlockedMin(g_key, uint(i2));
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        const int k = int(g_key);
        const int2 o = int2(k % VSIDE, k / VSIDE);
        float2 sub = 0;
        if (o.x > 0 && o.x < VSIDE - 1)
        {
            const float c0 = vcost.Load(int3(o - int2(1, 0), 0)), c1 = vcost.Load(int3(o, 0)), c2 = vcost.Load(int3(o + int2(1, 0), 0));
            const float den = c0 - 2.0 * c1 + c2;
            if (den > 1e-6) sub.x = clamp(0.5 * (c0 - c2) / den, -0.5, 0.5);
        }
        if (o.y > 0 && o.y < VSIDE - 1)
        {
            const float c0 = vcost.Load(int3(o - int2(0, 1), 0)), c1 = vcost.Load(int3(o, 0)), c2 = vcost.Load(int3(o + int2(0, 1), 0));
            const float den = c0 - 2.0 * c1 + c2;
            if (den > 1e-6) sub.y = clamp(0.5 * (c0 - c2) / den, -0.5, 0.5);
        }
        const float2 v0 = float2(lrec[int2(LR_V0, 0)], lrec[int2(LR_V0 + 1, 0)]);
        const float2 v = round(v0) + float2(o - VSEARCH) + sub;
        // layer_refine_edge: no inner cells (every offset costs 1e9 and the first, v0 - 16 px
        // both ways, won), or with 2 also the cheapest offset on the edge of the search (not a
        // minimum inside it - a valley running out of the window): the cells' mean stands.
        const bool no_fit = (layer_refine_edge >= 1.0 && vcost.Load(int3(o, 0)) >= 1e8) ||
                            (layer_refine_edge >= 2.0 && (any(o == 0) || any(o == VSIDE - 1)));
        if (layer_refine != 0 && !no_fit)
        {
            lrec[int2(LR_VK, 0)] = v.x;
            lrec[int2(LR_VK + 1, 0)] = v.y;
        }
        lrec[int2(LR_VCOST, 0)] = vcost.Load(int3(o, 0));
        lrec[int2(LR_VCOST + 1, 0)] = vcost.Load(int3(VSEARCH, VSEARCH, 0));
    }
}
#endif

#if defined(L_SPREAD)
Texture2D<float> lrec : register(t0);
Texture2D<float4> sum_fa : register(t1);
Texture2D<float4> sum_ba : register(t2);
Texture2D<float4> sum_fb : register(t3);
Texture2D<float4> sum_bb : register(t4);
RWTexture2D<float4> ref_fa : register(u0);  // weighted sums of the cells around: (rgb sum, count)
RWTexture2D<float4> ref_ba : register(u1);
RWTexture2D<float4> ref_fb : register(u2);
RWTexture2D<float4> ref_bb : register(u3);

// Per box cell: the layer's and the background's colour near it, the nearest
// cells weighing most (1 / (1 + d^2)^2), out to the band.
[numthreads(1, 1, 1)]
void SpreadCS(uint3 gid : SV_GroupID)
{
    const int4 bc = BoxCells(lrec);
    const int2 cell = bc.xy + int2(gid.xy);
    const int band = int(layer_band);
    float4 fa = 0, ba = 0, fb = 0, bb = 0;
    [loop] for (int dy = -band; dy <= band; ++dy)
    {
        [loop] for (int dx = -band; dx <= band; ++dx)
        {
            const int2 q = cell + int2(dx, dy);
            if (all(q >= bc.xy) && all(q <= bc.zw))
            {
                const float r2 = float(dx * dx + dy * dy);
                const float w = 1.0 / ((1.0 + r2) * (1.0 + r2));
                fa += w * sum_fa.Load(int3(q, 0));
                ba += w * sum_ba.Load(int3(q, 0));
                fb += w * sum_fb.Load(int3(q, 0));
                bb += w * sum_bb.Load(int3(q, 0));
            }
        }
    }
    ref_fa[cell] = fa;
    ref_ba[cell] = ba;
    ref_fb[cell] = fb;
    ref_bb[cell] = bb;
}
#endif

#if defined(L_INIT)
Texture2D<float4> color_a : register(t0);
Texture2D<float4> color_b : register(t1);
Texture2D<float> stats : register(t2);
Texture2D<float> lrec : register(t3);
Texture2D<uint> cls : register(t4);
Texture2D<float4> ref_fa : register(t5);
Texture2D<float4> ref_ba : register(t6);
Texture2D<float4> ref_fb : register(t7);
Texture2D<float4> ref_bb : register(t8);
Texture2D<float4> bgx : register(t9);
Texture2D<float4> bgy : register(t10);
Texture2D<float2> mid : register(t11);
SamplerState linear_clamp : register(s0);
RWTexture2D<uint> mstate : register(u0);  // bits 0-1 frame a, 2-3 frame b: 0 no, 1 candidate, 2 in

// A reference colour at full-res pixel y: the cells' weighted sums, bilinear
// between cell centres (inside the box only), divided out. w = 0: none known.
float4 RefAt(Texture2D<float4> ref, int4 bc, int2 y)
{
    const float2 g = (float2(y) + 0.5) / float(LAYER_CELL) - 0.5;
    const int2 c0 = int2(floor(g));
    const float2 f = g - float2(c0);
    float4 s = 0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 q = clamp(c0 + o, bc.xy, bc.zw);
        const float2 w = lerp(1.0 - f, f, float2(o));
        s += w.x * w.y * ref.Load(int3(q, 0));
    }
    return s.w > 1e-3 ? float4(s.rgb / s.w, s.w) : float4(0, 0, 0, 0);
}

uint State(uint klass, bool inside, bool fail, float3 c, float4 fg, float4 bg, bool fits)
{
    if (!inside || !fail || klass > 2u) return 0;
    const bool edge = fg.w > 0 && bg.w > 0 && SumAbs(fg.rgb - bg.rgb) >= layer_edge;
    const float closer = SumAbs(c - bg.rgb) - SumAbs(c - fg.rgb);
    // The layer's colour, or (where layer and background look alike - dark hair against a
    // dark pillar) the layer's own motion carrying the pixel onto the other frame.
    const bool colour = (edge && closer >= layer_colour) || fits;
    // Inner cells: all but what looks like the background (no hard cell edges).
    // layer_inner_colour: inner cells too need the layer's colour (or its motion's fit) - taking all of an
    // inner cell where layer and background look alike cut the layer's outline along the 16 px
    // cell edges (a square step in a hoof); the hole filling closes what this leaves inside.
    const bool inner_ok = layer_inner_colour > 0.0 ? colour || (edge && closer >= 0.0) : (!edge || closer >= 0.0);
    if ((klass == 0u && inner_ok) || (klass == 1u && colour)) return 2;
    return colour ? 1 : 0;
}

[numthreads(16, 16, 1)]
void MaskInitCS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const int4 bc = BoxCells(lrec);
    const int2 cell = bc.xy + int2(gid.xy);
    const int2 y = cell * LAYER_CELL + int2(tid.xy);
    if (any(y >= src_size)) return;
    const uint klass = cls.Load(int3(cell, 0)) & 3u;
    const float gain = GainFrom(stats);
    const float2 vk = float2(LR(lrec, LR_VK), LR(lrec, LR_VK + 1));
    const float2 vdom = BgAt(bgx, bgy, cell) * 2.0;
    const int2 pa = y + OffsetA(vk), pb = y + OffsetB(vk);
    const int2 maxp = src_size - 1;
    uint sa = 0, sb = 0;
    [branch] if (klass <= 2u)
    {
        const bool ina = all(pa >= 0) && all(pa <= maxp), inb = all(pb >= 0) && all(pb <= maxp);
        const float3 ca = gain * color_a.Load(int3(clamp(pa, 0, maxp), 0)).rgb;
        const float3 cb = color_b.Load(int3(clamp(pb, 0, maxp), 0)).rgb;
        bool fa = BgFails(color_a, color_b, linear_clamp, float2(pa) + 0.5, vdom, gain, 1.0);
        bool fb = BgFails(color_b, color_a, linear_clamp, float2(pb) + 0.5, -vdom, 1.0, gain);
        // layer_nbmotion >= 2: the background's motion "carries" a pixel only onto the background. Where
        // it lands on the layer's own cells in the other frame (the head moved over that spot), the
        // match is hair on hair - a spike carried onto the skull - and says nothing.
        [branch] if (layer_nbmotion >= 2.0)
        {
            const int2 la = clamp(int2(round(float2(pa) + vdom - (1.0 - t) * vk)) / LAYER_CELL, 0, grid_size - 1);
            const int2 lb = clamp(int2(round(float2(pb) - vdom + t * vk)) / LAYER_CELL, 0, grid_size - 1);
            if (!fa && all(la >= bc.xy) && all(la <= bc.zw) && (cls.Load(int3(la, 0)) & 3u) <= 1u) fa = true;
            if (!fb && all(lb >= bc.xy) && all(lb <= bc.zw) && (cls.Load(int3(lb, 0)) & 3u) <= 1u) fb = true;
        }
        // Outside the layer's cells, a pixel its own cell's motion explains (another part of the
        // character - a coat moving with the body, not the head) is not the layer's either.
        const float2 vc = mid.Load(int3(cell, 0)) * 2.0;
        [branch] if (klass == 2u && length(vc - vk) > layer_vdiff)
        {
            fa = fa && BgCost(color_a, color_b, linear_clamp, float2(pa) + 0.5, vc, gain, 1.0) > layer_bgfail;
            fb = fb && BgCost(color_b, color_a, linear_clamp, float2(pb) + 0.5, -vc, 1.0, gain) > layer_bgfail;
        }
        // layer_nbmotion: on the layer's rim and in the band, a pixel that a motion found in the 3x3
        // cells around carries onto the other frame - and that is neither the layer's nor the
        // background's model - is something else's: another character crossing behind the head,
        // or an edge of the scenery the background model misses by a pixel or two.
        [branch] if (layer_nbmotion > 0.0 && klass >= 1u)
        {
            [loop] for (int k = 0; k < 9; ++k)
            {
                const int2 q = clamp(cell + int2(k % 3 - 1, k / 3 - 1), 0, grid_size - 1);
                const float2 vn = mid.Load(int3(q, 0)) * 2.0;
                // layer_nb_own: the layer's own cells are not something else. A head that turns while
                // the body runs moves unlike the layer's mean, and its own cells' motion explained its
                // hair - the inside of the head left the mask in 16 px squares.
                const bool own = layer_nb_own > 0.0 && all(q >= bc.xy) && all(q <= bc.zw) && (cls.Load(int3(q, 0)) & 3u) <= 1u;
                [branch] if (!own && length(vn - vk) > layer_vdiff)
                {
                    if (fa) fa = BgCost(color_a, color_b, linear_clamp, float2(pa) + 0.5, vn, gain, 1.0) > layer_bgfail;
                    if (fb) fb = BgCost(color_b, color_a, linear_clamp, float2(pb) + 0.5, -vn, 1.0, gain) > layer_bgfail;
                }
            }
        }
        bool fita = false, fitb = false;
        [branch] if (layer_fit > 0.0)
        {
            fita = BgCost(color_a, color_b, linear_clamp, float2(pa) + 0.5, vk, gain, 1.0) < layer_fit;
            fitb = BgCost(color_b, color_a, linear_clamp, float2(pb) + 0.5, -vk, 1.0, gain) < layer_fit;
        }
        const float4 rfa = RefAt(ref_fa, bc, y), rba = RefAt(ref_ba, bc, y), rfb = RefAt(ref_fb, bc, y), rbb = RefAt(ref_bb, bc, y);
        sa = State(klass, ina, fa, ca, rfa, rba, fita);
        sb = State(klass, inb, fb, cb, rfb, rbb, fitb);

    }
    mstate[y] = sa | (sb << 2);
}
#endif

#if defined(L_GROW)
Texture2D<float> lrec : register(t0);
Texture2D<uint> mstate_in : register(t1);
RWTexture2D<uint> mstate_out : register(u0);
groupshared uint g_s[34 * 34];

// 4-connected flood inside a 32x32 tile, layer_grow_iters steps; the tiles
// pass it on to each other from one dispatch to the next.
[numthreads(32, 32, 1)]
void MaskGrowCS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const int4 bp = BoxPixels(lrec);
    const int2 origin = bp.xy + int2(gid.xy) * 32;
    for (int k = int(gi); k < 34 * 34; k += 1024)
    {
        const int2 q = origin - 1 + int2(k % 34, k / 34);
        g_s[k] = InBox(bp, q) ? mstate_in.Load(int3(q, 0)) : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 q = origin + int2(tid.xy);
    const bool inb = InBox(bp, q);
    const uint me = (tid.y + 1) * 34 + tid.x + 1;
    [loop] for (int it = 0; it < layer_grow_iters; ++it)
    {
        uint s = g_s[me];
        const uint nb = g_s[me - 1] | g_s[me + 1] | g_s[me - 34] | g_s[me + 34];
        if ((s & 3u) == 1u && (nb & 2u) != 0u) s = (s & ~3u) | 2u;
        if (((s >> 2) & 3u) == 1u && (nb & 8u) != 0u) s = (s & ~12u) | 8u;
        GroupMemoryBarrierWithGroupSync();
        if (inb) g_s[me] = s;
        GroupMemoryBarrierWithGroupSync();
    }
    if (inb) mstate_out[q] = g_s[me];
}
#endif

#if defined(L_FINAL)
Texture2D<float> lrec : register(t0);
Texture2D<uint> mstate_in : register(t1);
RWTexture2D<uint> mstate_out : register(u0);  // bit 0 a's mask, 1 b's, 2 a's dilated by layer_sup, 3 b's,
                                              // 4 a alone and thick, 5 b alone and thick
RWByteAddressBuffer acc : register(u1);       // the vote counters after the component records
#define FT (32 + 2 * SUP_MAX)
groupshared uint g_s[FT * FT];
groupshared uint g_h[FT * 32];
groupshared uint g_vote[6];

uint InBits(int4 bp, int2 q)
{
    if (!InBox(bp, q)) return 0;
    const uint s = mstate_in.Load(int3(q, 0));
    return ((s & 3u) == 2u ? 1u : 0u) | (((s >> 2) & 3u) == 2u ? 2u : 0u);
}

[numthreads(32, 32, 1)]
void MaskFinalCS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const int4 bp = BoxPixels(lrec);
    const int2 origin = bp.xy + int2(gid.xy) * 32;
    if (gi < 6) g_vote[gi] = 0;
    for (int k = int(gi); k < FT * FT; k += 1024) g_s[k] = InBits(bp, origin - SUP_MAX + int2(k % FT, k / FT));
    GroupMemoryBarrierWithGroupSync();
    // layer_holes: a pixel a frame's mask encloses - within layer_holes px left, right, above and below
    // it - is that frame's too. The inside of a spike is often lighter than the hair's mean colour
    // (its sheen) and joined nothing, so the spike was drawn as its outline with the floor inside.
    // A gap between two spikes or beside the head is open on one side and stays out.
    const int hr = clamp(int(layer_holes), 0, SUP_MAX - 1);
    [branch] if (hr > 0)
    {
        for (int k = int(gi); k < FT * FT; k += 1024)
        {
            const int x = k % FT, y = k / FT;
            const uint own = g_s[k] & 3u;
            uint l = 0, r2 = 0, u = 0, d = 0;
            [loop] for (int j = 1; j <= hr; ++j)
            {
                if (x - j >= 0) l |= g_s[k - j] & 3u;
                if (x + j < FT) r2 |= g_s[k + j] & 3u;
                if (y - j >= 0) u |= g_s[k - j * FT] & 3u;
                if (y + j < FT) d |= g_s[k + j * FT] & 3u;
            }
            const uint enc = l & r2 & u & d & ~own & 3u;
            g_s[k] |= enc << 8;
        }
        GroupMemoryBarrierWithGroupSync();
        for (int k2 = int(gi); k2 < FT * FT; k2 += 1024)
        {
            const int2 qq = origin - SUP_MAX + int2(k2 % FT, k2 / FT);
            g_s[k2] = InBox(bp, qq) ? ((g_s[k2] | (g_s[k2] >> 8)) & 3u) : 0u;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    const int r = clamp(int(layer_sup), 0, SUP_MAX);
    const int ra = clamp(int(layer_attach), 0, SUP_MAX);
    const int rr = max(r, ra);
    for (int k2 = int(gi); k2 < FT * 32; k2 += 1024)
    {
        const int row = k2 / 32, col = k2 % 32 + SUP_MAX;
        uint m = 0;
        [loop] for (int dx = -rr; dx <= rr; ++dx)
        {
            const uint w = g_s[row * FT + col + dx] & 3u;
            m |= (abs(dx) <= r ? w : 0u) | (abs(dx) <= ra && w == 3u ? 4u : 0u);
        }
        g_h[k2] = m;
    }
    GroupMemoryBarrierWithGroupSync();
    uint m = 0;
    [loop] for (int dy = -rr; dy <= rr; ++dy)
    {
        const uint w = g_h[(int(tid.y) + SUP_MAX + dy) * 32 + int(tid.x)];
        m |= (abs(dy) <= r ? (w & 3u) : 0u) | (abs(dy) <= ra ? (w & 4u) : 0u);
    }
    // bit 2 of m: both frames' masks cover a pixel within layer_attach.
    const uint near_both = (m >> 2) & 1u;
    m &= 3u;
    const uint s = g_s[(tid.y + SUP_MAX) * FT + tid.x + SUP_MAX] & 3u;
    // Thick one-frame coverage: the opening (erode, then dilate, by layer_thin) of the
    // pixels only one frame's mask covers. A spike that deformed between the frames is
    // a thin sliver of it; a strip of background that one frame shows beside the layer
    // and the other has under it (it slid beneath the layer) is as wide as the motion.
    // Bits of g_s: 0-1 the masks, 2-3 eroded across, 4-5 eroded, 6-7 dilated across.
    const int e = clamp(int(layer_thin), 0, SUP_MAX / 2);
    uint thick = 0;
    [branch] if (e > 0)
    {
        GroupMemoryBarrierWithGroupSync();
        for (int k3 = int(gi); k3 < FT * FT; k3 += 1024)
        {
            const int x = k3 % FT, y = k3 / FT;
            uint a = 3u;
            if (x < e || x >= FT - e) a = 0;
            else
                [loop] for (int dx = -e; dx <= e; ++dx)
                {
                    const uint w = g_s[y * FT + x + dx] & 3u;
                    a &= (w == 1u ? 1u : 0u) | (w == 2u ? 2u : 0u);
                }
            g_s[k3] = (g_s[k3] & 3u) | (a << 2);
        }
        GroupMemoryBarrierWithGroupSync();
        for (int k4 = int(gi); k4 < FT * FT; k4 += 1024)
        {
            const int x = k4 % FT, y = k4 / FT;
            uint a = 3u;
            if (y < e || y >= FT - e) a = 0;
            else
                [loop] for (int dy2 = -e; dy2 <= e; ++dy2) a &= (g_s[(y + dy2) * FT + x] >> 2) & 3u;
            g_s[k4] = (g_s[k4] & 15u) | (a << 4);
        }
        GroupMemoryBarrierWithGroupSync();
        for (int k5 = int(gi); k5 < FT * FT; k5 += 1024)
        {
            const int x = k5 % FT, y = k5 / FT;
            uint o = 0;
            if (x >= e && x < FT - e)
                [loop] for (int dx2 = -e; dx2 <= e; ++dx2) o |= (g_s[y * FT + x + dx2] >> 4) & 3u;
            g_s[k5] = (g_s[k5] & 63u) | (o << 6);
        }
        GroupMemoryBarrierWithGroupSync();
        [loop] for (int dy3 = -e; dy3 <= e; ++dy3) thick |= (g_s[(int(tid.y) + SUP_MAX + dy3) * FT + int(tid.x) + SUP_MAX] >> 6) & 3u;
    }
    const int2 q = origin + int2(tid.xy);
    if (InBox(bp, q))
    {
        mstate_out[q] = s | (m << 2) | (thick << 4) | (near_both << 6);
        // The deletion vote. Background b shows where the layer is at q came,
        // under the dominant motion, from q + (vk - vdom) in a's layer
        // coordinates: if that is not the layer in a, the layer covered it -
        // the layer is in front. Expected share: the same test on b's own mask.
        const int2 rel = int2(round(float2(LR(lrec, LR_VK), LR(lrec, LR_VK + 1)) -
                                    float2(LR(lrec, LR_VBG), LR(lrec, LR_VBG + 1))));
        if (s & 2u)
        {
            const uint o = InBits(bp, q + rel);
            InterlockedAdd(g_vote[0], 1u);
            if ((o & 1u) == 0u) InterlockedAdd(g_vote[1], 1u);
            if ((o & 2u) == 0u) InterlockedAdd(g_vote[2], 1u);
        }
        if (s & 1u)
        {
            const uint o = InBits(bp, q - rel);
            InterlockedAdd(g_vote[3], 1u);
            if ((o & 2u) == 0u) InterlockedAdd(g_vote[4], 1u);
            if ((o & 1u) == 0u) InterlockedAdd(g_vote[5], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 6)
    {
        uint dummy;
        acc.InterlockedAdd((uint(grid_size.x * grid_size.y) * ACC_STRIDE + gi) * 4, g_vote[gi], dummy);
    }
}
#endif

#if defined(L_DONE)
ByteAddressBuffer acc : register(t0);
RWTexture2D<float> lrec : register(u0);

// The kill switch. Prior: a compact minority is in front. The vote overrules
// it only when it can say something (the expected share is not tiny).
[numthreads(1, 1, 1)]
void DoneCS()
{
    const uint base = uint(grid_size.x * grid_size.y) * ACC_STRIDE * 4;
    const float tb = float(acc.Load(base)), fb = float(acc.Load(base + 4)), eb = float(acc.Load(base + 8));
    const float ta = float(acc.Load(base + 12)), fa = float(acc.Load(base + 16)), ea = float(acc.Load(base + 20));
    const float tot = ta + tb;
    const float f = tot > 0 ? (fa + fb) / tot : 0;
    const float e = tot > 0 ? (ea + eb) / tot : 0;
    const bool alive = lrec[int2(LR_ALIVE, 0)] > 0.5 && (layer_kill <= 0.0 || (tot >= 30 && (e < layer_void || f >= layer_kill * e)));
    lrec[int2(LR_ALIVE, 0)] = alive ? 1.0 : 0.0;
    lrec[int2(LR_F, 0)] = f;
    lrec[int2(LR_E, 0)] = e;
    lrec[int2(LR_MASKPX, 0)] = tot;
    [unroll] for (int k = 0; k < 15; ++k) lrec[int2(26 + k, 0)] = float(acc.Load(base + 4 * k));
}
#endif

// ---- The frame in between ----------------------------------------------------------

#if defined(WARP)
Texture2D<float4> color_a : register(t0);
Texture2D<float4> color_b : register(t1);
Texture2D<float2> mid : register(t2);  // motion at the frame in between, level-0 pixels
Texture2D<float2> fwd : register(t3);  // a -> b by a's cells, level-0 pixels
Texture2D<float2> bwd : register(t4);  // b -> a by b's cells
Texture2D<float> stats : register(t5);
Texture2D<float> mid_cost : register(t6);  // MidCS's best cost per cell (luma per pixel)
Texture2D<float> lrec : register(t7);      // the layer's record (L_* passes)
Texture2D<uint> lmask : register(t8);      // L_FINAL: bit 0 a's mask, 1 b's, 2 a's dilated, 3 b's
Texture2D<float> lcand : register(t9);     // debug_view 7: minority cells' margin
Texture2D<uint> llabel : register(t10);    // ... their component
Texture2D<float4> lbgx : register(t11);    // L_BG: the background's motion
Texture2D<float4> lbgy : register(t12);
SamplerState linear_clamp : register(s0);

// The motion layer over this pixel (see "Motion layers"): how much of it the
// layer covers (0 to 1) and the colour it draws there. Where both frames'
// masks cover the pixel, a and b along the layer's motion (or nothing, with
// layer_both 1: WarpPS's own result); where one does, that frame alone -
// except within layer_sup of the other frame's mask, where only the frame
// nearer in time draws (one tip, not two). A pixel just outside a mask gets
// half its say (a 1 px feather).
float LayerCover(float2 x, int2 p, float to_mid_a, float to_mid_b, out float4 colour, out float3 dbg, out float both)
{
    colour = 0;
    dbg = 0;
    both = 0;
    if (layer_enable == 0 || lrec.Load(int3(LR_ALIVE, 0, 0)) < 0.5) return 0;
    const int4 bc = int4(lrec.Load(int3(LR_BOX, 0, 0)), lrec.Load(int3(LR_BOX + 1, 0, 0)), lrec.Load(int3(LR_BOX + 2, 0, 0)),
                         lrec.Load(int3(LR_BOX + 3, 0, 0)));
    const int4 bp = int4(bc.xy * LAYER_CELL, min((bc.zw + 1) * LAYER_CELL, src_size));
    if (any(p < bp.xy) || any(p >= bp.zw)) return 0;
    const uint s = lmask.Load(int3(p, 0));
    const int2 maxp = bp.zw - 1;
    const uint n4 = lmask.Load(int3(clamp(p + int2(1, 0), bp.xy, maxp), 0)) | lmask.Load(int3(clamp(p - int2(1, 0), bp.xy, maxp), 0)) |
                    lmask.Load(int3(clamp(p + int2(0, 1), bp.xy, maxp), 0)) | lmask.Load(int3(clamp(p - int2(0, 1), bp.xy, maxp), 0));
    float a = (s & 1u) ? 1.0 : ((n4 & 1u) ? 0.5 : 0.0);
    float b = (s & 2u) ? 1.0 : ((n4 & 2u) ? 0.5 : 0.0);
    // One frame alone, over a thick stretch: background that slid under the layer, not the layer.
    if (s & 16u) a = 0.0;
    if (s & 32u) b = 0.0;
    // One frame alone, away from the body of the layer (a fragment of a fast limb, a thread
    // along an edge of the scenery): not drawn. A spike is attached to the head.
    if (layer_attach > 0.0 && (s & 64u) == 0u && (s & 3u) != 3u)
    {
        a = 0.0;
        b = 0.0;
    }
    // One tip, not two: one-frame coverage near the other frame's mask is the
    // nearer frame's alone.
    // layer_sup >= 100: the other frame never draws alone - one real frame's shape.
    const bool never = layer_sup >= 100.0;
    if (t <= 0.5)
    {
        if (a == 0.0 && ((s & 4u) || never)) b = 0.0;
    }
    else
    {
        if (b == 0.0 && ((s & 8u) || never)) a = 0.0;
    }
    // Both frames' masks cover the pixel itself (not only its feather).
    both = (s & 3u) == 3u ? 1.0 : 0.0;
    dbg = float3(a, b, 0);
    if (a == 0.0 && b == 0.0) return 0;
    const float2 vk = float2(lrec.Load(int3(LR_VK, 0, 0)), lrec.Load(int3(LR_VK + 1, 0, 0)));
    const float4 ca = color_a.SampleLevel(linear_clamp, (x - t * vk) * inv_size, 0);
    const float4 cb = color_b.SampleLevel(linear_clamp, (x + (1.0 - t) * vk) * inv_size, 0);
    if (a > 0.0 && b > 0.0)
    {
        // layer_both 2: the nearer frame alone here too (no doubled detail where the
        // layer deforms); 1: WarpPS's own result inside, the layer only on its rim.
        colour = layer_both == 2 ? (t <= 0.5 ? float4(ca.rgb * to_mid_a, ca.a) : float4(cb.rgb * to_mid_b, cb.a)) : lerp(ca, cb, t);
        if (layer_both == 1 && (s & 3u) == 3u) return 0;
        return max(a, b);
    }
    if (a > 0.0)
    {
        colour = float4(ca.rgb * to_mid_a, ca.a);
        return a;
    }
    colour = float4(cb.rgb * to_mid_b, cb.a);
    return b;
}

// layer_ghost: a frame's pixel at q (that frame's coordinates) is the layer's - its mask, or a
// pixel beside it (an anti-aliased edge), not a thick one-frame stretch (background that slid
// under it). off: OffsetA or OffsetB of the layer's motion; bit 0 for a, 1 for b.
bool OnLayer(float2 q, int2 off, uint bit, int4 bp)
{
    const int2 y = int2(floor(q)) - off;
    if (any(y < bp.xy - 1) || any(y > bp.zw)) return false;
    const uint mine = 1u << bit, slid = 16u << bit;
    bool on = false;
    [unroll] for (int k = 0; k < 5; ++k)
    {
        const int2 z = clamp(y + int2(k == 1 ? 1 : (k == 2 ? -1 : 0), k == 3 ? 1 : (k == 4 ? -1 : 0)), bp.xy, bp.zw - 1);
        const uint s = lmask.Load(int3(z, 0));
        on = on || ((s & mine) != 0u && (s & slid) == 0u);
    }
    return on;
}

// How far v is from the motion a frame's own blocks found near a full-
// resolution position (in full-resolution pixels): the distance to each of the
// four nearest blocks' motion, mixed by how near their centres are. Taking the
// block under the position alone makes the distance jump at every block edge,
// and where drawing from one frame alone wins (effects, a fast limb) the frame
// in between shows those edges as steps. Mixing the distances rather than the
// motions keeps an object's edge: on each side its own motion still matches.
float FieldMismatch(Texture2D<float2> field, float2 p, float2 v)
{
    const float2 q = p / (2.0 * BLOCK) - 0.5;
    const int2 c0 = int2(floor(q));
    const float2 f = q - float2(c0);
    float m = 0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const float2 w = lerp(1.0 - f, f, float2(o));
        const float2 u = field.Load(int3(clamp(c0 + o, 0, pred_size - 1), 0)) * 2.0;
        m += w.x * w.y * length(u - v);
    }
    return m;
}

bool Inside(float2 p) { return all(p >= 0.0) && all(p < float2(src_size)); }

#define MAX_CANDIDATES 10  // the 3x3 cells and none

// Drawing a pixel from one frame alone along v is only right if that frame's
// pixel moves with v. The motion found for its cell says so for the cell, but
// a cell on the edge of a still character takes the scenery's motion, and
// then a piece of the character - drawn from that frame alone, where scenery
// belongs - shows as a ghost of its outline beside it. So the pixel itself is
// asked too: if it is the same in the other frame at the same place, and not
// where v would have taken it, it stands still and does not move with v
// (1 is added to the option's cost, which rules it out). A camera turning
// around a character that stays put on screen is the common case.
float StandsStill(Texture2D<float4> own, Texture2D<float4> other, float2 pos, float2 v, float gain_to_other)
{
    if (dot(v, v) < 4.0) return 0;  // v itself is (nearly) still
    const float3 c = gain_to_other * own.SampleLevel(linear_clamp, pos * inv_size, 0).rgb;
    const float3 here = abs(c - other.SampleLevel(linear_clamp, pos * inv_size, 0).rgb);
    const float3 moved = abs(c - other.SampleLevel(linear_clamp, (pos + v) * inv_size, 0).rgb);
    const float e_here = here.r + here.g + here.b, e_moved = moved.r + moved.g + moved.b;
    // Clearly not moving with v (in a flat area both differences are small
    // and say nothing), and clearly better at rest.
    return (e_here < still_check && e_moved > 0.15 && e_here < 0.5 * e_moved) ? 1.0 : 0.0;
}

// How surely a sample at pos lies on (or within a margin of) a block of its
// frame that moves much slower than v (0 to 1): a character the camera turns
// around, with the scenery sweeping past behind it. Beside such a character
// lies a strip, half the scenery's step wide, that only one frame shows -
// the frame the scenery comes from on one side, the one it goes to on the
// other - and it is drawn from that frame alone. The other frame's sample
// there lands on the character: on its own blocks, or on a spike of its hair
// poking out of them that its blocks (mostly scenery, moving with it) do not
// show. Its cost is no higher, and what the frame in between shows there is
// a faint copy of the character's edge - three tips where there are two -
// half the scenery's step beside it, or, mixed the other way, scenery cut
// into the head. The right frame's sample lies at least half the step from
// the character, so the margin (spikes poke out of the character's blocks
// by up to two blocks) stays short of that.
#define SLOW_GROW 32.0
#define SLOW_SHARE 0.6  // WarpPS: ... where this share of the cells around moves with the scenery
float NearSlowBlock(Texture2D<float2> field, float2 pos, float2 v)
{
    const float slow = 0.25 * length(v);
    const float grow = min(SLOW_GROW, 0.5 * length(v) - 8.0);
    if (grow <= 0.0) return 0;
    const int2 c0 = int2(floor(pos / (2.0 * BLOCK)));
    float near = 0;
    [loop] for (int k = 0; k < 25; ++k)
    {
        const int2 c = clamp(c0 + int2(k % 5 - 2, k / 5 - 2), 0, pred_size - 1);
        const float2 lo = float2(c) * 2.0 * BLOCK, hi = lo + 2.0 * BLOCK;
        const float d = length(max(max(lo - pos, pos - hi), 0.0));  // to the block, 0 inside
        if (length(field.Load(int3(c, 0)) * 2.0) < slow) near = max(near, saturate((grow + 8.0 - d) / 8.0));
    }
    return near;
}

// Candidate i of a pixel: the motion of one of the cells around it, or none
// for i == cells, the tie-break it pays, and its say in the mix (reach). A
// cell off the edge of the grid is no candidate (outside): copying the edge
// cell's motion there would only count that cell twice.
//
// The reach falls off with distance - a tent, 1 within half a cell of the
// cell's centre down to 0 where the cell leaves the pixel's set (1.5 cells
// away in the 3x3, 1 in the 2x2). The set moves on by a whole column or row
// of cells as the pixel crosses a cell edge; at full say, the cells dropping
// out and coming in would change the mix at once there, and where the cells'
// motions differ much (effects, fast limbs) the frame in between would show
// the 16-pixel grid. Fading them in and out keeps it continuous.
void Candidate(int i, int cells, int ring, int2 first, float2 g, float none_tie, out float2 v, out float tie,
               out bool outside, out float reach)
{
    v = 0;
    tie = none_tie;
    outside = false;
    reach = 1.0;
    if (i < cells)
    {
        const int2 c = first + int2(i % ring, i / ring);
        outside = any(c < 0) || any(c >= grid_size);
        v = mid.Load(int3(clamp(c, 0, grid_size - 1), 0)) * 2.0;
        // Nearer cells win ties: nothing for the cell's own centre, the full
        // 0.02 a cell and a half away.
        const float2 d = abs(g - float2(c));
        tie = 0.02 * saturate(length(d) / 1.5);
        const float edge = ring == 3 ? 1.5 : 1.0;
        reach = saturate((edge - d.x) / (edge - reach_plateau)) * saturate((edge - d.y) / (edge - reach_plateau));
        outside = outside || reach <= 0.0;
    }
}

// A full-screen triangle from the vertex id alone (blend.hlsl's VSMain).
//
// Each pixel tries the motions of the cells around it (warp_ring: the 2x2 it
// sits between, or the 3x3 around its own cell) and none. A motion is judged
// by how well a and b agree where it takes them, on the pixel itself or on a
// small cross of five (warp_taps) so grain does not decide. Judging per pixel
// keeps the edge of a moving object on the object instead of smearing the
// cell's motion across it.
//
// Where only one frame shows what belongs here - background a moving object
// uncovers or covers, the edge of the screen in a pan - mixing is wrong
// whatever the motion. So each motion can also be drawn from one frame alone:
// it costs a fixed occlusion_cost (above what two frames that agree cost), plus
// how far that frame's own motion there is from it, and a frame whose sample
// would come from off screen cannot be used at all.
//
// The options are mixed by cost, each weighted exp(-(cost - best) / softness),
// rather than the cheapest taken alone. Where nothing fits - UI fading or
// pulsing over moving scenery, smoke, glass - taking the cheapest per pixel
// switches between motions from cell to cell, and the frame in between shows
// blocks; mixing them gives a soft blend instead. But where one option fits
// almost perfectly - a character the camera turns around, the scenery
// sweeping past behind it - a soft mix would let the others in too (the
// scenery drawn from one frame, at its fixed cost, faintly over the
// character's edge: a ghost). So the softness follows how well the best
// option fits: soft_scale times its cost, from soft_floor for a perfect fit up
// to soft_temp. That needs the best cost first, so the costs are taken in a
// first pass and the colours mixed in a second.
//
// Where no option fits at all - lightning, sparks, an attack faster than the
// search can follow - even soft_temp leaves one wrong motion or another
// winning from patch to patch, and since the motions on offer change from
// cell to cell, the patches line up with the cells: steps along the 16-pixel
// grid. There the softness may go up to soft_wide, and the mix becomes a
// smear, like motion blur, instead. But a pixel only one frame shows (the edge
// of the screen in a pan, background beside a moving character) also fits
// nothing in both frames, and it has an answer - that frame alone, along a
// motion its own blocks agree with (a one-frame option costing about
// occlusion_cost); widening there would blur the edge and bring the ghosts
// back. So the wide softness is only for pixels without such an answer.
//
// "None" pays the largest tie-break, so a flat area does not lose its motion
// to it - except where a and b are the same all around the pixel (a HUD,
// subtitles, a logo): there it wins outright, so a thin stroke is not swapped
// for whatever two moving samples happen to agree.
float4 WarpPS(float4 pos : SV_Position) : SV_Target
{
    const float2 x = pos.xy;  // the pixel's centre, full resolution
    const int2 p = int2(x);
    const int2 maxp = src_size - 1;
    const float gain = GainFrom(stats);

    // A cut: most of the frame matched nothing. Show a again.
    if (stats.Load(int3(STATS_BAD, 0, 0)) > cut_fraction || stats.Load(int3(STATS_BAD_DETAILED, 0, 0)) > cut_detailed)
    {
        float4 c = color_a.Load(int3(p, 0));
        if (marker > 0 && p.x < 32 && p.y < 32) c = float4(marker_level, 0, marker_level, 1);
        return c;
    }

    float still = 0;  // the largest difference between a and b around the pixel
    [unroll] for (int k = 0; k < 9; ++k)
    {
        const int3 q = int3(clamp(p + int2(k % 3 - 1, k / 3 - 1), 0, maxp), 0);
        const float3 d = abs(gain * color_a.Load(q).rgb - color_b.Load(q).rgb);
        still = max(still, d.r + d.g + d.b);
    }
    // The same all around in both frames (black bars beside a 16:9 picture, a
    // still HUD, a paused scene): nothing to search, draw it as it stands.
    if (still < static_eps && debug_view == 0)
    {
        float4 c = lerp(color_a.Load(int3(p, 0)), color_b.Load(int3(p, 0)), t);
        if (marker > 0 && p.x < 32 && p.y < 32) c = float4(marker_level, 0, marker_level, 1);
        return c;
    }
    const float none_tie = still < static_eps ? -0.02 : 0.02;

    // The pixel in cell units: cell c's centre is at c.
    const float2 g = x / (2.0 * MID_BLOCK) - 0.5;
    const int ring = warp_ring == 3 ? 3 : 2;
    const int2 first = ring == 3 ? int2(round(g)) - 1 : int2(floor(g));
    const int cells = ring * ring;
    // A sample drawn from one frame alone is brought to the brightness of the
    // moment in between.
    const float to_mid_a = lerp(1.0, gain, t), to_mid_b = to_mid_a / gain;
    // The motion layer drawn over this pixel (LayerCover); fully covered, it is the answer.
    float4 layer_colour;
    float3 layer_dbg;
    float layer_both_px;
    const float layer_w = LayerCover(x, p, to_mid_a, to_mid_b, layer_colour, layer_dbg, layer_both_px);
    if (layer_w >= 1.0 && debug_view == 0 && layer_gate == 0)
    {
        if (marker > 0 && p.x < 32 && p.y < 32) layer_colour = float4(marker_level, 0, marker_level, 1);
        return layer_colour;
    }
    const float2 taps[5] = {float2(0, 0), float2(1.5, 0), float2(-1.5, 0), float2(0, 1.5), float2(0, -1.5)};
    const int ntaps = warp_taps >= 5 ? 5 : 1;
    // What a and b show at the pixel - on the cross of taps the options are
    // judged on, averaged, so a speck of texture does not decide - and how
    // alike they are there (see persist).
    float3 here_a = 0, here_b = 0;
    if (layer_cost > 0.0)
    {
        [loop] for (int hk = 0; hk < ntaps; ++hk)
        {
            here_a += gain * color_a.SampleLevel(linear_clamp, (x + taps[hk]) * inv_size, 0).rgb;
            here_b += color_b.SampleLevel(linear_clamp, (x + taps[hk]) * inv_size, 0).rgb;
        }
    }
    here_a /= float(ntaps);
    here_b /= float(ntaps);
    const float3 here_d = abs(here_a - here_b);
    const float persist_here = layer_cost * saturate((persist_max - (here_d.r + here_d.g + here_d.b)) / persist_max);
    float near_still = 0;  // a cell around stands still (as near as its reach), see persist
    // layer_ghost: the layer's motion and box (full-res pixels).
    const bool ghost_on = layer_ghost > 0.0 && layer_enable != 0 && lrec.Load(int3(LR_ALIVE, 0, 0)) > 0.5;
    const float2 ghost_vk = ghost_on ? float2(lrec.Load(int3(LR_VK, 0, 0)), lrec.Load(int3(LR_VK + 1, 0, 0))) : float2(0, 0);
    int4 ghost_bp = int4(0, 0, 0, 0);
    bool ghost_here = false;
    [branch] if (ghost_on)
    {
        const int4 gbc = int4(lrec.Load(int3(LR_BOX, 0, 0)), lrec.Load(int3(LR_BOX + 1, 0, 0)), lrec.Load(int3(LR_BOX + 2, 0, 0)),
                              lrec.Load(int3(LR_BOX + 3, 0, 0)));
        ghost_bp = int4(gbc.xy * LAYER_CELL, min((gbc.zw + 1) * LAYER_CELL, src_size));
        // Only where the pixel itself is background: no mask (either frame's, moved to the frame in
        // between) within 3 px. On the layer the character's own parts move as they like (hair
        // turning, a face) and sample the layer in both frames as they should.
        ghost_here = true;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const int2 z = p + 3 * int2(k % 3 - 1, k / 3 - 1);
            if (all(z >= ghost_bp.xy) && all(z < ghost_bp.zw) && (lmask.Load(int3(z, 0)) & 3u) != 0u) ghost_here = false;
        }
    }

    // First pass: what every option costs - both frames along each motion, a
    // alone, b alone (1e30 where it cannot be used) - and the lowest. A motion
    // already tried (cells moving together, as in a pan, mostly share theirs)
    // is not sampled again; it is counted instead, so in the mix a motion many
    // cells around agree on weighs as many times (by their reach) - the
    // consensus outweighs one odd cell or option - and it pays the smallest
    // tie-break of the cells that share it (the costs are kept without it until
    // the end).
    float cost2[MAX_CANDIDATES], costa[MAX_CANDIDATES], costb[MAX_CANDIDATES], votes[MAX_CANDIDATES];
    float ties[MAX_CANDIDATES];
    float layer[MAX_CANDIDATES];  // how much each option moves and changes the pixel (0 to 1), see persist
    float one_ok[MAX_CANDIDATES];  // the one-frame options, as evidence the pixel is explained
    float2 tried[MAX_CANDIDATES];
    [unroll] for (int i = 0; i < MAX_CANDIDATES; ++i)
    {
        cost2[i] = 1e30;
        costa[i] = 1e30;
        costb[i] = 1e30;
        ties[i] = 0;
        layer[i] = 0;
        one_ok[i] = 1e30;
        tried[i] = float2(1e9, 1e9);
        votes[i] = 0;
        if (i <= cells)
        {
            float2 v;
            float tie;
            bool outside;
            float reach;
            Candidate(i, cells, ring, first, g, none_tie, v, tie, outside, reach);
            if (outside) continue;
            if (i < cells) near_still = max(near_still, reach * saturate(3.0 - length(v)));
            bool again = false;
            [unroll] for (int k = 0; k < i; ++k)
            {
                if (!again && all(abs(v - tried[k]) < 0.25))
                {
                    votes[k] += reach;
                    ties[k] = min(ties[k], tie);
                    again = true;
                }
            }
            if (again) continue;
            tried[i] = v;
            votes[i] = reach;
            ties[i] = tie;
            const float2 pa = x - t * v, pb = x + (1.0 - t) * v;
            const bool in_a = Inside(pa), in_b = Inside(pb);
            if (in_a && in_b)
            {
                float3 d = 0, mean_a = 0, mean_b = 0;
                [loop] for (int k = 0; k < ntaps; ++k)
                {
                    const float3 sa = gain * color_a.SampleLevel(linear_clamp, (pa + taps[k]) * inv_size, 0).rgb;
                    const float3 sb = color_b.SampleLevel(linear_clamp, (pb + taps[k]) * inv_size, 0).rgb;
                    d += abs(sa - sb);
                    mean_a += sa;
                    mean_b += sb;
                }
                cost2[i] = (d.r + d.g + d.b) / float(ntaps);
                // How far what this option brings is from what both frames
                // show here (on the same cross, averaged), see persist.
                const float3 e = 0.5 * (abs(mean_a / float(ntaps) - here_a) + abs(mean_b / float(ntaps) - here_b));
                layer[i] = saturate((length(v) - 2.0) / 4.0) * saturate((e.r + e.g + e.b) / 0.1);
            }
            if (in_a)
            {
                costa[i] = occlusion_cost + flow_weight * FieldMismatch(fwd, pa, v);
                if (costa[i] < occlusion_cost + 0.05) costa[i] += StandsStill(color_a, color_b, pa, v, gain);
                // Where b's sample would be off screen, a alone is the answer
                // whatever a's blocks say: those along the edge of the screen,
                // half their pixels off it, often find no proper motion.
                one_ok[i] = in_b ? costa[i] : occlusion_cost;
            }
            if (in_b)
            {
                costb[i] = occlusion_cost + flow_weight * FieldMismatch(bwd, pb, -v);
                if (costb[i] < occlusion_cost + 0.05) costb[i] += StandsStill(color_b, color_a, pb, -v, 1.0 / gain);
                one_ok[i] = min(one_ok[i], in_a ? costb[i] : occlusion_cost);
            }
            // layer_ghost: where a frame shows the layer, it does not show what moves otherwise
            // (the scenery behind a character the camera turns around): an option that does not
            // move with the layer and samples the layer in a frame is wrong in that frame - drawn
            // from it, the head shows again half a scenery-step beside itself.
            [branch] if (ghost_here && length(v - ghost_vk) > layer_vdiff)
            {
                const bool on_a = OnLayer(pa, OffsetA(ghost_vk), 0u, ghost_bp), on_b = OnLayer(pb, OffsetB(ghost_vk), 1u, ghost_bp);
                if (on_a) costa[i] += layer_ghost;
                if (on_b) costb[i] += layer_ghost;
                if (on_a || on_b) cost2[i] += layer_ghost;
            }
        }
    }
    // A one-frame option that draws the scenery beside something slow from
    // the wrong frame pays slow_cost, as surely as its sample lies on the slow
    // thing (NearSlowBlock). Only for a motion that is the scenery's here
    // (most cells around move with it): a spear swung across still scenery,
    // or a character running past, has the scenery's still blocks all around
    // it too, and must still be drawn from one frame where that is all there is.
    // Asked only where drawing from one frame is in reach at all (nothing from
    // both frames fits much better), as in a pan it is not.
    float best2 = 1e30;
    [unroll] for (int n0 = 0; n0 < MAX_CANDIDATES; ++n0) best2 = min(best2, cost2[n0]);
    const float reach1 = best2 + 0.05;
    if (slow_cost > 0.0)
    {
        float all_votes = 0;
        [unroll] for (int n1 = 0; n1 < MAX_CANDIDATES; ++n1) all_votes += votes[n1];
        [unroll] for (int n = 0; n < MAX_CANDIDATES; ++n)
        {
            const float2 v = tried[n];
            [branch] if (min(costa[n], costb[n]) < reach1 && dot(v, v) >= 256.0)
            {
                // How many of the cells around move with v (the scenery's, if most do).
                float with_v = 0;
                [unroll] for (int m = 0; m < MAX_CANDIDATES; ++m)
                    with_v += length(tried[m] - v) < 0.25 * length(v) ? votes[m] : 0.0;
                // Only one way round: one frame's sample on the slow thing, the
                // other's clear of it and in reach - then that one is the
                // answer. Where both or neither are (smoke, an effect), this
                // says nothing about which.
                [branch] if (with_v >= SLOW_SHARE * all_votes && max(costa[n], costb[n]) < reach1)
                {
                    const float na = NearSlowBlock(fwd, x - t * v, v), nb = NearSlowBlock(bwd, x + (1.0 - t) * v, -v);
                    costa[n] += slow_cost * na * (1.0 - nb);
                    costb[n] += slow_cost * nb * (1.0 - na);
                }
            }
        }
    }

    // The softness follows the best cost, and how far it may go whether a one-
    // frame option explains the pixel. An option whose cells are fading out
    // (reach, see Candidate) counts for both less and less: it gains up to a
    // margin past which it changes neither. Otherwise it would set them at full
    // strength right up to where its cells leave the set, and the mix change at
    // once there - the cell grid again.
    // What a pixel shows in both frames it shows in between: where a and b
    // show the same there (averaged over the cross of taps, so one speck of
    // texture does not decide; persist_here, up to layer_cost), an option
    // from both frames that moves and would bring something else (layer)
    // pays for it. The top of a head the camera turns around shows the same
    // hair in both frames, but its spikes turn a little, and the scenery's
    // motion carries the plain ground beside the head onto it better than
    // the turning hair fits itself: without this the frame in between shows
    // the ground through the top of the head. It holds only near a cell that
    // stands still (near_still - the character the camera follows or turns
    // around, a HUD): in a pan, a detail narrower than the pan's step passes
    // pixels that show plain background in both frames, and must not be
    // lost. (Near something still, a thing moving further than its own
    // width - a spear swung across still scenery - is in neither frame at
    // the pixels it crosses in between, and loses them to the scenery.)
    const float persist = persist_here * near_still;

    const float wide = max(soft_wide, soft_temp);
    const float margin = wide / max(soft_scale, 1e-3);
    float lowest = 1e30;  // (1e30 plus a tie is still 1e30 in single precision)
    float basis = 1e30;   // ... with the margin: what the softness follows
    float lowest_one = 1e30;  // ... of the options drawing from one frame alone
    float lowest_both = 1e30;  // ... of the options drawing from both frames (cap_fit)
    [unroll] for (int n = 0; n < MAX_CANDIDATES; ++n)
    {
        cost2[n] += ties[n];
        costa[n] += ties[n];
        costb[n] += ties[n];
        const float fade = 1.0 - saturate(votes[n]);
        const float one = min(costa[n], costb[n]), best = min(cost2[n], one);
        basis = min(basis, best + fade * margin);
        lowest_one = min(lowest_one, one_ok[n] + ties[n] + fade * 0.05);
        lowest_both = min(lowest_both, cost2[n] + fade * 0.05);
        // The persistence (above), on drawing from both frames only: the
        // scenery beside a character that only one frame shows is drawn from
        // that frame as before, and the softness and what explains the pixel
        // do not change with it.
        cost2[n] += persist * layer[n];
        lowest = min(lowest, min(cost2[n], one));
    }
    // Explained: a one-frame option within 0.02 of occlusion_cost (its frame's
    // own motion agrees, or the other frame's sample is off screen), not at all
    // from 0.05 above.
    float explained = saturate((occlusion_cost + 0.05 - lowest_one) / 0.03);
    // cap_fit (from softcap2): a both-frames option that already fits explains the pixel too.
    // The spikes of a head the camera orbits fit the cell's still motion under 0.08 but no
    // one-frame option, and widening the softness there lets the fast scenery beside it in as a
    // faint second outline. Lightning and sparks have no both-frames option that fits and stay
    // wide; cap_cell also asks that the cell's own motion fits (a wrong motion matching a spark).
    [branch] if (cap_fit > 0.0)
    {
        float fits = saturate((cap_fit + 0.02 - lowest_both) / 0.02);
        if (cap_cell > 0.0 && fits > 0.0)
        {
            const float cc = mid_cost.SampleLevel(linear_clamp, x / (2.0 * MID_BLOCK) / float2(grid_size), 0);
            fits *= saturate((cap_cell + 0.01 - cc) / 0.01);
        }
        explained = max(explained, fits);
    }

    // Second pass: the colours, mixed.
    const float soft = soft_temp <= 0.0 ? 1e-6 : clamp(soft_scale * basis, soft_floor, lerp(wide, soft_temp, explained));
    float4 acc = 0;
    float sum = 0;
    float3 debug = 0;  // weight of: both frames, a alone, b alone
    float debug_motion = 0;
    float bg_weight = 0;  // ... of options moving with the background (the layer's gate)
    const float2 vbg = layer_w > 0.0 ? BgAt(lbgx, lbgy, clamp(p / (2 * MID_BLOCK), 0, grid_size - 1)) * 2.0 : float2(1e9, 1e9);
    const float2 vlay = layer_w > 0.0 ? float2(lrec.Load(int3(LR_VK, 0, 0)), lrec.Load(int3(LR_VK + 1, 0, 0))) : float2(1e9, 1e9);
    // layer_gate 3, 4: where both frames' masks cover the pixel, the layer is there in both
    // frames - a one-frame option there is not a strip sliding under it, so it is replaced too
    // (3: every option with the background's motion; 4: every option not moving with the layer).
    // layer_gate 6: as 3, but only on the thin parts of the both-frames cover (spikes); on a skull
    // or a limb WarpPS's own one-frame choices (its occlusion edges) stand, as with 2.
    // layer_gate 7: as 3 where the layer stands nearly still on the screen (within LAYER_STILL px:
    // the camera turns around a standing character, which one translation fits well and whose
    // both-frames cover can be trusted); as 2 for a layer that moves (a running character, a limb).
    const bool still_layer = length(vlay) <= LAYER_STILL;
    const int gate_eff = layer_gate == 7 ? (still_layer ? 3 : 2) : layer_gate;
    const int gate_px = gate_eff >= 3 ? (layer_both_px > 0.0 ? gate_eff : 2) : gate_eff;
    float top_w = 0, top_mode = 0;  // debug_view 10: the strongest option
    float2 top_v = 0;
    [unroll] for (int j = 0; j < MAX_CANDIDATES; ++j)
    {
        if (j <= cells)
        {
            const float w2 = votes[j] * exp(-(cost2[j] - lowest) / soft);
            const float wa = votes[j] * exp(-(costa[j] - lowest) / soft);
            const float wb = votes[j] * exp(-(costb[j] - lowest) / soft);
            if (w2 + wa + wb > 1e-4)
            {
                float2 v;
                float tie;
                bool outside;
                float reach;
                Candidate(j, cells, ring, first, g, none_tie, v, tie, outside, reach);
                const float4 ca = color_a.SampleLevel(linear_clamp, (x - t * v) * inv_size, 0);
                const float4 cb = color_b.SampleLevel(linear_clamp, (x + (1.0 - t) * v) * inv_size, 0);
                acc += w2 * lerp(ca, cb, t) + wa * float4(ca.rgb * to_mid_a, ca.a) + wb * float4(cb.rgb * to_mid_b, cb.a);
                sum += w2 + wa + wb;
                debug += float3(w2, wa, wb);
                debug_motion += (w2 + wa + wb) * length(v);
                if (debug_view == 10)
                {
                    if (w2 > top_w) top_w = w2, top_v = v, top_mode = 0.0;
                    if (wa > top_w) top_w = wa, top_v = v, top_mode = 0.5;
                    if (wb > top_w) top_w = wb, top_v = v, top_mode = 1.0;
                }
                // layer_gate 2: only both-frames options (the floor matching itself across a and b
                // over a deforming spike); a one-frame option is a strip one frame cannot show
                // (background sliding under the layer), which the layer must not overwrite.
                // Moving with the background: within layer_vdiff of its local model, or within
                // layer_bgnear of how far the background moves past the layer if that is more. In a
                // fast turn the floor beside a head moves 92-128 px per frame across 300 px of
                // perspective, the model is 10-15 px off, and the floor carried onto the top of the
                // head stayed. (Anything nearer the background than the layer took a swinging arm too.)
                const bool with_bg = length(v - vbg) <= max(layer_vdiff, layer_bgnear * length(vbg - vlay));
                if (gate_px == 5) bg_weight += w2 + wa + wb;
                else if (gate_px == 4) bg_weight += length(v - vlay) > layer_vdiff ? w2 + wa + wb : 0.0;
                else if (gate_px == 3) bg_weight += with_bg ? w2 + wa + wb : 0.0;
                else bg_weight += with_bg ? (gate_px == 2 ? w2 : w2 + wa + wb) : 0.0;
            }
        }
    }
    float4 result = sum > 0 ? acc / sum : lerp(color_a.Load(int3(p, 0)), color_b.Load(int3(p, 0)), t);
    if (debug_view == 1 && sum > 0)
    {
        // Blue: both frames, red: a alone, green: b alone; brighter for more motion.
        const float3 share = debug / sum;
        result = float4(share.gbr * (0.35 + 0.65 * saturate(debug_motion / sum / 80.0)), 1);
    }
    if (debug_view == 10 && sum > 0)
    {
        // The strongest option: red/green its motion (0.5 + v / 512), blue 0 both frames, 0.5 a alone, 1 b alone
        // (evaluator, read the PNG's 16-bit values).
        result = float4(saturate(0.5 + top_v / 512.0), top_mode, 1);
    }
    if (debug_view == 3)
    {
        // Grey: how badly the cells' own motion matched (white = 0.1 and up).
        const float cc = mid_cost.SampleLevel(linear_clamp, x / (2.0 * MID_BLOCK) / float2(grid_size), 0);
        result = float4(saturate(cc / 0.1).xxx, 1);
    }
    if (debug_view == 2)
    {
        // Grey: the cost the softness follows (white = 0.4 and up); red: may
        // be mixed widely (no one-frame option explains the pixel).
        const float widened = soft_wide > soft_temp ? 1.0 - explained : 0.0;
        result = float4(lerp(saturate(basis / 0.4).xxx, float3(1, 0, 0), 0.5 * widened), 1);
    }
    if (debug_view == 5)
    {
        // Costs, x2 (white = 0.5 and up): red no motion from both frames,
        // green the best other motion from both frames, blue the best option
        // from one frame alone.
        float still2 = 1e30, other = 1e30, one = 1e30;
        [unroll] for (int m = 0; m < MAX_CANDIDATES; ++m)
        {
            if (length(tried[m]) < 2.0) still2 = min(still2, cost2[m]);
            else other = min(other, cost2[m]);
            one = min(one, min(costa[m], costb[m]));
        }
        result = float4(saturate(float3(still2, other, one) * 2.0), 1);
    }
    if (debug_view == 4)
    {
        // The motion found there, its length (white = 128 pixels and up): red
        // a's blocks, green b's, blue the frame in between's cells.
        const int2 c = clamp(int2(x / (2.0 * BLOCK)), 0, pred_size - 1);
        const float2 m = mid.Load(int3(clamp(int2(x / (2.0 * MID_BLOCK)), 0, grid_size - 1), 0));
        result = float4(saturate(float3(length(fwd.Load(int3(c, 0))), length(bwd.Load(int3(c, 0))), length(m)) / 64.0), 1);
    }
    // The layer replaces the frame where it covers it - with layer_gate, only as far as the frame
    // drew the background's motion there (the floor carried onto a spike of hair), not where it
    // already drew the character with a motion of its own (the coat, the skull).
    const float layer_say = layer_w * (layer_gate != 0 ? (sum > 0 ? bg_weight / sum : 1.0) : 1.0);
    if (layer_w > 0.0 && debug_view == 0) result = lerp(result, layer_colour, layer_say);
    if (debug_view == 9)
    {
        // The layer's say: red what it replaced, green its cover, blue both masks; over grey.
        const float l = 0.3 * Luma(lerp(color_a.Load(int3(p, 0)).rgb, color_b.Load(int3(p, 0)).rgb, t));
        result = float4(l + 0.7 * layer_say, l + 0.5 * layer_w, l + 0.5 * layer_both_px, 1);
    }
    if (debug_view == 6)
    {
        // The layer: grey picture, red a's mask, green b's (yellow both), as
        // they are drawn after the one-tip rule; blue tint: inside the layer's box.
        const float l = 0.4 * Luma(lerp(color_a.Load(int3(p, 0)).rgb, color_b.Load(int3(p, 0)).rgb, t));
        float inbox = 0;
        if (layer_enable != 0 && lrec.Load(int3(LR_ALIVE, 0, 0)) > 0.5)
        {
            const int4 bc = int4(lrec.Load(int3(LR_BOX, 0, 0)), lrec.Load(int3(LR_BOX + 1, 0, 0)), lrec.Load(int3(LR_BOX + 2, 0, 0)),
                                 lrec.Load(int3(LR_BOX + 3, 0, 0)));
            inbox = all(p >= bc.xy * LAYER_CELL) && all(p < (bc.zw + 1) * LAYER_CELL) ? 1.0 : 0.0;
        }
        result = float4(l + 0.6 * layer_dbg.x, l + 0.6 * layer_dbg.y, l + 0.15 * inbox, 1);
    }
    if (debug_view == 8)
    {
        // The background's motion (L_BG): red x, green y (0.5 = none, +-64 px full scale), blue: its cells fitted.
        const int2 c = clamp(p / (2 * MID_BLOCK), 0, grid_size - 1);
        const float2 vb = BgAt(lbgx, lbgy, c) * 2.0;
        result = float4(saturate(0.5 + vb / 128.0), saturate(lbgx.Load(int3(c / 8, 0)).w / 1600.0), 1);
    }
    if (debug_view == 7)
    {
        // Minority cells (red, brighter for a larger margin), the chosen layer's cells (green).
        const int2 c = clamp(p / LAYER_CELL, 0, grid_size - 1);
        const float m = lcand.Load(int3(c, 0));
        const uint lb = llabel.Load(int3(c, 0));
        const float root = lrec.Load(int3(LR_ROOT, 0, 0));
        bool chosen = false;
        if (lb != NO_LABEL && root >= 0)
        {
            uint l = lb;
            [unroll] for (int k = 0; k < 3; ++k) l = min(l, llabel.Load(int3(int(l % uint(grid_size.x)), int(l / uint(grid_size.x)), 0)));
            chosen = l == uint(root);
        }
        const float l0 = 0.35 * Luma(color_a.Load(int3(p, 0)).rgb);
        result = float4(l0 + (m > 0 ? 0.3 + saturate(m / 0.1) * 0.5 : 0), l0 + (chosen ? 0.5 : 0), l0, 1);
    }
    if (marker > 0 && p.x < 32 && p.y < 32) result = float4(marker_level, 0, marker_level, 1);
    return result;
}
#endif
