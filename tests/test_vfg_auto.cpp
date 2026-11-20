// SPDX-License-Identifier: MIT
// VfgQuality=auto (src/framegen/fg_vfg_auto.h): which of NVIDIA Video Frame
// Generation's models runs, on made-up loads. The times here are invented for
// the test - a small, a middling and a large cost - and say nothing about
// NVIDIA's software.

#include <cstdio>

#include "fg_vfg_auto.h"

namespace {

int g_failed = 0;

void expect(bool ok, const char *what) {
    if (ok) return;
    ++g_failed;
    std::printf("FAILED: %s\n", what);
}

// A source and a GPU: every real frame takes `base` and the model's own cost, and does not keep up
// (strained, and mpv drops a frame now and then) when that is more than the interval.
struct Run {
    fg::VfgAuto pick;
    double now = 0;
    double cost[3] = {0.001, 0.003, 0.014};
    int changes = 0, lowered = 0;

    void play(double seconds, double fps, double base, bool neural) {
        const double interval = 1.0 / fps;
        const double until = now + seconds;
        int late = 0;
        while (now < until) {
            now += interval;
            const double busy = base + cost[int(pick.mode())];
            const bool over = busy > interval;
            const fg::VfgMode before = pick.mode();
            bool changed = false;
            if (over && ++late % 4 == 0)
                changed = pick.dropped(now, interval, neural);
            else
                changed = pick.frame(now, interval, busy, over, neural);
            if (changed) {
                ++changes;
                if (int(pick.mode()) < int(before)) ++lowered;
            }
        }
    }
};

}  // namespace

int main() {
    using fg::VfgMode;
    {
        // A 30 fps game, little else for the GPU to do: up to the largest, and it stays there.
        Run r;
        r.pick.reset(VfgMode::medium);
        r.play(20, 30, 0.004, false);
        expect(r.pick.mode() == VfgMode::high && r.changes == 1 && r.pick.why() == fg::VfgAuto::Why::room,
               "30 fps with time to spare: the largest model, taken once");
        r.play(600, 30, 0.004, false);
        expect(r.pick.mode() == VfgMode::high && r.changes == 1, "and kept");
        // The game goes to 60 fps: the largest no longer fits.
        r.play(5, 60, 0.004, false);
        expect(r.pick.mode() == VfgMode::medium && r.lowered == 1, "60 fps: down to the one that keeps up, within seconds");
        const int before = r.changes;
        r.play(1800, 60, 0.004, false);
        expect(r.pick.mode() == VfgMode::medium && r.changes - before <= 2 * 4,
               "and it does not keep trying the one that does not fit (a few tries in half an hour at most)");
        // Back to 30 fps: the largest again, without waiting out what 60 fps taught.
        r.play(30, 30, 0.004, false);
        expect(r.pick.mode() == VfgMode::high, "30 fps again: the largest again, soon");
    }
    {
        // 60 fps with DLSS 5 taking most of the interval: the smallest.
        Run r;
        r.pick.reset(VfgMode::medium);
        r.play(10, 60, 0.0145, true);
        expect(r.pick.mode() == VfgMode::low, "a full GPU: the smallest model");
        const int before = r.changes;
        r.play(900, 60, 0.0145, true);
        expect(r.pick.mode() == VfgMode::low && r.changes == before, "and no tries while there is no room");
        // DLSS 5 switched off: room again.
        r.play(30, 60, 0.004, false);
        expect(r.pick.mode() == VfgMode::medium, "DLSS 5 off: a larger one, soon");
    }
    {
        // A long overload at the smallest model, then the game goes to 30 fps with DLSS 5 still on: a larger
        // model again soon, not after the overload has been worked off frame by frame.
        Run r;
        r.pick.reset(VfgMode::medium);
        r.play(900, 60, 0.0155, true);
        expect(r.pick.mode() == VfgMode::low, "overloaded for a quarter of an hour: the smallest");
        r.play(15, 30, 0.0155, true);
        expect(r.pick.mode() == VfgMode::medium, "30 fps afterwards, DLSS 5 still on: up again within seconds");
    }
    {
        // A hitch (a few late frames of mpv's own) steps down, but does not mark the model as too large.
        Run r;
        r.pick.reset(VfgMode::medium);
        r.play(20, 30, 0.004, false);
        expect(r.pick.mode() == VfgMode::high, "up, as before");
        for (int i = 0; i < 6; ++i) {
            r.now += 1.0 / 30;
            r.pick.frame(r.now, 1.0 / 30, 0.2, true, false);
        }
        expect(r.pick.mode() == VfgMode::medium, "six frames left out in a row: one down");
        r.play(90, 30, 0.004, false);
        expect(r.pick.mode() == VfgMode::high, "and up again after the wait, not after a quarter of an hour");
    }
    {
        // Frames without a frame in between (generation paused, settling) say nothing.
        Run r;
        r.pick.reset(VfgMode::medium);
        double t = 0;
        bool changed = false;
        for (int i = 0; i < 30 * 600; ++i) changed = r.pick.frame(t += 1.0 / 30, 1.0 / 30, -1, false, false) || changed;
        expect(!changed && r.pick.mode() == VfgMode::medium, "no frames in between: no change, whatever the time");
    }
    {
        // A smaller model that cannot be made: not stepped down to; the caller pauses instead.
        fg::VfgAuto q;
        q.reset(VfgMode::low);
        q.failed(VfgMode::medium);  // low could not be made: medium goes on
        expect(q.mode() == VfgMode::medium && !q.has_smaller() && !q.dropped(10.0, 1.0 / 60, false),
               "the smaller model could not be made: nothing to step down to, so the caller pauses");
    }
    {
        // Nothing fits, not even the smallest: it stays there and says so (the add-on pauses as it does anyway).
        Run r;
        r.pick.reset(VfgMode::high);
        r.play(10, 60, 0.020, true);
        expect(r.pick.mode() == VfgMode::low, "an overloaded GPU: the smallest");
        expect(!r.pick.dropped(r.now, 1.0 / 60, true), "nothing smaller to take: the caller is told");
    }
    {
        // The largest takes nearly the whole interval without ever going over it: too tight to keep.
        Run r;
        r.pick.reset(VfgMode::high);
        r.play(3, 60, 0.0016, false);
        expect(r.pick.mode() == VfgMode::medium && r.lowered == 1, "a model that only just fits is not kept");
        r.play(600, 60, 0.0016, false);
        expect(r.pick.mode() == VfgMode::medium && r.changes == 1, "nor tried again while nothing changes");
    }
    {
        // A single late frame (a shader being compiled) changes nothing.
        Run r;
        r.pick.reset(VfgMode::medium);
        r.play(10, 60, 0.004, false);
        const bool changed = r.pick.frame(r.now + 0.2, 1.0 / 60, 0.2, true, false);
        r.now += 0.2;
        r.play(10, 60, 0.004, false);
        expect(!changed && r.pick.mode() == VfgMode::medium && r.changes == 0, "one late frame is not a load");
    }
    {
        // A model that cannot be made (no video memory left for it) is not tried again.
        Run r;
        r.pick.reset(VfgMode::medium);
        r.play(20, 30, 0.004, false);
        expect(r.pick.mode() == VfgMode::high, "up, as before");
        r.pick.failed(VfgMode::medium);
        r.play(600, 30, 0.004, false);
        expect(r.pick.mode() == VfgMode::medium && r.changes == 1, "a model that could not be made is left alone");
    }
    std::printf(g_failed ? "%d failed\n" : "vfg auto: all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
