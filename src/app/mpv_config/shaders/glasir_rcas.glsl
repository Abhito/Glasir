// SPDX-License-Identifier: MIT
// "Sharpen" (glasir.ini video.sharpen): RCAS, the sharpening pass of AMD
// FidelityFX Super Resolution 1, alone on mpv's finished picture - at the
// size it is shown, whatever scaled it, or nothing did. glasir_scaling.lua
// puts this shader in mpv's list while Sharpen is on and sets its strength.
// The pass is the one in glasir_fsr1.glsl (ported for Glasir from AMD's
// ffx_fsr1.h, FidelityFX-FSR v1.0.2, the 32-bit float path):
// https://github.com/GPUOpen-Effects/FidelityFX-FSR
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Copyright (c) 2026 Abhito (the mpv port).
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// It runs on mpv's output (the OUTPUT hook): after mpv's own scaler and the
// scaling shaders, on values as they go to the screen (gamma or PQ encoded,
// as RCAS expects perceptual values in 0..1), and before DLSS 5, which keeps
// most of it. RCAS limits itself where a pixel's four neighbours are already
// far apart, so that nothing is pushed out of 0..1. At full strength it
// brings up a game's grain, which is why Glasir's strongest setting stops
// short of that.

//!PARAM glasir_sharpen
//!DESC RCAS sharpness in stops: 0 is the most, each 1.0 halves it
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 4.0
1.0

//!HOOK OUTPUT
//!BIND HOOKED
//!DESC Glasir sharpen (RCAS)

#define RCAS_LIMIT (0.25 - (1.0 / 16.0))

vec3 at(float x, float y) { return HOOKED_texOff(vec2(x, y)).rgb; }

vec4 hook() {
    //    b
    //  d e f
    //    h
    vec3 b = at(0.0, -1.0), d = at(-1.0, 0.0), e = at(0.0, 0.0), f = at(1.0, 0.0), h = at(0.0, 1.0);
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    // Limiters (FsrRcasF), per channel.
    // (Kept off 0 / 0 at pure black and pure white.)
    vec3 hit_min = min(mn4, e) / max(4.0 * mx4, vec3(1.0e-5));
    vec3 hit_max = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0e-5));
    vec3 lobe3 = max(-hit_min, hit_max);
    float lobe = max(-RCAS_LIMIT, min(max(max(lobe3.r, lobe3.g), lobe3.b), 0.0)) * exp2(-glasir_sharpen);
    float rcpl = 1.0 / (4.0 * lobe + 1.0);
    return vec4((lobe * (b + d + h + f) + e) * rcpl, 1.0);
}
