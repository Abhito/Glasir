// SPDX-License-Identifier: MIT
// Finding the capture card's own audio input.
#pragma once
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace glasir {

struct AudioInput {
    std::wstring id;    // WASAPI endpoint ID
    std::wstring name;  // e.g. "HDMI/Line In (Live Gamer Ultra 2.1-Audio)"
    GUID container{};   // the physical device it belongs to
};

// Active WASAPI capture endpoints.
std::vector<AudioInput> list_audio_inputs();

// The physical device a video capture device belongs to (from its symbolic
// link), or nullopt.
std::optional<GUID> video_container(const std::wstring &symbolic_link);

// The card's audio input: one on the same physical device (USB cards), else
// one whose name contains the card's name without "Video" (PCIe cards share
// one container with everything built in). wanted (glasir.ini) overrides:
// an exact name or ID, else part of a name.
std::optional<AudioInput> find_card_audio(const std::vector<AudioInput> &inputs, const std::wstring &video_name,
                                          const std::optional<GUID> &container, const std::wstring &wanted);

// "Live Gamer Ultra 2.1-Video" -> "Live Gamer Ultra 2.1"
std::wstring card_base_name(const std::wstring &video_name);

}  // namespace glasir
