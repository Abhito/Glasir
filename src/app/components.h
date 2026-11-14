// SPDX-License-Identifier: MIT
// The third-party programs Glasir runs on, downloaded on the user's PC from
// where their authors publish them (Glasir does not ship them):
//
//   mpv        github.com/zhongfly/mpv-winbuild (GPL)            engine\mpv.exe
//   ReShade    reshade.me, the build with add-on support          engine\dxgi.dll
//   DLSS 5     DLSS5-Feeder (jlrouzies-fr), RenoDX-DLSS5 and       engine\...
//              nvngx_dlssnr.dll (RHI's mirror, RankFTW/rhi-repo),
//              nvngx_dlss.dll (NVIDIA/DLSS), ReShade.fxh
//
// Each Glasir installs the versions it was tested with: components.json
// (built in) names every part's version, address and SHA-256, so a new
// release of one of them does not reach users before a Glasir tested with it
// does. A tested download that is no longer offered (mpv's builds are kept
// about a month) is replaced by the newest. `--setup --newest` installs the
// newest of everything, to try before tools/pin_components.py pins it.
//
// What was installed, from where, is kept in engine\components.ini.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glasir {

enum class Component { Mpv, ReShade, Dlss5 };

const wchar_t *component_name(Component c);
bool component_installed(const std::wstring &engine_dir, Component c);
// "name version" lines from components.ini, for the log.
std::vector<std::wstring> installed_versions(const std::wstring &engine_dir);

// One part in its tested version. The parts: mpv, reshade, and DLSS 5's
// feeder, renodx, dlssnr, dlss and reshade_fxh.
struct Pin {
    std::wstring version, url;
    std::string sha256;  // of the download
    uint64_t size = 0;  // bytes, 0 if not said (for the progress bar)
};
struct PinSet {
    std::vector<std::pair<std::string, Pin>> parts;
    const Pin *find(std::string_view part) const;
};
// Reads components.json: {"mpv": {"version", "url", "sha256", "size"}, ...}.
// False with error set for a part without a version, an https address or a
// SHA-256 of 64 hex digits; members that are not objects are skipped.
bool read_pins(const std::string &json, PinSet &pins, std::wstring &error);
// The set this Glasir was tested with (GLASIR_COMPONENTS=<file>: that
// components.json instead, to try a set before building it in).
const PinSet &tested_components();
// Whether the component, as components.ini records it, is the tested one
// (or the newest in place of a tested part that was no longer offered).
bool component_as_tested(const std::wstring &engine_dir, Component c);

// What setup is doing, for the window that shows it (any thread).
class SetupProgress {
public:
    struct State {
        std::wstring step;             // "Downloading mpv"
        int index = 0, count = 0;      // component index (from 1) of count
        uint64_t done = 0, total = 0;  // bytes of the current download (total 0: unknown)
    };
    void step(const std::wstring &text);
    void bytes(uint64_t done, uint64_t total);
    void set_index(int index, int count);
    State get() const;

private:
    mutable std::mutex m_;
    State s_;
};

enum class Versions { Tested, Newest };

// Downloads and installs the components into engine_dir. A component is
// replaced only once all of its files are downloaded and checked, so a
// failure or cancel leaves the one there before. False with error set (and
// the components done before it installed).
bool install_components(const std::wstring &engine_dir, const std::vector<Component> &which, Versions versions,
                        SetupProgress &progress, const std::atomic<bool> &cancel, std::wstring &error);

// The rhi-repo release tag rule: PREFIX-x.y.z; true with the version parts.
bool parse_release_tag(const std::wstring &tag, const std::wstring &prefix, int (&version)[3]);

}  // namespace glasir
