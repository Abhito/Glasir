// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// Frames to and from NVIDIA's Video Frame Generation (fg_vfg.h).
//
// The filter reads and writes rows of packed pixels in CUDA memory, and CUDA
// shares a Direct3D 11 buffer as such memory (a 10-bit texture it cannot
// share at all). PackCS writes the picture's part of a frame into a buffer,
// one 32-bit word a pixel; UnpackPS draws the buffer the filter wrote into
// the frame, and what lies outside the picture as frame a has it.
//
// The values are the frame's as they are (PQ-encoded for HDR, sRGB-encoded
// for SDR): the filter gets the encoding the screen gets.

cbuffer VfgParams : register(b0)
{
    uint origin_x;       // the picture's corner in the frame
    uint origin_y;
    uint picture_w;      // its size: a buffer row, and the rows
    uint picture_h;
    uint ten_bit;        // 1: R10 G10 B10 A2, red lowest; 0: R8 G8 B8 A8, red the first byte
    float marker;        // 1 = debug square on the frame in between
    float marker_level;  // its brightness in the frame's encoding
    uint pad;
};

#ifdef PACK
Texture2D<float4> frame : register(t0);
RWBuffer<uint> packed : register(u0);

[numthreads(8, 8, 1)]
void PackCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= picture_w || id.y >= picture_h) return;
    const float3 c = saturate(frame.Load(int3(id.xy + uint2(origin_x, origin_y), 0)).rgb);
    uint word;
    if (ten_bit) {
        const uint3 v = uint3(c * 1023.0 + 0.5);
        word = v.r | (v.g << 10) | (v.b << 20) | 0xC0000000u;
    } else {
        const uint3 v = uint3(c * 255.0 + 0.5);
        word = v.r | (v.g << 8) | (v.b << 16) | 0xFF000000u;
    }
    packed[id.y * picture_w + id.x] = word;
}
#endif

#ifdef UNPACK
Buffer<uint> generated : register(t0);
Texture2D<float4> frame_a : register(t1);

float4 UnpackPS(float4 pos : SV_Position) : SV_Target
{
    const uint2 p = uint2(pos.xy);
    float4 c;
    if (p.x >= origin_x && p.y >= origin_y && p.x - origin_x < picture_w && p.y - origin_y < picture_h) {
        const uint word = generated[(p.y - origin_y) * picture_w + (p.x - origin_x)];
        if (ten_bit)
            c = float4(float3(word & 1023u, (word >> 10) & 1023u, (word >> 20) & 1023u) / 1023.0, 1);
        else
            c = float4(float3(word & 255u, (word >> 8) & 255u, (word >> 16) & 255u) / 255.0, 1);
        // In the picture's corner, as fg_interp.h's is.
        if (marker > 0 && p.x - origin_x < 32 && p.y - origin_y < 32) c = float4(marker_level, 0, marker_level, 1);
    } else {
        c = frame_a.Load(int3(p, 0));
    }
    return c;
}
#endif
