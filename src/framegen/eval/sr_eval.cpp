// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// sr_eval - a picture through fg_sr.h (NVIDIA's Video Super Resolution as the
// add-on runs it: Direct3D 11 buffers shared with CUDA), from a file of raw
// pixels to a file of raw pixels. sr_eval.py feeds it game frames, compares
// what comes out with the SDK run without Direct3D, and scores both.
//
//   sr_eval --runtime DIR --in FILE --size WxH --to WxH --out FILE
//           [--level low|medium|high|ultra] [--deep] [--then WxH --out2 FILE] [--runs N]
//   sr_eval --in FILE --size WxH --resample WxH --out FILE [--deep]
//
// --resample: the picture through fg_resample.h alone (what takes the effect's
// picture to its place in the window when "Scale to" is another size), to
// check it against a resampler written out in full. Nothing of NVIDIA's runs
// then.
//
// The files: 4 bytes a pixel, rows from the top - R, G, B, A, or with --deep
// one 32-bit word, R10 G10 B10 A2 with red lowest. --then: the same picture
// again at another size, in the same effect (its model is not loaded anew, as
// when a window is resized). --runs N: draws N times and says how long one
// took - for the developer: NVIDIA's licence forbids giving results of its
// software (speed, quality) to others, so not for the repository or a report.

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../fg_resample.h"
#include "../fg_sr.h"

using Microsoft::WRL::ComPtr;

namespace {

bool read_file(const std::string &path, std::vector<BYTE> &data, size_t bytes) {
    FILE *f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0) return false;
    data.resize(bytes);
    const size_t got = fread(data.data(), 1, bytes, f);
    fclose(f);
    return got == bytes;
}

bool write_file(const std::string &path, const BYTE *data, size_t bytes) {
    FILE *f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0) return false;
    const size_t put = fwrite(data, 1, bytes, f);
    fclose(f);
    return put == bytes;
}

bool size_of(const char *text, UINT &w, UINT &h) { return sscanf_s(text, "%ux%u", &w, &h) == 2 && w >= 16 && h >= 16; }

double now_ms() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

}  // namespace

int main(int argc, char **argv) {
    std::string runtime, in, out, out2;
    UINT w = 0, h = 0, ow = 0, oh = 0, ow2 = 0, oh2 = 0;
    fg::SrLevel level = fg::SrLevel::medium;
    bool deep = false, resample = false;
    int runs = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--deep") {
            deep = true;
            continue;
        }
        ++i;
        if (a == "--runtime") runtime = v;
        else if (a == "--in") in = v;
        else if (a == "--out") out = v;
        else if (a == "--out2") out2 = v;
        else if (a == "--size" && size_of(v, w, h)) {}
        else if (a == "--to" && size_of(v, ow, oh)) {}
        else if (a == "--resample" && size_of(v, ow, oh)) resample = true;
        else if (a == "--then" && size_of(v, ow2, oh2)) {}
        else if (a == "--runs") runs = atoi(v);
        else if (a == "--level") {
            const std::string l = v;
            level = l == "low" ? fg::SrLevel::low : l == "high" ? fg::SrLevel::high : l == "ultra" ? fg::SrLevel::ultra
                                                                                                   : fg::SrLevel::medium;
        } else {
            fprintf(stderr, "sr_eval: %s %s?\n", a.c_str(), v);
            return 2;
        }
    }
    if ((runtime.empty() && !resample) || in.empty() || out.empty() || !w || !ow) {
        fprintf(stderr, "sr_eval --runtime DIR --in FILE --size WxH --to WxH --out FILE [--level L] [--deep] "
                        "[--then WxH --out2 FILE] [--runs N]\n"
                        "sr_eval --in FILE --size WxH --resample WxH --out FILE [--deep]\n");
        return 2;
    }

    std::vector<BYTE> pixels;
    if (!read_file(in, pixels, size_t(w) * h * 4)) return fprintf(stderr, "sr_eval: cannot read %s\n", in.c_str()), 1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &ctx)))
        return fprintf(stderr, "sr_eval: no Direct3D 11 device\n"), 1;
    const DXGI_FORMAT format = deep ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init{pixels.data(), w * 4, 0};
    ComPtr<ID3D11Texture2D> source;
    ComPtr<ID3D11ShaderResourceView> source_view;
    if (FAILED(device->CreateTexture2D(&d, &init, &source)) ||
        FAILED(device->CreateShaderResourceView(source.Get(), nullptr, &source_view)))
        return fprintf(stderr, "sr_eval: no source texture\n"), 1;

    const std::wstring folder(runtime.begin(), runtime.end());
    fg::Sr sr;
    fg::Resample to_size;
    if (resample) {
        std::string error;
        if (!to_size.create(device.Get(), error)) return fprintf(stderr, "sr_eval: %s\n", error.c_str()), 1;
    }
    const struct {
        UINT w, h;
        const std::string &file;
    } passes[] = {{ow, oh, out}, {ow2, oh2, out2}};
    for (const auto &pass : passes) {
        if (!pass.w || pass.file.empty()) continue;
        std::string error;
        const double t0 = now_ms();
        if (!resample && !sr.create(device.Get(), w, h, pass.w, pass.h, deep, folder, level, error))
            return fprintf(stderr, "sr_eval: %s\n", error.c_str()), 1;
        const double made = now_ms() - t0;
        d.Width = pass.w;
        d.Height = pass.h;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target, staging;
        ComPtr<ID3D11RenderTargetView> target_view;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &target)) ||
            FAILED(device->CreateRenderTargetView(target.Get(), nullptr, &target_view)))
            return fprintf(stderr, "sr_eval: no target texture\n"), 1;
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &staging))) return fprintf(stderr, "sr_eval: no staging texture\n"), 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        double sum = 0;
        for (int run = 0; run <= runs; ++run) {
            const double t = now_ms();
            if (resample) {
                if (!to_size.draw(ctx.Get(), source_view.Get(), w, h, target_view.Get(), 0, 0, pass.w, pass.h))
                    return fprintf(stderr, "sr_eval: no texture between the passes\n"), 1;
            } else if (!sr.draw(ctx.Get(), source_view.Get(), target_view.Get(), 0, 0, error))
                return fprintf(stderr, "sr_eval: %s\n", error.c_str()), 1;
            if (run > 0) sum += now_ms() - t;  // the first one aside
        }
        ctx->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return fprintf(stderr, "sr_eval: no read-back\n"), 1;
        std::vector<BYTE> result(size_t(pass.w) * pass.h * 4);
        for (UINT y = 0; y < pass.h; ++y)
            memcpy(result.data() + size_t(y) * pass.w * 4, static_cast<const BYTE *>(map.pData) + size_t(y) * map.RowPitch,
                   size_t(pass.w) * 4);
        ctx->Unmap(staging.Get(), 0);
        if (!write_file(pass.file, result.data(), result.size()))
            return fprintf(stderr, "sr_eval: cannot write %s\n", pass.file.c_str()), 1;
        printf("%ux%u -> %ux%u, set up in %.0f ms", w, h, pass.w, pass.h, made);
        if (runs > 0) printf(", %.2f ms a picture over %d", sum / runs, runs);
        printf("\n");
    }
    return 0;
}
