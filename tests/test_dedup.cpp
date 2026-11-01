// SPDX-License-Identifier: MIT
// Checks dedup (RepeatFilter), the frame slot's drop counting, and repeats
// sent while frame generation is off, on synthetic P010, NV12 and YUY2 frames.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "capture.h"
#include "frame_slot.h"
#include "repeat_filter.h"

using namespace glasir;

namespace {

constexpr UINT32 W = 640, H = 360;
int g_failures = 0;

std::vector<BYTE> make_frame(UINT16 base10) {
    std::vector<BYTE> f(size_t(W) * H * 3);
    auto *y = reinterpret_cast<UINT16 *>(f.data());
    for (size_t i = 0; i < size_t(W) * H; ++i) y[i] = static_cast<UINT16>((base10 + (i % 7)) << 6);
    return f;
}

// Moves a 2x2 block of luma far, at (x, y): the smallest thing a frame can change.
void poke(std::vector<BYTE> &f, UINT32 x, UINT32 y, UINT16 value10) {
    auto *luma = reinterpret_cast<UINT16 *>(f.data());
    for (UINT32 dy = 0; dy < 2; ++dy)
        for (UINT32 dx = 0; dx < 2; ++dx) luma[size_t(y + dy) * W + x + dx] = static_cast<UINT16>(value10 << 6);
}

void expect(bool got, bool want, const char *what) {
    std::printf("%-58s %s\n", what, got == want ? "ok" : "FAIL");
    if (got != want) ++g_failures;
}

}  // namespace

int main() {
    {
        RepeatFilter f(PixelFormat::P010, W, H, 2, 100000);
        auto a = make_frame(100), b = make_frame(300), c = make_frame(500);
        expect(f.take(a.data()), true, "30-in-60: first A is sent");
        expect(f.take(a.data()), false, "30-in-60: repeated A is dropped");
        expect(f.take(b.data()), true, "30-in-60: B is sent");
        expect(f.take(b.data()), false, "30-in-60: repeated B is dropped");
        expect(f.take(c.data()), true, "30-in-60: C is sent");
    }
    {
        RepeatFilter f(PixelFormat::P010, W, H, 2, 100000);
        auto a = make_frame(100), noisy = make_frame(100);
        auto *y = reinterpret_cast<UINT16 *>(noisy.data());
        for (size_t i = 0; i < size_t(W) * H; i += 3) y[i] = static_cast<UINT16>(((y[i] >> 6) + 2) << 6);
        f.take(a.data());
        expect(f.take(noisy.data()), false, "noise of 2 codes counts as a repeat");
    }
    {
        RepeatFilter f(PixelFormat::P010, W, H, 2, 100000);
        auto a = make_frame(100);
        f.take(a.data());
        auto tiny = a;
        poke(tiny, 100, 100, 900);  // a 2x2 block on a sampled row and column
        expect(f.take(tiny.data()), false, "one 2x2 block changed (1 sample) stays a repeat");
        auto more = a;
        for (UINT32 i = 0; i < 4; ++i) poke(more, 100 + 8 * i, 200, 900);
        expect(f.take(more.data()), true, "four small spots changed (4 samples) is new");
    }
    {
        RepeatFilter f(PixelFormat::P010, W, H, 2, 100000);
        auto f0 = make_frame(100), f1 = make_frame(101), f2 = make_frame(102), f3 = make_frame(103);
        f.take(f0.data());
        expect(f.take(f1.data()), false, "slow fade: +1 code is a repeat");
        expect(f.take(f2.data()), false, "slow fade: +2 codes from the last sent is a repeat");
        expect(f.take(f3.data()), true, "slow fade: +3 codes from the last sent goes out");
    }
    {
        RepeatFilter f(PixelFormat::P010, W, H, 2, 60);
        auto a = make_frame(100);
        f.take(a.data());
        expect(f.keepalive(), false, "keepalive: the first frame is new, not a keepalive");
        expect(f.take(a.data()), false, "keepalive: repeat right away is dropped");
        Sleep(120);
        expect(f.take(a.data()), true, "keepalive: repeat after the keepalive is sent");
        expect(f.keepalive(), true, "keepalive: and is marked as one (not a new frame)");
        expect(f.take(a.data()), false, "keepalive: and the next repeat is dropped again");
        auto b = make_frame(300);
        expect(f.take(b.data()) && !f.keepalive(), true, "keepalive: a new frame after it is new again");
    }
    {
        // FrameSlot's drop counting, which the frame generation add-on backs off on.
        auto a = make_frame(100);
        GlasirFrameGenLink link{};
        FrameSlot slot(a.size());
        slot.set_link(&link);
        UINT64 published = 0, dropped = 0;
        slot.publish(a.data(), a.size(), true);
        slot.take(0);
        slot.publish(a.data(), a.size(), true);
        slot.counts(published, dropped);
        expect(dropped == 0 && link.dropped == 0, true, "slot: a frame taken before the next is not dropped");
        slot.publish(a.data(), a.size(), true);
        slot.counts(published, dropped);
        expect(dropped == 1 && link.dropped == 1, true, "slot: a new frame overwritten before mpv took it is dropped");
        slot.take(0);
        slot.publish(a.data(), a.size(), false);
        slot.publish(a.data(), a.size(), true);
        slot.counts(published, dropped);
        expect(dropped == 1, true, "slot: an overwritten keepalive repeat is not counted");
        expect(published == 4 && link.published == 4, true, "slot: keepalive repeats are not published as new");
        expect(link.stamps[3] >= link.stamps[0] && link.stamps[0] > 0, true, "slot: new frames are stamped in order");
        // A new frame (published last above) is waiting; a keepalive must not displace it.
        slot.publish(a.data(), a.size(), false);
        slot.counts(published, dropped);
        expect(dropped == 1 && link.dropped == 1, true, "slot: a keepalive behind a waiting new frame is no drop");
        slot.publish(a.data(), a.size(), true);
        slot.counts(published, dropped);
        expect(dropped == 2, true, "slot: and the waiting new frame still counts if a new one replaces it");
    }
    {
        // Repeats sent while frame generation is off (the add-on sets send_repeats).
        auto a = make_frame(100), b = make_frame(300);
        GlasirFrameGenLink link{};
        FrameSlot slot(a.size());
        slot.set_link(&link);
        RepeatFilter f(PixelFormat::P010, W, H, 2, 100000);
        UINT64 published = 0, dropped = 0;
        deliver_frame(slot, &f, a.data(), a.size());
        slot.take(0);
        deliver_frame(slot, &f, a.data(), a.size());
        expect(slot.take(0) != nullptr, false, "send repeats off: a repeat is not sent");
        link.send_repeats = 1;
        deliver_frame(slot, &f, a.data(), a.size());
        expect(slot.take(0) != nullptr, true, "send repeats on: a repeat is sent");
        slot.counts(published, dropped);
        expect(published == 1 && link.published == 1, true, "send repeats on: the repeat is not stamped as new");
        deliver_frame(slot, &f, b.data(), b.size());
        deliver_frame(slot, &f, b.data(), b.size());
        slot.counts(published, dropped);
        expect(published == 2 && dropped == 0, true, "send repeats on: a repeat does not displace a waiting new frame");
        slot.take(0);
        deliver_frame(slot, &f, b.data(), b.size());
        deliver_frame(slot, &f, a.data(), a.size());
        slot.counts(published, dropped);
        expect(published == 3 && dropped == 0, true, "send repeats on: a new frame replacing a waiting repeat is no drop");
        link.send_repeats = 0;
        slot.take(0);
        deliver_frame(slot, &f, a.data(), a.size());
        expect(slot.take(0) != nullptr, false, "send repeats off again: repeats are dropped again");
    }
    {
        // NV12 (SDR): 8-bit luma, compared in 10-bit code values.
        expect(frame_size(PixelFormat::NV12, W, H) == size_t(W) * H * 3 / 2 &&
                   frame_size(PixelFormat::P010, W, H) == size_t(W) * H * 3,
               true, "frame sizes: NV12 1.5 bytes a pixel, P010 3");
        auto nv12 = [](BYTE base) {
            std::vector<BYTE> f(frame_size(PixelFormat::NV12, W, H), 128);  // chroma stays grey
            for (size_t i = 0; i < size_t(W) * H; ++i) f[i] = static_cast<BYTE>(base + (i % 7));
            return f;
        };
        auto a = nv12(40), a1 = nv12(41), a2 = nv12(42), b = nv12(120);
        RepeatFilter f(PixelFormat::NV12, W, H, 2, 100000);
        expect(f.take(a.data()), true, "nv12 30-in-60: first A is sent");
        expect(f.take(a.data()), false, "nv12 30-in-60: repeated A is dropped");
        expect(f.take(b.data()), true, "nv12 30-in-60: B is sent");
        expect(f.take(b.data()), false, "nv12 30-in-60: repeated B is dropped");
        RepeatFilter g(PixelFormat::NV12, W, H, 2, 100000);
        g.take(a.data());
        expect(g.take(a1.data()), true, "nv12 threshold 2: one 8-bit step (4 codes) is new");
        RepeatFilter h(PixelFormat::NV12, W, H, 4, 100000);
        h.take(a.data());
        expect(h.take(a1.data()), false, "nv12 threshold 4: one 8-bit step is a repeat");
        expect(h.take(a2.data()), true, "nv12 threshold 4: two steps from the last sent go out");
        auto chroma = a;
        for (size_t i = size_t(W) * H; i < chroma.size(); ++i) chroma[i] = 200;
        RepeatFilter c(PixelFormat::NV12, W, H, 2, 100000);
        c.take(a.data());
        expect(c.take(chroma.data()), false, "nv12: the chroma plane is not compared (luma only)");
        auto spot = a;
        for (UINT32 i = 0; i < 4; ++i)
            for (UINT32 dy = 0; dy < 2; ++dy)
                for (UINT32 dx = 0; dx < 2; ++dx) spot[size_t(200 + dy) * W + 100 + 8 * i + dx] = 250;
        expect(c.take(spot.data()), true, "nv12: four small spots changed (4 samples) is new");
    }
    {
        // A source that is not bit-exact on repeats, like the PS5: every
        // refresh adds faint noise to a fraction of the pixels. Repeats must
        // still be dropped at the default threshold (3), and real changes -
        // a fade, a small HUD change - must still go out.
        UINT32 seed = 12345;
        auto rnd = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 8;  // 24 bits
        };
        // Per pixel: +-1 step on 15%, +-2 on 3%, +-3 on 0.5% (in the format's own steps).
        auto noise = [&rnd]() {
            const UINT32 r = rnd() % 1000, sign = rnd() & 1;
            const int m = r < 5 ? 3 : r < 35 ? 2 : r < 185 ? 1 : 0;
            return sign ? -m : m;
        };
        auto nv12 = [](int base) {
            std::vector<BYTE> f(frame_size(PixelFormat::NV12, W, H), 128);
            for (UINT32 y = 0; y < H; ++y)
                for (UINT32 x = 0; x < W; ++x) f[size_t(y) * W + x] = static_cast<BYTE>(base + (x * 7 + y * 13) % 64);
            return f;
        };
        auto noisy_nv12 = [&](const std::vector<BYTE> &clean) {
            auto f = clean;
            for (size_t i = 0; i < size_t(W) * H; ++i) f[i] = static_cast<BYTE>(std::clamp(f[i] + noise(), 0, 255));
            return f;
        };
        RepeatFilter f(PixelFormat::NV12, W, H, 3, 100000);
        int sent = 0, sent_new = 0;
        for (int k = 0; k < 20; ++k) {
            const auto clean = nv12(30 + k * 3);  // each game frame moves up 3 steps
            sent_new += f.take(noisy_nv12(clean).data());
            sent += f.take(noisy_nv12(clean).data());
        }
        expect(sent_new == 20 && sent == 0, true, "noisy nv12 30-in-60: every new frame sent, no repeat");

        RepeatFilter s(PixelFormat::NV12, W, H, 3, 100000);
        auto a = nv12(40);
        s.take(noisy_nv12(a).data());
        for (int k = 0; k < 30; ++k) {
            if (s.take(noisy_nv12(a).data())) {
                expect(true, false, "noisy nv12: 30 repeats of a still frame dropped");
                break;
            }
            if (k == 29) expect(true, true, "noisy nv12: 30 repeats of a still frame dropped");
        }
        auto hud = a;  // a digit-sized change: 8x12 pixels from dark to bright
        for (UINT32 y = 300; y < 312; ++y)
            for (UINT32 x = 500; x < 508; ++x) hud[size_t(y) * W + x] = 230;
        expect(s.take(noisy_nv12(hud).data()), true, "noisy nv12: a digit-sized HUD change is new");
        auto fade = nv12(41);  // the whole picture one 8-bit step brighter (back without the HUD change)
        s.take(noisy_nv12(a).data());
        expect(s.take(noisy_nv12(fade).data()), true, "noisy nv12: a fade of one 8-bit step is new");

        // P010: the noise in 10-bit steps (+-1..3, as the 8-bit noise above but finer).
        auto p010 = [](int base) {
            std::vector<BYTE> f(frame_size(PixelFormat::P010, W, H));
            auto *y10 = reinterpret_cast<UINT16 *>(f.data());
            for (UINT32 y = 0; y < H; ++y)
                for (UINT32 x = 0; x < W; ++x)
                    y10[size_t(y) * W + x] = static_cast<UINT16>((base + (x * 7 + y * 13) % 256) << 6);
            return f;
        };
        auto noisy_p010 = [&](const std::vector<BYTE> &clean) {
            auto f = clean;
            auto *y10 = reinterpret_cast<UINT16 *>(f.data());
            for (size_t i = 0; i < size_t(W) * H; ++i)
                y10[i] = static_cast<UINT16>(std::clamp((y10[i] >> 6) + 2 * noise(), 0, 1023) << 6);
            return f;
        };
        RepeatFilter p(PixelFormat::P010, W, H, 3, 100000);
        sent = sent_new = 0;
        for (int k = 0; k < 20; ++k) {
            const auto clean = p010(100 + k * 8);
            sent_new += p.take(noisy_p010(clean).data());
            sent += p.take(noisy_p010(clean).data());
        }
        expect(sent_new == 20 && sent == 0, true, "noisy p010 30-in-60 (noise up to 6): new frames sent, no repeat");
    }
    {
        // YUY2 (cheap USB cards): packed Y U Y V, luma in every other byte.
        expect(frame_size(PixelFormat::YUY2, W, H) == size_t(W) * H * 2, true, "frame sizes: YUY2 2 bytes a pixel");
        auto yuy2 = [](BYTE base, BYTE chroma) {
            std::vector<BYTE> f(frame_size(PixelFormat::YUY2, W, H));
            for (size_t i = 0; i < size_t(W) * H; ++i) {
                f[i * 2] = static_cast<BYTE>(base + (i % 7));
                f[i * 2 + 1] = chroma;
            }
            return f;
        };
        auto a = yuy2(40, 128), b = yuy2(120, 128), a_chroma = yuy2(40, 200);
        RepeatFilter f(PixelFormat::YUY2, W, H, 2, 100000);
        expect(f.take(a.data()), true, "yuy2 30-in-60: first A is sent");
        expect(f.take(a.data()), false, "yuy2 30-in-60: repeated A is dropped");
        expect(f.take(a_chroma.data()), false, "yuy2: chroma bytes are not compared");
        expect(f.take(b.data()), true, "yuy2 30-in-60: B is sent");
        std::vector<BYTE> uyvy(a.size());
        for (size_t i = 0; i + 1 < a.size(); i += 2) {
            uyvy[i] = a[i + 1];
            uyvy[i + 1] = a[i];
        }
        expect(luma10(PixelFormat::UYVY, uyvy.data(), 5) == luma10(PixelFormat::YUY2, a.data(), 5), true,
               "uyvy: luma read from the odd bytes");
    }
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
