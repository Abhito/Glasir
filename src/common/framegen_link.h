// SPDX-License-Identifier: MIT
// What Glasir's capture tells the frame generation add-on inside mpv.
//
// The capture side creates this block (with dedup on) under a name built from
// mpv's process id, so it belongs to that one mpv and nothing it starts. The
// add-on finds it by its own process id. Its presence means "mpv gets new
// frames only", and it carries what mpv cannot see from inside: when each new
// frame really arrived from the card, and how many new frames were dropped
// because mpv was still busy with the one before.
//
// One field goes the other way: while frame generation is off, the add-on
// asks for the repeats too, and mpv gets every frame as without dedup.
//
// The add-on needs both because it paces mpv itself. While it generates, mpv's
// render thread is held for two presents per real frame, so the gaps the
// add-on measures between mpv's frames show its own pace, not the card's: on a
// display too slow for twice the source rate it would keep measuring a rate
// that fits, and keep generating, while frames are dropped upstream.

#pragma once
#include <windows.h>

#define GLASIR_FRAMEGEN_LINK_NAME L"Local\\Glasir_FrameGen_%lu"  // %lu = mpv's process id
#define GLASIR_FRAMEGEN_LINK_MAGIC 0x46534C47u                  // 'GLSF'
#define GLASIR_FRAMEGEN_LINK_VERSION 1u

struct GlasirFrameGenLink {
    static constexpr int kStamps = 16;

    UINT32 magic;
    UINT32 version;
    // New frames from the source, ready for mpv (some may then be dropped,
    // below). The writer stores the frame's QueryPerformanceCounter value in
    // stamps[published % kStamps] first, then increments.
    volatile LONG64 published;
    // New frames overwritten before mpv took them: mpv fell behind.
    volatile LONG64 dropped;
    // Written by the add-on: nonzero while frame generation is off. Repeats
    // are then sent too (still told apart: only new frames are stamped).
    volatile LONG send_repeats;
    volatile LONG64 stamps[kStamps];
};
