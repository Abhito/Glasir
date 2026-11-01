// SPDX-License-Identifier: MIT
// AMD FidelityFX Super Resolution 1 (FSR 1) for mpv: EASU (edge-adaptive
// upscaling) to the window's size, then RCAS (sharpening). Ported for Glasir
// from AMD's ffx_fsr1.h (FidelityFX-FSR v1.0.2, the 32-bit float paths):
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
// Both passes run only when the picture is made larger (a capture smaller
// than the window). Taps are read at texel centres instead of AMD's gather4;
// the arithmetic is AMD's. Values are taken as they come (gamma or PQ
// encoded), as FSR 1 expects perceptual values in 0..1.

//!PARAM sharpness
//!DESC RCAS sharpness in stops: 0 is the most, each 1.0 halves it
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 2.0
0.2

//!HOOK MAIN
//!BIND HOOKED
//!SAVE GLASIR_EASU
//!WIDTH OUTPUT.w
//!HEIGHT OUTPUT.h
//!WHEN OUTPUT.w HOOKED.w > OUTPUT.h HOOKED.h > +
//!DESC Glasir FSR 1 EASU

float rcp_lo(float a) { return 1.0 / max(a, 1.0e-5); }
float rsq_lo(float a) { return inversesqrt(max(a, 1.0e-5)); }

// One tap of the kernel (FsrEasuTapF).
void easu_tap(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x);
    v *= len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wb = 2.0 / 5.0 * d2 - 1.0;
    float wa = lob * d2 - 1.0;
    wb *= wb;
    wa *= wa;
    wb = 25.0 / 16.0 * wb - (25.0 / 16.0 - 1.0);
    float w = wb * wa;
    ac += c * w;
    aw += w;
}

// Direction and length, weighted for one of the four nearest (FsrEasuSetF).
void easu_set(inout vec2 dir, inout float len, float w, float la, float lb, float lc, float ld, float le) {
    float dc = ld - lc;
    float cb = lc - lb;
    float lenx = rcp_lo(max(abs(dc), abs(cb)));
    float dirx = ld - lb;
    dir.x += dirx * w;
    lenx = clamp(abs(dirx) * lenx, 0.0, 1.0);
    len += lenx * lenx * w;
    float ec = le - lc;
    float ca = lc - la;
    float leny = rcp_lo(max(abs(ec), abs(ca)));
    float diry = le - la;
    dir.y += diry * w;
    leny = clamp(abs(diry) * leny, 0.0, 1.0);
    len += leny * leny * w;
}

vec3 tap(vec2 fp, float x, float y) { return HOOKED_tex((fp + vec2(x, y) + 0.5) * HOOKED_pt).rgb; }
float luma2(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }  // luma times 2

vec4 hook() {
    // This output pixel's position in the input, pixel centres at whole numbers.
    vec2 pp = HOOKED_pos * HOOKED_size - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = tap(fp, 0.0, -1.0), c = tap(fp, 1.0, -1.0);
    vec3 e = tap(fp, -1.0, 0.0), f = tap(fp, 0.0, 0.0), g = tap(fp, 1.0, 0.0), h = tap(fp, 2.0, 0.0);
    vec3 i = tap(fp, -1.0, 1.0), j = tap(fp, 0.0, 1.0), k = tap(fp, 1.0, 1.0), l = tap(fp, 2.0, 1.0);
    vec3 n = tap(fp, 0.0, 2.0), o = tap(fp, 1.0, 2.0);
    float bl = luma2(b), cl = luma2(c), el = luma2(e), fl = luma2(f), gl = luma2(g), hl = luma2(h);
    float il = luma2(i), jl = luma2(j), kl = luma2(k), ll = luma2(l), nl = luma2(n), ol = luma2(o);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bl, el, fl, gl, jl);
    easu_set(dir, len, pp.x * (1.0 - pp.y), cl, fl, gl, hl, kl);
    easu_set(dir, len, (1.0 - pp.x) * pp.y, fl, il, jl, kl, nl);
    easu_set(dir, len, pp.x * pp.y, gl, jl, kl, ll, ol);

    vec2 dir2 = dir * dir;
    float dirr = dir2.x + dir2.y;
    bool zro = dirr < 1.0 / 32768.0;
    dirr = zro ? 1.0 : rsq_lo(dirr);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirr;
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * rcp_lo(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = rcp_lo(lob);

    vec3 min4 = min(min(f, g), min(j, k));
    vec3 max4 = max(max(f, g), max(j, k));
    vec3 ac = vec3(0.0);
    float aw = 0.0;
    easu_tap(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easu_tap(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easu_tap(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    easu_tap(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easu_tap(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easu_tap(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easu_tap(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easu_tap(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easu_tap(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easu_tap(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easu_tap(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    easu_tap(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    return vec4(min(max4, max(min4, ac / aw)), 1.0);
}

//!HOOK MAIN
//!BIND GLASIR_EASU
//!WIDTH GLASIR_EASU.w
//!HEIGHT GLASIR_EASU.h
//!WHEN OUTPUT.w MAIN.w > OUTPUT.h MAIN.h > +
//!DESC Glasir FSR 1 RCAS

#define RCAS_LIMIT (0.25 - (1.0 / 16.0))

vec3 at(float x, float y) { return GLASIR_EASU_texOff(vec2(x, y)).rgb; }

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
    float lobe = max(-RCAS_LIMIT, min(max(max(lobe3.r, lobe3.g), lobe3.b), 0.0)) * exp2(-sharpness);
    float rcpl = 1.0 / (4.0 * lobe + 1.0);
    return vec4((lobe * (b + d + h + f) + e) * rcpl, 1.0);
}
