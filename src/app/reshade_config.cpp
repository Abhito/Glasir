// SPDX-License-Identifier: MIT
#include "reshade_config.h"

#include <windows.h>

#include "log.h"

namespace glasir {
namespace {

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

}  // namespace

bool reshade_installed(const std::wstring &engine_dir) {
    const DWORD a = GetFileAttributesW((engine_dir + L"\\dxgi.dll").c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void write_reshade_config(const std::wstring &engine_dir, const Settings &s) {
    const std::wstring ini = engine_dir + L"\\ReShade.ini";
    // ReShade's own defaults that do not suit a player: its first-run
    // tutorial banner, and debug information in compiled effects. (Effect
    // paths come with the DLSS 5 effects.)
    put_default(ini, L"OVERLAY", L"TutorialProgress", L"4");
    put_default(ini, L"GENERAL", L"PerformanceMode", L"1");
    put_default(ini, L"GENERAL", L"NoDebugInfo", L"1");

    // Frame generation follows glasir.ini on every start (F8 toggles it for
    // the session).
    put(ini, L"GLASIR_FRAMEGEN", L"Enabled", s.framegen ? L"1" : L"0");
    put(ini, L"GLASIR_FRAMEGEN", L"Quality", s.framegen_fast ? L"fast" : L"high");
    put(ini, L"GLASIR_FRAMEGEN", L"DedupWhenOff", s.all_frames_when_off ? L"0" : L"1");
    put(ini, L"GLASIR_FRAMEGEN", L"MpvPipe", L"glasir");
}

}  // namespace glasir
