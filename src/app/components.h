// SPDX-License-Identifier: MIT
// The third-party programs Glasir runs on, downloaded on the user's PC from
// where their authors publish them (Glasir does not ship them):
//
//   mpv        github.com/zhongfly/mpv-winbuild (GPL)            engine\mpv.exe
//   ReShade    reshade.me, the build with add-on support          engine\dxgi.dll
//   DLSS 5     DLSS5-Feeder (jlrouzies-fr), RenoDX-DLSS5 and       engine\...
//              nvngx_dlssnr.dll (RHI's mirror, RankFTW/rhi-repo),
//              nvngx_dlss.dll (NVIDIA/DLSS), LumeniteFX, ReShade.fxh
//
// What was installed, from where, is kept in engine\components.ini.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace glasir {

enum class Component { Mpv, ReShade, Dlss5 };

const wchar_t *component_name(Component c);
bool component_installed(const std::wstring &engine_dir, Component c);
// "name version" lines from components.ini, for the log.
std::vector<std::wstring> installed_versions(const std::wstring &engine_dir);

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

// Downloads and installs the components into engine_dir. A component is
// replaced only once all of its files are downloaded and checked, so a
// failure or cancel leaves the one there before. False with error set (and
// the components done before it installed).
bool install_components(const std::wstring &engine_dir, const std::vector<Component> &which, SetupProgress &progress,
                        const std::atomic<bool> &cancel, std::wstring &error);

// The rhi-repo release tag rule: PREFIX-x.y.z; true with the version parts.
bool parse_release_tag(const std::wstring &tag, const std::wstring &prefix, int (&version)[3]);

}  // namespace glasir
