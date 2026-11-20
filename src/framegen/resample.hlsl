// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// A picture drawn at another size (fg_resample.h): what takes NVIDIA Super
// Resolution's output to its place in the window when "Scale to" makes it
// another size than that place - down from a larger one (supersampling), up
// from a smaller one.
//
// Made smaller, every source pixel under an output pixel counts (the kernel is
// as many source pixels wide as the picture is made smaller: nothing is
// skipped, so nothing shimmers), weighted by a Hermite curve, which has no
// negative lobes and so adds no ringing - mpv's own default for scaling down
// (dscale = hermite, correct-downscaling), which the other scalers' pictures
// go through. Made larger, Catmull-Rom over four by four pixels.
//
// The values are averaged as they are (gamma-encoded SDR, PQ), not as light.

cbuffer ResampleParams : register(b0)
{
    float2 src_size;     // the source, in pixels
    float2 dst_origin;   // the corner of its place in the target
    float2 dst_size;     // the size of that place
    float2 pad;
};

Texture2D<float4> source : register(t0);

float weight(float x, bool down)
{
    x = abs(x);
    if (down) return x < 1.0 ? (2.0 * x - 3.0) * x * x + 1.0 : 0.0;  // Hermite
    return x < 1.0 ? (1.5 * x - 2.5) * x * x + 1.0                    // Catmull-Rom
         : x < 2.0 ? ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0 : 0.0;
}

float4 ResamplePS(float4 pos : SV_Position) : SV_Target
{
    const float2 ratio = src_size / dst_size;            // source pixels to an output pixel
    const float2 at = (pos.xy - dst_origin) * ratio;     // this pixel's middle, in source pixels
    const bool2 down = ratio > 1.0005;
    const float2 stretch = max(ratio, 1.0);
    const float2 reach = float2(down.x ? 1.0 : 2.0, down.y ? 1.0 : 2.0) * stretch;
    // The source pixels whose middles (n + 0.5) lie within reach; at most 16 a side (8 times smaller),
    // the outermost left out evenly on both sides.
    int2 first = int2(ceil(at - reach - 0.5));
    int2 last = int2(floor(at + reach - 0.5));
    const int2 extra = max(last - first - 15, 0);
    first += extra >> 1;
    last = first + min(last - first, 15);
    const int2 edge = int2(src_size) - 1;

    float3 sum = 0;
    float total = 0;
    [loop] for (int y = first.y; y <= last.y; ++y) {
        const float wy = weight((float(y) + 0.5 - at.y) / stretch.y, down.y);
        float3 row = 0;
        float row_total = 0;
        [loop] for (int x = first.x; x <= last.x; ++x) {
            const float wx = weight((float(x) + 0.5 - at.x) / stretch.x, down.x);
            row += source.Load(int3(clamp(int2(x, y), 0, edge), 0)).rgb * wx;
            row_total += wx;
        }
        sum += row * wy;
        total += row_total * wy;
    }
    return float4(saturate(sum / total), 1.0);
}
