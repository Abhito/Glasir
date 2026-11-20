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
// Catmull-Rom both ways: over four by four pixels when the picture is made
// larger. Made smaller, the curve is as many source pixels wider as the
// picture is made smaller, so every source pixel under an output pixel counts
// (nothing is skipped, so nothing shimmers; ResamplePS says how far that
// holds) and the picture keeps its fine
// detail: a curve without negative lobes (Hermite, mpv's own default for
// scaling down, which this pass used at first) softens whatever it is given,
// and a 1440p picture by way of 4K came out softer than it went in. The price
// is a little ringing at hard edges, which Catmull-Rom's small lobes keep
// slight.
//
// In two passes, which give the same picture as one over both directions for
// far fewer pixels read (twelve and twelve for one of 8K's made 1440p,
// instead of twelve times twelve): across into a picture between, as wide as
// the place and as tall as the source, then down from that into the place.
// The picture between is 16 bits a channel and holds half the value plus a
// quarter, so what the curve's lobes put under black or over white on the way
// is kept for the second pass.
//
// The values are averaged as they are (gamma-encoded SDR, PQ), not as light.

cbuffer ResampleParams : register(b0)
{
    float2 src_size;     // this pass's source, in pixels
    float2 dst_origin;   // the corner of its place in the target
    float2 dst_size;     // the size of that place
    float2 axis;         // (1, 0): across, into the picture between; (0, 1): down, out of it
};

Texture2D<float4> source : register(t0);

float weight(float x)  // Catmull-Rom
{
    x = abs(x);
    return x < 1.0 ? (1.5 * x - 2.5) * x * x + 1.0
         : x < 2.0 ? ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0 : 0.0;
}

float4 ResamplePS(float4 pos : SV_Position) : SV_Target
{
    const bool down = axis.y > 0.5;                       // the second pass
    const float2 place = pos.xy - dst_origin;             // this pixel's middle within its place
    const float ratio = down ? src_size.y / dst_size.y : src_size.x / dst_size.x;  // source pixels to an output pixel
    const float at = (down ? place.y : place.x) * ratio;  // this pixel's middle, in source pixels
    const int other = int(down ? place.x : place.y);      // the other direction: pixel for pixel
    const float stretch = max(ratio, 1.0);
    const float reach = 2.0 * stretch;
    // The source pixels whose middles (n + 0.5) lie within reach; at most 16, which is all of them
    // down to 4 times smaller - as far as the add-on goes (the effect makes the picture at most four
    // times larger, and its place is never smaller than the picture came). Beyond that (sr_eval
    // --resample can ask for it) the outermost are left out, as many before as after or one more
    // after: the curve's small lobes first, and past 8 times smaller pixels under the output pixel
    // too.
    int first = int(ceil(at - reach - 0.5));
    int last = int(floor(at + reach - 0.5));
    first += max(last - first - 15, 0) >> 1;
    last = first + min(last - first, 15);
    const int edge = int(down ? src_size.y : src_size.x) - 1;

    float3 sum = 0;
    float total = 0;
    [loop] for (int n = first; n <= last; ++n) {
        const float w = weight((float(n) + 0.5 - at) / stretch);
        const int c = clamp(n, 0, edge);
        sum += source.Load(down ? int3(other, c, 0) : int3(c, other, 0)).rgb * w;
        total += w;
    }
    const float3 value = sum / total;
    // Into the picture between: half the value plus a quarter. Out of it: the value again.
    return down ? float4(saturate(value * 2.0 - 0.5), 1.0) : float4(value * 0.5 + 0.25, 1.0);
}
