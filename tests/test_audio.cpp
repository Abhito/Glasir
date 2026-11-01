// SPDX-License-Identifier: MIT
// Checks the audio plugin's sample conversion and queue (the conversion and
// queue cases come from z-er's audio_core_test.cpp), the delay and gain, and
// finding the capture card's audio input.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../src/audio/audio_core.h"
#include "audio_endpoint.h"

using namespace glasir;
using namespace glasir::audio;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    unsigned char bytes[4];
    bool round_trip = true;
    for (unsigned bits : {16u, 24u, 32u})
        for (float value : {-1.f, -.5f, 0.f, .5f, 1.f}) {
            encode(bytes, value, bits, false);
            round_trip = round_trip && std::abs(decode(bytes, bits, false) - value) < .00004;
        }
    expect(round_trip, "PCM 16/24/32 round trip");
    encode(bytes, 5, 32, true);
    expect(decode(bytes, 32, true) == 1, "float output clipped to 1");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(bytes, &nan, 4);
    expect(decode(bytes, 32, true) == 0, "NaN input reads as silence");

    Ring ring(4);
    Stereo v;
    expect(!ring.read(1, v), "empty queue: no sample");
    for (int n = 0; n < 5; ++n) ring.push({float(n), float(-n)});
    expect(ring.size() == 4 && ring.dropped == 1, "full queue drops the oldest");
    expect(ring.read(.5, v) && v.left == 1 && v.right == -1, "read at half rate: first sample");
    expect(ring.read(.5, v) && v.left == 1.5f && v.right == -1.5f, "read at half rate: interpolated");
    expect(ring.read(2, v) && v.left == 2, "read at double rate");
    expect(!ring.read(1, v), "underrun when too few are queued");
    ring.trim(0);
    expect(ring.size() == 0, "trim to empty");

    Ring rate(48000);
    for (int n = 0; n < 48000; ++n) rate.push({.25f, -.25f});
    int outputs = 0;
    bool steady = true;
    while (rate.read(48000.0 / 44100, v)) {
        steady = steady && std::abs(v.left - .25) < .00001;
        ++outputs;
    }
    expect(steady && outputs >= 44098 && outputs <= 44100, "48 kHz -> 44.1 kHz: one second in, one second out");

    expect(queue_target(48000, 960, 0) == 960, "delay 0: the base queue");
    expect(queue_target(48000, 960, 100) == 960 + 4800, "delay 100 ms at 48 kHz: 4800 more samples");
    expect(queue_target(48000, 960, -5) == 960, "negative delay counts as 0");
    expect(output_gain(100, 100, false) == 1.0, "gain: 100 % volume, no boost");
    expect(std::abs(output_gain(60, 200, false) - 1.2) < 1e-9, "gain: 60 % volume, 200 % boost");
    expect(output_gain(100, 900, false) == 4.0, "gain: boost capped at 400 %");
    expect(output_gain(150, 100, false) == 1.0, "gain: volume capped at 100 %");
    expect(output_gain(100, 400, true) == 0.0, "gain: muted");

    expect(card_base_name(L"Live Gamer Ultra 2.1-Video") == L"Live Gamer Ultra 2.1", "card name without -Video");
    expect(card_base_name(L"Game Capture HD60 X (Video)") == L"Game Capture HD60 X", "card name without (Video)");
    expect(card_base_name(L"USB Video") == L"USB", "card name without Video");

    const GUID usb_card = {0x1234, 0x5678, 0x9abc, {1, 2, 3, 4, 5, 6, 7, 8}};
    const GUID headset = {0x9999, 0x5678, 0x9abc, {1, 2, 3, 4, 5, 6, 7, 8}};
    const GUID builtin = {0, 0, 0, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    const std::vector<AudioInput> inputs = {
        {L"mic", L"Microphone (USB Headset)", headset},
        {L"usb", L"Digital Audio Interface (USB Digital Audio)", usb_card},
        {L"lgu", L"HDMI/Line In (Live Gamer Ultra 2.1-Audio)", builtin},
        {L"elg", L"Game Capture 4K60 Pro MK.2 Audio", builtin},
    };
    auto pick = [&](const wchar_t *video, std::optional<GUID> container, const wchar_t *wanted) {
        const auto a = find_card_audio(inputs, video, container, wanted);
        return a ? a->id : std::wstring(L"none");
    };
    expect(pick(L"USB Video", usb_card, L"") == L"usb", "USB card: its audio by container, despite the name");
    expect(pick(L"USB Video", std::nullopt, L"") == L"none", "USB card without container: not the headset mic");
    expect(pick(L"Live Gamer Ultra 2.1-Video", builtin, L"") == L"lgu", "built-in container: by name");
    expect(pick(L"Game Capture 4K60 Pro MK.2", builtin, L"") == L"elg", "PCIe card: by name");
    expect(pick(L"Live Gamer Ultra 2.1-Video", builtin, L"Headset") == L"mic", "glasir.ini names another input");
    expect(pick(L"Live Gamer Ultra 2.1-Video", builtin, L"No Such Input") == L"none", "glasir.ini input missing");

    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
