// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// raw_pace - passes raw video frames from standard input to standard output
// at a steady rate, the way a capture card delivers them. ffmpeg's -re paces
// in uneven steps (31/31/31/46 ms at 30 fps), and with VRR the screen shows
// that unevenness directly; this keeps each frame to its slot to within a
// fraction of a millisecond.
//
//   ffmpeg ... -f rawvideo - | raw_pace.exe FRAME_BYTES FPS | Glasir.exe --input-raw - ...

#include <windows.h>
#include <timeapi.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace {

double now_s() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}

// Sleep most of the way with a high-resolution timer, spin the rest.
void wait_until(double target, HANDLE timer) {
    for (;;) {
        const double left = target - now_s();
        if (left <= 0) return;
        if (left > 0.002 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -LONGLONG((left - 0.0015) * 1e7);
            SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0);
            WaitForSingleObject(timer, INFINITE);
        } else {
            YieldProcessor();
        }
    }
}

bool read_all(void *dst, size_t n) {
    char *p = static_cast<char *>(dst);
    while (n > 0) {
        const size_t got = fread(p, 1, n, stdin);
        if (got == 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: raw_pace FRAME_BYTES FPS\n");
        return 2;
    }
    const size_t bytes = size_t(_strtoui64(argv[1], nullptr, 10));
    const double period = 1.0 / atof(argv[2]);
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    setvbuf(stdout, nullptr, _IONBF, 0);
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    std::vector<char> frame(bytes);
    double next = 0;
    long long sent = 0, late = 0;
    while (read_all(frame.data(), bytes)) {
        if (sent == 0) next = now_s();
        wait_until(next, timer);
        if (now_s() - next > period * 0.5) {
            ++late;              // the reader or the source fell behind:
            next = now_s();      // start the slots again from here
        }
        if (fwrite(frame.data(), 1, bytes, stdout) != bytes) break;
        ++sent;
        next += period;
    }
    fprintf(stderr, "raw_pace: %lld frames, %lld started late\n", sent, late);
    return 0;
}
