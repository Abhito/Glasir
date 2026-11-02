// SPDX-License-Identifier: MIT
#include "reshade_config.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#include "components.h"
#include "log.h"

namespace glasir {
namespace {

bool exists(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool has_key(const std::wstring &ini, const wchar_t *section, const wchar_t *key) {
    wchar_t value[8];
    // A default no real value has, to tell "missing" from "empty".
    GetPrivateProfileStringW(section, key, L"\x01", value, 8, ini.c_str());
    return value[0] != L'\x01';
}

void put(const std::wstring &ini, const wchar_t *section, const wchar_t *key, const std::wstring &value) {
    if (!WritePrivateProfileStringW(section, key, value.c_str(), ini.c_str()))
        log(L"could not write [{}] {} to {} (error {})", section, key, ini, GetLastError());
}

// Written once; after that the value is the user's (or ReShade's).
void put_default(const std::wstring &ini, const wchar_t *section, const wchar_t *key, const std::wstring &value) {
    if (!has_key(ini, section, key)) put(ini, section, key, value);
}

// Makes sure a comma-separated list value has an entry with needle in it:
// if not, entry goes first and what was there (other than ReShade's bare
// default ".\") stays after it.
void put_in_list(const std::wstring &ini, const wchar_t *section, const wchar_t *key, const std::wstring &needle,
                 const std::wstring &entry) {
    wchar_t value[2048] = {};
    GetPrivateProfileStringW(section, key, L"", value, 2048, ini.c_str());
    const std::wstring current = value;
    if (current.find(needle) != std::wstring::npos) return;
    put(ini, section, key, current.empty() || current == L".\\" ? entry : entry + L"," + current);
}

// Sets key=value in a file of plain "key=value" lines before any [section]
// (dlss5-feed.cfg, and the global part of ReShadePreset.ini, which the INI
// functions cannot reach), keeping every other line.
void put_plain(const std::wstring &path, const std::string &key, const std::string &value) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    size_t insert_at = 0;
    bool done = false;
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        if (!lines[i].empty() && lines[i][0] == '[') break;  // the global part ends here
        insert_at = i + 1;
        if (lines[i].compare(0, key.size() + 1, key + "=") == 0) {
            lines[i] = key + "=" + value;
            done = true;
        }
    }
    if (!done) lines.insert(lines.begin() + std::min(insert_at, lines.size()), key + "=" + value);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const auto &l : lines) out << l << "\r\n";
    if (!out) log(L"could not write {}", path);
}

constexpr char kDlss5Techniques[] = "Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx";

}  // namespace

bool reshade_installed(const std::wstring &engine_dir) { return exists(engine_dir + L"\\dxgi.dll"); }

bool dlss5_installed(const std::wstring &engine_dir) { return component_installed(engine_dir, Component::Dlss5); }

void write_reshade_config(const std::wstring &engine_dir, const Settings &s, bool dlss5_loaded, bool dlss5_on) {
    const std::wstring ini = engine_dir + L"\\ReShade.ini";
    // ReShade's own defaults that do not suit a player: its first-run
    // tutorial banner, and debug information in compiled effects.
    put_default(ini, L"OVERLAY", L"TutorialProgress", L"4");
    put_default(ini, L"GENERAL", L"PerformanceMode", L"1");
    put_default(ini, L"GENERAL", L"NoDebugInfo", L"1");

    // Frame generation follows glasir.ini on every start (F8 toggles it for
    // the session).
    put(ini, L"GLASIR_FRAMEGEN", L"Enabled", s.framegen ? L"1" : L"0");
    put(ini, L"GLASIR_FRAMEGEN", L"Quality", s.framegen_fast ? L"fast" : L"high");
    put(ini, L"GLASIR_FRAMEGEN", L"DedupWhenOff", s.all_frames_when_off ? L"0" : L"1");
    put(ini, L"GLASIR_FRAMEGEN", L"MpvPipe", L"glasir");

    if (!dlss5_installed(engine_dir)) return;
    // DLSS 5: DLSS5_Feed.fx hands the picture to the DLSS5-Feeder add-on,
    // which runs DLSS on it; RenoDX-DLSS5 turns that into the neural
    // rendering pass. LumeniteFX's Kernel gives the shader its motion vectors.
    put_in_list(ini, L"GENERAL", L"EffectSearchPaths", L"reshade-shaders\\Shaders", L".\\reshade-shaders\\Shaders\\**");
    put_in_list(ini, L"GENERAL", L"TextureSearchPaths", L"reshade-shaders\\Textures",
                L".\\reshade-shaders\\Textures\\**");
    put_in_list(ini, L"GENERAL", L"PreprocessorDefinitions", L"DLSS5_MV_PROVIDER=", L"DLSS5_MV_PROVIDER=3");
    put_default(ini, L"GENERAL", L"PresetPath", L".\\ReShadePreset.ini");
    // On every start: RenoDX and the Feeder work whenever DLSS 5 is loaded;
    // whether it shows is the effects' state (the menu switches them; RenoDX's
    // own key, F6, toggles its part for the session).
    put(ini, L"RenoDX.DLSS5", L"NeuralUplift", dlss5_loaded ? L"1" : L"0");
    // RenoDX's screenshot key (F5): its Auto source (0, its default) takes
    // the presented frame, which here is mpv's window, not the picture DLSS 5
    // made (the Feeder runs it on a device of its own), so F5 saved nothing
    // ("this presentation window does not carry the enhanced game image").
    // The evaluated model buffers (2) are DLSS 5's input and output. Auto
    // is replaced on every start; any other choice is the user's.
    wchar_t source[8] = {};
    GetPrivateProfileStringW(L"RenoDX.DLSS5", L"NRScreenshotSource", L"0", source, 8, ini.c_str());
    if (std::wstring(source) == L"0") put(ini, L"RenoDX.DLSS5", L"NRScreenshotSource", L"2");
    const std::wstring preset = engine_dir + L"\\ReShadePreset.ini";
    put_plain(preset, "Techniques", dlss5_loaded && dlss5_on ? kDlss5Techniques : "");
    put_plain(preset, "TechniqueSorting", kDlss5Techniques);
    // The Feeder writes its file with working defaults on its first start;
    // Glasir only switches it (and makes one only to keep it off).
    const std::wstring feeder = engine_dir + L"\\dlss5-feed.cfg";
    if (exists(feeder) || !dlss5_loaded) put_plain(feeder, "enabled", dlss5_loaded ? "1" : "0");
}

}  // namespace glasir
