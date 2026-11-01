// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// The frame in between two real frames. This first version blends them; the
// motion-compensated version will replace PSMain and keep the same bindings.
//
// The back buffer holds whatever mpv writes (PQ-encoded rgb10_a2 for HDR,
// sRGB-encoded for SDR, linear for scRGB). Blending happens in that encoding,
// which is fine for a 50/50 mix of two nearby frames.

Texture2D<float4> prev_frame : register(t0);
Texture2D<float4> next_frame : register(t1);

cbuffer Params : register(b0)
{
    float weight;        // 0 = prev_frame, 1 = next_frame
    float marker;        // 1 = draw the debug square on generated frames
    float marker_level;  // the square's brightness in the back buffer's encoding
    float pad;
};

// A full-screen triangle from the vertex id alone, no vertex buffer.
float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    int3 p = int3(pos.xy, 0);
    float4 c = lerp(prev_frame.Load(p), next_frame.Load(p), weight);
    if (marker > 0 && p.x < 32 && p.y < 32)
        c = float4(marker_level, 0, marker_level, 1);
    return c;
}
