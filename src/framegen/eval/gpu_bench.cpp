// SPDX-License-Identifier: MPL-2.0
// What a second GPU would cost and give for frame generation, measured on
// this PC (docs/FRAMEGEN-LEARNINGS.md has what came of it):
//
//   gpu_bench.exe [--a NAME] [--b NAME] [--size WxH] [--runs N]
//
// a: the GPU mpv and DLSS 5 run on (the default one), b: the other. NAME is
// part of the GPU's name ("4070"). Frames are 10-bit RGB (R10G10B10A2), the
// format of mpv's back buffer here, with a pattern that moves between the two.
//
// 1. The generator (fg_interp.h) on each GPU: submit, flush, wait until done.
// 2. A frame from a to b and back through system memory (two staging
//    textures mapped at once, one memcpy): each step, and both ways.
// 3. The whole round: a frame of a's to b, the frame in between made there,
//    and back into a texture of a's.
// 4. A second device on a itself: its generator while the first device is
//    idle, while the first device's commands wait on a fence (how the real
//    frame is held), and while the first device is kept busy with generator
//    draws back to back (standing in for DLSS 5).
//
// Times are wall-clock milliseconds, the median of the runs (and the 90th
// percentile).
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "fg_interp.h"

using Microsoft::WRL::ComPtr;

namespace {

double now_ms() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

struct Times {
    std::vector<double> v;
    void add(double ms) { v.push_back(ms); }
    double at(double share) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, size_t(share * double(v.size())))];
    }
    void print(const char *what) { printf("  %-62s %7.2f ms   (90%%: %.2f)\n", what, at(0.5), at(0.9)); }
};

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    // When the GPU is done is told by a fence: an event query polled here
    // answered only at the next timer tick (15.6 ms) on NVIDIA's driver.
    ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<ID3D11Fence> done;
    HANDLE done_event = nullptr;
    UINT64 done_value = 0;
    std::wstring name;

    bool open(IDXGIAdapter1 *adapter) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                     levels, 3, D3D11_SDK_VERSION, &device, nullptr, &ctx)))
            return false;
        ComPtr<ID3D11Device5> device5;
        if (FAILED(device.As(&device5)) || FAILED(ctx.As(&ctx4)) ||
            FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&done))))
            return false;
        done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> a;
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&a)) && SUCCEEDED(a->GetDesc(&d))) name = d.Description;
        return true;
    }
    // Until the GPU has done everything submitted so far.
    void wait() {
        ctx4->Signal(done.Get(), ++done_value);
        ctx->Flush();
        done->SetEventOnCompletion(done_value, done_event);
        WaitForSingleObject(done_event, INFINITE);
    }
};

struct Frame {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

Frame make_frame(Gpu &g, UINT w, UINT h) {
    Frame f;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = kFormat;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    g.device->CreateTexture2D(&d, nullptr, &f.tex);
    g.device->CreateShaderResourceView(f.tex.Get(), nullptr, &f.srv);
    g.device->CreateRenderTargetView(f.tex.Get(), nullptr, &f.rtv);
    return f;
}

ComPtr<ID3D11Texture2D> make_staging(Gpu &g, UINT w, UINT h, UINT access) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = kFormat;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = access;
    ComPtr<ID3D11Texture2D> t;
    g.device->CreateTexture2D(&d, nullptr, &t);
    return t;
}

// Blobs and stripes, moved by `shift` pixels: something for the search to find.
void fill(Gpu &g, Frame &f, UINT w, UINT h, int shift) {
    std::vector<UINT32> px(size_t(w) * h);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            const double u = double(int(x) - shift), v = double(y);
            const double a = 0.5 + 0.5 * std::sin(u * 0.013) * std::cos(v * 0.017);
            const double b = 0.5 + 0.5 * std::sin((u + v) * 0.05);
            const double c = ((int(u) / 64 + int(v) / 64) & 1) ? 0.7 : 0.3;
            const UINT32 r = UINT32(a * 1023), gr = UINT32(b * 1023), bl = UINT32(c * 1023);
            px[size_t(y) * w + x] = r | (gr << 10) | (bl << 20) | (3u << 30);
        }
    g.ctx->UpdateSubresource(f.tex.Get(), 0, nullptr, px.data(), w * 4, 0);
}

// A frame of `from`'s into a texture of `to`'s, through system memory.
// Returns the steps: reading it back, the memcpy, and until `to` has it.
struct CopySteps {
    double read = 0, copy = 0, write = 0;
};
CopySteps move_frame(Gpu &from, ID3D11Texture2D *source, ID3D11Texture2D *from_staging, Gpu &to,
                     ID3D11Texture2D *to_staging, ID3D11Texture2D *target, UINT w, UINT h) {
    CopySteps s;
    double t = now_ms();
    from.ctx->CopyResource(from_staging, source);
    D3D11_MAPPED_SUBRESOURCE in{}, out{};
    from.ctx->Map(from_staging, 0, D3D11_MAP_READ, 0, &in);  // waits for the GPU
    s.read = now_ms() - t;
    t = now_ms();
    to.ctx->Map(to_staging, 0, D3D11_MAP_WRITE, 0, &out);
    if (in.RowPitch == out.RowPitch) {
        memcpy(out.pData, in.pData, size_t(in.RowPitch) * h);
    } else {
        for (UINT y = 0; y < h; ++y)
            memcpy(static_cast<BYTE *>(out.pData) + size_t(y) * out.RowPitch,
                   static_cast<const BYTE *>(in.pData) + size_t(y) * in.RowPitch, size_t(w) * 4);
    }
    to.ctx->Unmap(to_staging, 0);
    from.ctx->Unmap(from_staging, 0);
    s.copy = now_ms() - t;
    t = now_ms();
    to.ctx->CopyResource(target, to_staging);
    to.wait();
    s.write = now_ms() - t;
    return s;
}

ComPtr<IDXGIAdapter1> find_adapter(const std::wstring &name, const std::wstring &not_named) {
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> a;
    for (UINT n = 0; factory && factory->EnumAdapters1(n, &a) == S_OK; ++n, a.Reset()) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        const std::wstring text = d.Description;
        if (!name.empty() ? text.find(name) != std::wstring::npos : (not_named.empty() || text != not_named)) return a;
    }
    return nullptr;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    std::wstring name_a, name_b;
    UINT w = 2560, h = 1440;
    int runs = 120;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--a" && i + 1 < argc) name_a = argv[++i];
        else if (a == L"--b" && i + 1 < argc) name_b = argv[++i];
        else if (a == L"--size" && i + 1 < argc) swscanf(argv[++i], L"%ux%u", &w, &h);
        else if (a == L"--runs" && i + 1 < argc) runs = std::max(10, _wtoi(argv[++i]));
        else {
            fwprintf(stderr, L"usage: gpu_bench.exe [--a NAME] [--b NAME] [--size WxH] [--runs N]\n");
            return 1;
        }
    }
    Gpu a, a2, b;
    const ComPtr<IDXGIAdapter1> adapter_a = find_adapter(name_a, L"");
    if (!adapter_a || !a.open(adapter_a.Get()) || !a2.open(adapter_a.Get())) {
        fwprintf(stderr, L"no GPU for --a\n");
        return 1;
    }
    const ComPtr<IDXGIAdapter1> adapter_b = find_adapter(name_b, a.name);
    const bool two = adapter_b && b.open(adapter_b.Get());
    wprintf(L"a: %s\nb: %s\nframes: %ux%u, 10-bit RGB (%.1f MB each)\n\n", a.name.c_str(), two ? b.name.c_str() : L"(none)", w,
            h, double(w) * h * 4 / 1e6);

    struct Side {
        Gpu *gpu;
        Frame frame[2], target;
        fg::Interpolator interp;
        bool ok = false;
        void make(UINT w, UINT h) {
            frame[0] = make_frame(*gpu, w, h);
            frame[1] = make_frame(*gpu, w, h);
            target = make_frame(*gpu, w, h);
            fill(*gpu, frame[0], w, h, 0);
            fill(*gpu, frame[1], w, h, 9);
            std::string error;
            ok = interp.create(gpu->device.Get(), w, h, error);
            if (!ok) fprintf(stderr, "no generator: %s\n", error.c_str());
            for (int i = 0; ok && i < 5; ++i) draw();  // the first draws compile and allocate
            gpu->wait();
        }
        void draw() {
            fg::DrawOptions o;
            interp.draw(gpu->ctx.Get(), frame[0].srv.Get(), frame[1].srv.Get(), target.rtv.Get(), o);
        }
    };
    Side sa{&a}, sa2{&a2}, sb{&b};
    sa.make(w, h);
    sa2.make(w, h);
    if (two) sb.make(w, h);
    if (!sa.ok || !sa2.ok || (two && !sb.ok)) return 1;

    printf("1. the generator, one frame in between\n");
    for (Side *s : {&sa, &sb}) {
        if (s == &sb && !two) continue;
        Times t;
        for (int i = 0; i < runs; ++i) {
            const double start = now_ms();
            s->draw();
            s->gpu->wait();
            t.add(now_ms() - start);
        }
        t.print(s == &sa ? "on a" : "on b");
    }

    if (two) {
        printf("2. a frame through system memory\n");
        const auto a_read = make_staging(a, w, h, D3D11_CPU_ACCESS_READ), a_write = make_staging(a, w, h, D3D11_CPU_ACCESS_WRITE);
        const auto b_read = make_staging(b, w, h, D3D11_CPU_ACCESS_READ), b_write = make_staging(b, w, h, D3D11_CPU_ACCESS_WRITE);
        Times ar, ac, aw, at, br, bc, bw, bt;
        for (int i = 0; i < runs; ++i) {
            a.wait();
            b.wait();
            const CopySteps s = move_frame(a, sa.frame[i & 1].tex.Get(), a_read.Get(), b, b_write.Get(), sb.frame[i & 1].tex.Get(), w, h);
            ar.add(s.read), ac.add(s.copy), aw.add(s.write), at.add(s.read + s.copy + s.write);
            const CopySteps r = move_frame(b, sb.target.tex.Get(), b_read.Get(), a, a_write.Get(), sa.target.tex.Get(), w, h);
            br.add(r.read), bc.add(r.copy), bw.add(r.write), bt.add(r.read + r.copy + r.write);
        }
        ar.print("a to b: read back from a");
        ac.print("a to b: memcpy");
        aw.print("a to b: into b's texture");
        at.print("a to b: all of it");
        br.print("b to a: read back from b");
        bc.print("b to a: memcpy");
        bw.print("b to a: into a's texture");
        bt.print("b to a: all of it");

        printf("3. the whole round: a's frame to b, the frame in between made there, back to a\n");
        Times round;
        for (int i = 0; i < runs; ++i) {
            a.wait();
            b.wait();
            const double start = now_ms();
            move_frame(a, sa.frame[i & 1].tex.Get(), a_read.Get(), b, b_write.Get(), sb.frame[i & 1].tex.Get(), w, h);
            sb.draw();
            move_frame(b, sb.target.tex.Get(), b_read.Get(), a, a_write.Get(), sa.target.tex.Get(), w, h);
            round.add(now_ms() - start);
        }
        round.print("from a's frame to the frame in between in a's texture");
    }

    printf("4. a second device on a: its generator, one frame in between\n");
    {
        Times alone;
        for (int i = 0; i < runs; ++i) {
            const double start = now_ms();
            sa2.draw();
            a2.wait();
            alone.add(now_ms() - start);
        }
        alone.print("the first device idle");

        // The first device's commands wait on a fence, as the real frame held for its time does.
        ComPtr<ID3D11Device5> dev5_a, dev5_a2;
        ComPtr<ID3D11DeviceContext4> ctx4_a, ctx4_a2;
        ComPtr<ID3D11Fence> fence_a2, fence_a;
        HANDLE shared = nullptr;
        if (SUCCEEDED(a.device.As(&dev5_a)) && SUCCEEDED(a2.device.As(&dev5_a2)) && SUCCEEDED(a.ctx.As(&ctx4_a)) &&
            SUCCEEDED(a2.ctx.As(&ctx4_a2)) &&
            SUCCEEDED(dev5_a2->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_a2))) &&
            SUCCEEDED(fence_a2->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared)) &&
            SUCCEEDED(dev5_a->OpenSharedFence(shared, IID_PPV_ARGS(&fence_a)))) {
            Times waiting;
            UINT64 value = 0;
            for (int i = 0; i < runs; ++i) {
                ++value;
                ctx4_a->Wait(fence_a.Get(), value);
                sa.draw();  // behind the wait
                a.ctx->Flush();
                const double start = now_ms();
                sa2.draw();
                a2.wait();
                waiting.add(now_ms() - start);
                ctx4_a2->Signal(fence_a2.Get(), value);
                a2.ctx->Flush();
                a.wait();
            }
            waiting.print("the first device's commands waiting on a fence");
        } else {
            printf("  (no shared fences here)\n");
        }
        if (shared) CloseHandle(shared);

        // The first device kept busy: generator draws back to back, three to a flush (about a DLSS 5 pass).
        std::atomic<bool> stop{false};
        std::atomic<long> batches{0};
        std::thread load([&] {
            while (!stop) {
                for (int i = 0; i < 3; ++i) sa.draw();
                a.wait();
                ++batches;
            }
        });
        Sleep(200);
        Times busy;
        const long before = batches;
        const double began = now_ms();
        for (int i = 0; i < runs; ++i) {
            const double start = now_ms();
            sa2.draw();
            a2.wait();
            busy.add(now_ms() - start);
            Sleep(8);  // a frame in between now and then, not back to back
        }
        const double per_batch = (now_ms() - began) / double(std::max(1L, batches - before));
        stop = true;
        load.join();
        busy.print("the first device busy with generator draws back to back");
        printf("  %-62s %7.2f ms\n", "(the first device's three draws took, meanwhile)", per_batch);
    }
    return 0;
}
