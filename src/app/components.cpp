// SPDX-License-Identifier: MIT
#include "components.h"

#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>

#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <regex>

#include "http.h"
#include "json.h"
#include "log.h"
#include "mpv_launch.h"

namespace fs = std::filesystem;

namespace glasir {
namespace {

// ---------------------------------------------------------------- checks

bool exists(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string sha256_file(const std::wstring &path) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> buffer(1 << 20);
        bool ok = bool(in);
        while (ok && in) {
            in.read(buffer.data(), std::streamsize(buffer.size()));
            if (in.gcount() > 0)
                ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(in.gcount()), 0) == 0;
        }
        UCHAR digest[32];
        if (ok && in.eof() && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
            for (UCHAR b : digest) hex += std::format("{:02x}", b);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

// The file has a valid Authenticode signature whose signer's name contains
// signer (e.g. "NVIDIA Corporation").
bool signed_by(const std::wstring &path, const std::wstring &signer, std::wstring &who) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG trust = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    bool ok = false;
    if (trust == ERROR_SUCCESS) {
        if (CRYPT_PROVIDER_DATA *provider = WTHelperProvDataFromStateData(data.hWVTStateData))
            if (CRYPT_PROVIDER_SGNR *sgnr = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0))
                if (sgnr->csCertChain && sgnr->pasCertChain && sgnr->pasCertChain[0].pCert) {
                    wchar_t name[256] = {};
                    CertGetNameStringW(sgnr->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                                       DWORD(std::size(name)));
                    who = name;
                    ok = who.find(signer) != std::wstring::npos;
                }
    } else {
        who = std::format(L"not validly signed (0x{:08X})", static_cast<unsigned long>(trust));
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return ok;
}

std::wstring product_name(const std::wstring &path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return L"";
    std::vector<BYTE> info(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, info.data())) return L"";
    struct Translation {
        WORD language, codepage;
    } *t = nullptr;
    UINT n = 0;
    if (!VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&t), &n) ||
        n < sizeof(Translation))
        return L"";
    const std::wstring key = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\ProductName", t->language, t->codepage);
    wchar_t *value = nullptr;
    if (!VerQueryValueW(info.data(), key.c_str(), reinterpret_cast<void **>(&value), &n) || !n) return L"";
    return value;
}

// ---------------------------------------------------------------- files

// A new folder under %TEMP% for one setup run, removed afterwards.
class WorkDir {
public:
    WorkDir() {
        wchar_t temp[MAX_PATH + 1] = {};
        GetTempPathW(MAX_PATH + 1, temp);
        path_ = std::format(L"{}glasir-setup-{}-{}", temp, GetCurrentProcessId(), GetTickCount64());
        std::error_code ec;
        fs::create_directories(path_, ec);
    }
    ~WorkDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const std::wstring &path() const { return path_; }

private:
    std::wstring path_;
};

// Unpacks a zip or 7z with Windows' tar (libarchive). members: only these.
bool unpack(const std::wstring &archive, const std::wstring &folder, const std::vector<std::wstring> &members,
            std::wstring &error) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring tar = std::wstring(system) + L"\\tar.exe";
    std::vector<std::wstring> args = {L"-xf", archive, L"-C", folder};
    args.insert(args.end(), members.begin(), members.end());
    std::wstring command = join_command_line(tar, args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi)) {
        error = std::format(L"could not run {} (error {})", tar, GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 5 * 60 * 1000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        error = std::format(L"could not unpack {} (tar exit code {})", fs::path(archive).filename().wstring(), code);
        return false;
    }
    return true;
}

// The first file called name anywhere under folder, or "".
std::wstring find_file(const std::wstring &folder, const std::wstring &name) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && _wcsicmp(it->path().filename().c_str(), name.c_str()) == 0)
            return it->path().wstring();
    return L"";
}

// Puts file at dest, replacing what is there. The copy is made beside it and
// renamed over, so dest is never half written.
bool place(const std::wstring &file, const std::wstring &dest, std::wstring &error) {
    std::error_code ec;
    fs::create_directories(fs::path(dest).parent_path(), ec);
    const std::wstring part = dest + L".new";
    if (!CopyFileW(file.c_str(), part.c_str(), FALSE) ||
        !MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        DeleteFileW(part.c_str());
        error = e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED
                    ? L"could not replace " + dest + L": it is in use or read-only (close mpv and Glasir)"
                    : std::format(L"could not write {} (error {})", dest, e);
        return false;
    }
    return true;
}

// Replaces folder dest with a copy of folder src.
bool place_folder(const std::wstring &src, const std::wstring &dest, std::wstring &error) {
    std::error_code ec;
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    fs::copy(src, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = L"could not copy to " + dest + L": " + widen(ec.message());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- downloads

struct Asset {
    std::wstring name, url;
    uint64_t size = 0;
    std::string sha256;  // as published, "" if not
};

class Downloader {
public:
    Downloader(SetupProgress &progress, const std::atomic<bool> &cancel) : progress_(progress), cancel_(cancel) {}

    std::optional<Json> json(const std::wstring &url, std::wstring &error) {
        HttpRequest r;
        r.url = url;
        r.headers = {{L"Accept", L"application/vnd.github+json"}, {L"X-GitHub-Api-Version", L"2022-11-28"}};
        r.cancel = &cancel_;
        std::string body;
        if (!http_get(r, body, error)) return std::nullopt;
        auto j = Json::parse(body);
        if (!j) error = L"unexpected answer from " + url;
        return j;
    }

    bool get(const std::wstring &url, const std::vector<std::pair<std::wstring, std::wstring>> &headers,
             std::string &body, std::wstring &error) {
        HttpRequest r;
        r.url = url;
        r.headers = headers;
        r.cancel = &cancel_;
        return http_get(r, body, error);
    }

    bool file(const std::wstring &label, const std::wstring &url, const std::wstring &dest,
              const std::vector<std::pair<std::wstring, std::wstring>> &headers, uint64_t size_hint,
              std::wstring &error) {
        progress_.step(L"Downloading " + label);
        HttpRequest r;
        r.url = url;
        r.headers = headers;
        r.cancel = &cancel_;
        r.progress = [&](uint64_t done, uint64_t total) { progress_.bytes(done, total ? total : size_hint); };
        if (!http_download(r, dest, error)) {
            error = label + L": " + error;
            return false;
        }
        log(L"downloaded {} ({} bytes)", url, fs::file_size(dest));
        return true;
    }

    // A release asset, checked against the SHA-256 GitHub publishes for it.
    bool asset(const std::wstring &label, const Asset &a, const std::wstring &dest, std::wstring &error) {
        if (!file(label, a.url, dest, {}, a.size, error)) return false;
        if (a.sha256.empty()) {
            log(L"{}: no SHA-256 published for {}; not checked", label, a.name);
            return true;
        }
        const std::string got = sha256_file(dest);
        if (got != a.sha256) {
            error = label + L": the download does not match its published SHA-256 (" + widen(got) + L")";
            return false;
        }
        log(L"{}: SHA-256 {} matches", a.name, widen(got));
        return true;
    }

    bool cancelled(std::wstring &error) const {
        if (cancel_.load()) error = L"cancelled";
        return cancel_.load();
    }

private:
    SetupProgress &progress_;
    const std::atomic<bool> &cancel_;
};

Asset to_asset(const Json &a) {
    Asset out;
    out.name = widen(a.str("name"));
    out.url = widen(a.str("browser_download_url"));
    if (const Json *size = a.get("size")) out.size = uint64_t(size->number());
    const std::string &digest = a.str("digest");
    if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64) out.sha256 = digest.substr(7);
    return out;
}

// The release's first asset whose name matches pattern.
std::optional<Asset> find_asset(const Json &release, const std::wregex &pattern) {
    if (const Json *assets = release.get("assets"))
        for (const Json &a : assets->items())
            if (std::regex_match(widen(a.str("name")), pattern)) return to_asset(a);
    return std::nullopt;
}

const std::wstring kGitHubApi = L"https://api.github.com/repos/";

// ---------------------------------------------------------------- records

std::wstring today() {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    wchar_t text[16];
    wcsftime(text, std::size(text), L"%Y-%m-%d", &local);
    return text;
}

class Record {
public:
    explicit Record(const std::wstring &engine_dir) : ini_(engine_dir + L"\\components.ini") {}
    void set(const wchar_t *section, const wchar_t *key, const std::wstring &value) {
        WritePrivateProfileStringW(section, key, value.c_str(), ini_.c_str());
    }
    void clear(const wchar_t *section) { WritePrivateProfileStringW(section, nullptr, nullptr, ini_.c_str()); }

private:
    std::wstring ini_;
};

// ---------------------------------------------------------------- mpv

bool install_mpv(const std::wstring &engine, const std::wstring &work, Downloader &dl, std::wstring &error) {
    const auto release = dl.json(kGitHubApi + L"zhongfly/mpv-winbuild/releases/latest", error);
    if (!release) return error = L"mpv: " + error, false;
    // The plain x86_64 build: not x86_64-v3 (needs AVX2), not libmpv.
    const auto asset = find_asset(*release, std::wregex(LR"(mpv-x86_64-\d{8}-git-[0-9a-f]+\.7z)"));
    if (!asset) return error = L"mpv: no x86_64 build in release " + widen(release->str("tag_name")), false;
    const std::wstring archive = work + L"\\" + asset->name;
    if (!dl.asset(L"mpv", *asset, archive, error)) return false;
    if (dl.cancelled(error)) return false;
    const std::wstring unpacked = work + L"\\mpv";
    if (!unpack(archive, unpacked, {}, error)) return error = L"mpv: " + error, false;
    const std::wstring exe = find_file(unpacked, L"mpv.exe");
    if (exe.empty()) return error = L"mpv: no mpv.exe in " + asset->name, false;
    if (!place(exe, engine + L"\\mpv.exe", error)) return false;

    Record record(engine);
    record.clear(L"mpv");
    const std::wstring version = std::regex_replace(asset->name, std::wregex(LR"(^mpv-x86_64-(.+)\.7z$)"), L"$1");
    record.set(L"mpv", L"version", version);
    record.set(L"mpv", L"source", asset->url);
    record.set(L"mpv", L"sha256", widen(sha256_file(archive)));
    record.set(L"mpv", L"installed", today());
    log(L"installed mpv {}", version);
    return true;
}

// ---------------------------------------------------------------- ReShade

bool install_reshade(const std::wstring &engine, const std::wstring &work, Downloader &dl, std::wstring &error) {
    // reshade.me links "ReShade with full add-on support" from its front page.
    // It asks for its downloads to come from there, not to be shared.
    const std::wstring site = L"https://reshade.me";
    const std::vector<std::pair<std::wstring, std::wstring>> headers = {{L"Referer", site + L"/"}};
    std::string page;
    if (!dl.get(site, headers, page, error)) return error = L"ReShade: " + error, false;
    std::smatch m;
    if (!std::regex_search(page, m, std::regex(R"(/downloads/ReShade_Setup_([\d.]+)_Addon\.exe)", std::regex::icase)))
        return error = L"ReShade: no download link for the add-on build on reshade.me", false;
    const std::wstring version = widen(m[1].str());
    const std::wstring url = site + widen(m[0].str());
    const std::wstring setup = work + L"\\ReShade_Setup_" + version + L"_Addon.exe";
    if (!dl.file(L"ReShade " + version, url, setup, headers, 0, error)) return false;
    {
        std::ifstream in(setup, std::ios::binary);
        char mz[2] = {};
        in.read(mz, 2);
        if (fs::file_size(setup) < (1u << 20) || mz[0] != 'M' || mz[1] != 'Z')
            return error = L"ReShade: the download is not the ReShade installer", false;
    }
    // The installer is an exe with a zip appended; the DLLs are in it.
    const std::wstring unpacked = work + L"\\reshade";
    if (!unpack(setup, unpacked, {L"ReShade64.dll"}, error)) return error = L"ReShade: " + error, false;
    const std::wstring dll = unpacked + L"\\ReShade64.dll";
    if (!exists(dll)) return error = L"ReShade: no ReShade64.dll in the installer", false;
    const std::wstring product = product_name(dll);
    if (product.find(L"ReShade") == std::wstring::npos)
        return error = L"ReShade: ReShade64.dll is not ReShade (" + product + L")", false;
    // mpv loads it as dxgi.dll from its own folder.
    if (!place(dll, engine + L"\\dxgi.dll", error)) return false;

    Record record(engine);
    record.clear(L"reshade");
    record.set(L"reshade", L"version", version);
    record.set(L"reshade", L"source", url);
    record.set(L"reshade", L"sha256", widen(sha256_file(setup)));
    record.set(L"reshade", L"installed", today());
    log(L"installed ReShade {} (add-on support)", version);
    return true;
}

// ---------------------------------------------------------------- DLSS 5

// The newest rhi-repo release tagged PREFIX-x.y.z (no release candidates).
const Json *newest_rhi(const Json &releases, const std::wstring &prefix) {
    const Json *best = nullptr;
    int best_v[3] = {-1, -1, -1};
    for (const Json &r : releases.items()) {
        int v[3];
        if (!parse_release_tag(widen(r.str("tag_name")), prefix, v)) continue;
        if (std::lexicographical_compare(best_v, best_v + 3, v, v + 3)) {
            std::copy(v, v + 3, best_v);
            best = &r;
        }
    }
    return best;
}

// A release's zip: the one asset, or the first .zip.
std::optional<Asset> zip_asset(const Json &release) {
    if (auto a = find_asset(release, std::wregex(LR"(.*\.zip)", std::regex::icase))) return a;
    const Json *assets = release.get("assets");
    if (assets && !assets->items().empty()) return to_asset(assets->items().front());
    return std::nullopt;
}

bool install_dlss5(const std::wstring &engine, const std::wstring &work, Downloader &dl, SetupProgress &progress,
                   std::wstring &error) {
    // Everything is downloaded and checked into stage first; only then is
    // anything in engine replaced.
    const std::wstring stage = work + L"\\dlss5";
    const std::wstring shaders = stage + L"\\reshade-shaders\\Shaders";
    const std::wstring textures = stage + L"\\reshade-shaders\\Textures";
    std::error_code ec;
    fs::create_directories(shaders + L"\\DLSS5Feeder", ec);
    std::vector<std::pair<const wchar_t *, std::wstring>> versions;
    auto fail = [&](const std::wstring &what) {
        error = L"DLSS 5: " + what;
        return false;
    };

    // DLSS5-Feeder: the add-on and its ReShade shader.
    const auto feeder = dl.json(kGitHubApi + L"jlrouzies-fr/DLSS5-Feeder/releases/latest", error);
    if (!feeder) return fail(error);
    const auto feeder_zip = find_asset(*feeder, std::wregex(LR"(DLSS5-Feeder-.*\.zip)", std::regex::icase));
    if (!feeder_zip) return fail(L"no DLSS5-Feeder zip in release " + widen(feeder->str("tag_name")));
    if (!dl.asset(L"DLSS5-Feeder", *feeder_zip, work + L"\\" + feeder_zip->name, error)) return false;
    if (!unpack(work + L"\\" + feeder_zip->name, work + L"\\feeder", {}, error)) return fail(error);
    const std::wstring addon = find_file(work + L"\\feeder", L"dlss5-feed.addon64");
    const std::wstring feed_fx = find_file(work + L"\\feeder", L"DLSS5_Feed.fx");
    if (addon.empty() || feed_fx.empty()) return fail(L"the DLSS5-Feeder zip is missing its add-on or shader");
    fs::copy_file(addon, stage + L"\\dlss5-feed.addon64", ec);
    fs::copy_file(feed_fx, shaders + L"\\DLSS5Feeder\\DLSS5_Feed.fx", ec);
    versions.push_back({L"feeder", widen(feeder->str("tag_name"))});
    if (dl.cancelled(error)) return false;

    // RenoDX-DLSS5 and the neural rendering runtime, from RHI's mirror.
    progress.step(L"Looking up RenoDX-DLSS5");
    const auto rhi = dl.json(kGitHubApi + L"RankFTW/rhi-repo/releases?per_page=100", error);
    if (!rhi) return fail(error);
    struct RhiPart {
        const wchar_t *prefix, *label, *file, *key;
    };
    for (const RhiPart part : {RhiPart{L"renodx-dlss5", L"RenoDX-DLSS5", L"renodx-dlss5.addon64", L"renodx"},
                               RhiPart{L"dlssnr", L"DLSS neural rendering", L"nvngx_dlssnr.dll", L"dlssnr"}}) {
        const Json *release = newest_rhi(*rhi, part.prefix);
        if (!release) return fail(std::wstring(L"no ") + part.prefix + L" release on RankFTW/rhi-repo");
        const auto zip = zip_asset(*release);
        if (!zip) return fail(std::wstring(L"the ") + part.prefix + L" release has no download");
        const std::wstring archive = work + L"\\" + zip->name;
        if (!dl.asset(part.label, *zip, archive, error)) return false;
        const std::wstring unpacked = work + L"\\" + part.prefix;
        if (!unpack(archive, unpacked, {}, error)) return fail(error);
        const std::wstring found = find_file(unpacked, part.file);
        if (found.empty()) return fail(std::wstring(L"no ") + part.file + L" in " + zip->name);
        fs::copy_file(found, stage + L"\\" + part.file, ec);
        versions.push_back({part.key, widen(release->str("tag_name"))});
        if (dl.cancelled(error)) return false;
    }

    // DLSS itself from NVIDIA's SDK, as published there, signed by NVIDIA.
    const auto dlss = dl.json(kGitHubApi + L"NVIDIA/DLSS/releases/latest", error);
    if (!dlss) return fail(error);
    const std::wstring dlss_tag = widen(dlss->str("tag_name"));
    const std::wstring dlss_dll = stage + L"\\nvngx_dlss.dll";
    if (!dl.file(L"NVIDIA DLSS " + dlss_tag,
                 L"https://github.com/NVIDIA/DLSS/raw/" + dlss_tag + L"/lib/Windows_x86_64/rel/nvngx_dlss.dll", dlss_dll,
                 {}, 0, error))
        return false;
    std::wstring signer;
    if (!signed_by(dlss_dll, L"NVIDIA Corporation", signer))
        return fail(L"nvngx_dlss.dll is not signed by NVIDIA (" + signer + L")");
    versions.push_back({L"dlss", dlss_tag});
    if (dl.cancelled(error)) return false;

    // LumeniteFX: motion vectors for DLSS5_Feed.fx (DLSS5_MV_PROVIDER=3).
    const std::wstring lumenite = work + L"\\lumenite.zip";
    if (!dl.file(L"LumeniteFX", L"https://github.com/umar-afzaal/LumeniteFX/archive/refs/heads/mainline.zip", lumenite,
                 {}, 0, error))
        return false;
    if (!unpack(lumenite, work + L"\\lumenite", {}, error)) return fail(error);
    const std::wstring license = find_file(work + L"\\lumenite", L"LICENSE.md");
    if (license.empty()) return fail(L"the LumeniteFX download has no LICENSE.md");
    const fs::path root = fs::path(license).parent_path();
    if (!fs::is_directory(root / L"Shaders") || !fs::is_directory(root / L"Textures"))
        return fail(L"the LumeniteFX download has no Shaders or Textures folder");
    fs::create_directories(shaders + L"\\LumeniteFX", ec);
    fs::create_directories(textures + L"\\LumeniteFX", ec);
    fs::copy(root / L"Shaders", shaders + L"\\LumeniteFX", fs::copy_options::recursive, ec);
    fs::copy(root / L"Textures", textures + L"\\LumeniteFX", fs::copy_options::recursive, ec);
    fs::copy_file(license, shaders + L"\\LumeniteFX\\LICENSE.md", fs::copy_options::overwrite_existing, ec);
    if (ec) return fail(L"could not unpack LumeniteFX: " + widen(ec.message()));
    versions.push_back({L"lumenite", L"mainline " + today()});

    // ReShade.fxh, which DLSS5_Feed.fx includes.
    if (!dl.file(L"ReShade.fxh", L"https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders/ReShade.fxh",
                 shaders + L"\\ReShade.fxh", {}, 0, error))
        return false;
    versions.push_back({L"reshade_fxh", L"crosire/reshade-shaders slim " + today()});
    if (dl.cancelled(error)) return false;

    // All there: replace what engine has. The shader folders are replaced
    // whole (no files left from an older version); the user's settings
    // (ReShade.ini, ReShadePreset.ini, dlss5-feed.cfg) are not touched.
    progress.step(L"Installing DLSS 5");
    for (const wchar_t *file : {L"dlss5-feed.addon64", L"renodx-dlss5.addon64", L"nvngx_dlssnr.dll", L"nvngx_dlss.dll"})
        if (!place(stage + L"\\" + file, engine + L"\\" + file, error)) return fail(error);
    const std::wstring engine_shaders = engine + L"\\reshade-shaders\\Shaders";
    if (!place_folder(shaders + L"\\DLSS5Feeder", engine_shaders + L"\\DLSS5Feeder", error) ||
        !place_folder(shaders + L"\\LumeniteFX", engine_shaders + L"\\LumeniteFX", error) ||
        !place_folder(textures + L"\\LumeniteFX", engine + L"\\reshade-shaders\\Textures\\LumeniteFX", error) ||
        !place(shaders + L"\\ReShade.fxh", engine_shaders + L"\\ReShade.fxh", error))
        return fail(error);

    Record record(engine);
    record.clear(L"dlss5");
    for (const auto &[key, version] : versions) record.set(L"dlss5", key, version);
    record.set(L"dlss5", L"installed", today());
    log(L"installed DLSS 5 components");
    return true;
}

}  // namespace

// ---------------------------------------------------------------- public

const wchar_t *component_name(Component c) {
    switch (c) {
    case Component::Mpv: return L"mpv";
    case Component::ReShade: return L"ReShade";
    case Component::Dlss5: return L"DLSS 5";
    }
    return L"?";
}

bool component_installed(const std::wstring &engine_dir, Component c) {
    switch (c) {
    case Component::Mpv: return exists(engine_dir + L"\\mpv.exe");
    case Component::ReShade: return exists(engine_dir + L"\\dxgi.dll");
    case Component::Dlss5:
        for (const wchar_t *file : {L"\\dlss5-feed.addon64", L"\\renodx-dlss5.addon64", L"\\nvngx_dlssnr.dll",
                                    L"\\nvngx_dlss.dll", L"\\reshade-shaders\\Shaders\\DLSS5Feeder\\DLSS5_Feed.fx"})
            if (!exists(engine_dir + file)) return false;
        return true;
    }
    return false;
}

std::vector<std::wstring> installed_versions(const std::wstring &engine_dir) {
    const std::wstring ini = engine_dir + L"\\components.ini";
    std::vector<std::wstring> out;
    for (const wchar_t *section : {L"mpv", L"reshade", L"dlss5"}) {
        wchar_t keys[2048] = {};
        const DWORD n = GetPrivateProfileSectionW(section, keys, DWORD(std::size(keys)), ini.c_str());
        std::wstring line;
        for (const wchar_t *k = keys; k < keys + n && *k; k += wcslen(k) + 1) {
            const std::wstring entry = k;
            if (entry.rfind(L"source=", 0) == 0 || entry.rfind(L"sha256=", 0) == 0) continue;
            line += (line.empty() ? L"" : L", ") + entry;
        }
        if (!line.empty()) out.push_back(std::wstring(section) + L": " + line);
    }
    return out;
}

void SetupProgress::step(const std::wstring &text) {
    std::lock_guard lock(m_);
    s_.step = text;
    s_.done = s_.total = 0;
}

void SetupProgress::bytes(uint64_t done, uint64_t total) {
    std::lock_guard lock(m_);
    s_.done = done;
    s_.total = total;
}

void SetupProgress::set_index(int index, int count) {
    std::lock_guard lock(m_);
    s_.index = index;
    s_.count = count;
}

SetupProgress::State SetupProgress::get() const {
    std::lock_guard lock(m_);
    return s_;
}

bool install_components(const std::wstring &engine_dir, const std::vector<Component> &which, SetupProgress &progress,
                        const std::atomic<bool> &cancel, std::wstring &error) {
    std::error_code ec;
    fs::create_directories(engine_dir, ec);
    WorkDir work;
    Downloader dl(progress, cancel);
    int index = 0;
    for (Component c : which) {
        progress.set_index(++index, int(which.size()));
        progress.step(std::wstring(L"Looking up ") + component_name(c));
        bool ok = false;
        switch (c) {
        case Component::Mpv: ok = install_mpv(engine_dir, work.path(), dl, error); break;
        case Component::ReShade: ok = install_reshade(engine_dir, work.path(), dl, error); break;
        case Component::Dlss5: ok = install_dlss5(engine_dir, work.path(), dl, progress, error); break;
        }
        if (!ok) {
            log(L"setup: {} failed: {}", component_name(c), error);
            if (cancel.load()) error = L"cancelled";  // whatever piece was running
            return false;
        }
    }
    return true;
}

bool parse_release_tag(const std::wstring &tag, const std::wstring &prefix, int (&version)[3]) {
    std::wsmatch m;
    if (tag.rfind(prefix + L"-", 0) != 0) return false;
    const std::wstring rest = tag.substr(prefix.size() + 1);
    if (!std::regex_match(rest, m, std::wregex(LR"((\d{1,6})\.(\d{1,6})\.(\d{1,6}))"))) return false;
    for (int i = 0; i < 3; ++i) version[i] = std::stoi(m[i + 1].str());
    return true;
}

}  // namespace glasir
