// SPDX-License-Identifier: MIT
// Puts the menu (drawn by Dear ImGui into an sRGB texture, premultiplied by
// its own blending) onto the frame, in the frame's encoding:
//   0  SDR: as it is
//   1  HDR10: BT.2020 primaries, PQ, white at paper_nits
//   2  scRGB: linear BT.709, 1.0 = 80 nits, white at paper_nits
// The frame's blend state mixes it in with its alpha.

Texture2D<float4> ui : register(t0);

cbuffer Params : register(b0) {
    uint mode;
    float paper_nits;
    float2 pad;
};

void VSMain(uint id : SV_VertexID, out float4 pos : SV_Position) {
    // One triangle over the whole target.
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 srgb_to_linear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 pq_encode(float3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

static const float3x3 kBt709To2020 = {
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950,
};

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    float4 c = ui.Load(int3(pos.xy, 0));
    if (c.a <= 0.0) discard;
    float3 s = saturate(c.rgb / c.a);
    if (mode == 1)
        s = pq_encode(mul(kBt709To2020, srgb_to_linear(s)) * paper_nits);
    else if (mode == 2)
        s = srgb_to_linear(s) * (paper_nits / 80.0);
    return float4(s, c.a);
}
