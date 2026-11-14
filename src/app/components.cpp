// SPDX-License-Identifier: MIT
#include "components.h"

#include <windows.h>

#include <cctype>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <regex>

#include "files.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include "mpv_launch.h"
#include "tested_components.h"

namespace fs = std::filesystem;

namespace glasir {
namespace {

using Headers = std::vector<std::pair<std::wstring, std::wstring>>;

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

    bool get(const std::wstring &url, const Headers &headers, std::string &body, std::wstring &error) {
        HttpRequest r;
        r.url = url;
        r.headers = headers;
        r.cancel = &cancel_;
        return http_get(r, body, error);
    }

    // status: the server's answer (404 when the file is no longer there).
    bool file(const std::wstring &label, const std::wstring &url, const std::wstring &dest, const Headers &headers,
              uint64_t size_hint, std::wstring &error, int *status = nullptr) {
        progress_.step(L"Downloading " + label);
        HttpRequest r;
        r.url = url;
        r.headers = headers;
        r.cancel = &cancel_;
        r.status = status;
        r.progress = [&](uint64_t done, uint64_t total) { progress_.bytes(done, total ? total : size_hint); };
        if (!http_download(r, dest, error)) {
            error = label + L": " + error;
            return false;
        }
        log(L"downloaded {} ({} bytes)", url, fs::file_size(dest));
        return true;
    }

    bool cancelled(std::wstring &error) const {
        if (cancel_.load()) error = L"cancelled";
        return cancel_.load();
    }

    void step(const std::wstring &text) { progress_.step(text); }

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

// ---------------------------------------------------------------- tested or newest

// Where one part comes from.
struct Source {
    std::wstring version, url;
    std::string sha256;  // what the download must be, "" if nothing says
    uint64_t size = 0;
};
// Finds a part's newest version.
using Lookup = std::function<bool(Source &, std::wstring &)>;

// A part as downloaded.
struct Got {
    std::wstring file, version, url;
    std::string sha256;       // of the download
    std::wstring instead_of;  // the tested version this stands in for (no longer offered)
    bool tested = false;
};

class Fetcher {
public:
    Fetcher(Downloader &dl, Versions versions, const std::wstring &work) : dl_(dl), versions_(versions), work_(work) {}
    Downloader &dl() { return dl_; }
    const std::wstring &work() const { return work_; }

    // Downloads part into the work folder: its tested version, or the newest
    // when that is asked for, when the tested set does not name the part, or
    // when the tested download is no longer offered. The download must have
    // the SHA-256 its source gives, if it gives one.
    bool fetch(const char *part, const std::wstring &label, const Lookup &newest, Got &got, std::wstring &error,
               const Headers &headers = {}) {
        got = {};
        Source src;
        if (const Pin *pin = versions_ == Versions::Tested ? tested_components().find(part) : nullptr) {
            src = {pin->version, pin->url, pin->sha256, pin->size};
            got.file = file_for(part, src.url);
            int status = 0;
            if (dl_.file(label, src.url, got.file, headers, src.size, error, &status)) {
                got.tested = true;
            } else if (status == 404 || status == 410) {
                log(L"{}: the tested version {} is no longer offered ({}): taking the newest", label, src.version,
                    src.url);
                got.instead_of = src.version;
                error.clear();
            } else {
                return false;
            }
        }
        if (!got.tested) {
            dl_.step(L"Looking up " + label);
            src = {};
            if (!newest(src, error)) {
                error = label + L": " + error;
                return false;
            }
            if (dl_.cancelled(error)) return false;
            got.file = file_for(part, src.url);
            if (!dl_.file(label, src.url, got.file, headers, src.size, error)) return false;
        }
        got.version = src.version;
        got.url = src.url;
        got.sha256 = sha256_file(got.file);
        if (src.sha256.empty()) {
            log(L"{} {}: no SHA-256 published; not checked", label, src.version);
        } else if (got.sha256 != src.sha256) {
            error = label + L": the download does not match " +
                    (got.tested ? L"the tested version's" : L"its published") + L" SHA-256 (" + widen(got.sha256) +
                    L")";
            return false;
        } else {
            log(L"{} {}: SHA-256 {} matches{}", label, src.version, widen(got.sha256),
                got.tested ? L" the tested version's" : L"");
        }
        return !dl_.cancelled(error);
    }

private:
    // work\<part>-<the address's last piece>
    std::wstring file_for(const char *part, const std::wstring &url) const {
        std::wstring name = url.substr(url.find_last_of(L'/') + 1);
        name = name.substr(0, name.find_first_of(L"?#"));
        for (wchar_t &c : name)
            if (!iswalnum(c) && c != L'.' && c != L'-' && c != L'_') c = L'_';
        return work_ + L"\\" + widen(part) + L"-" + name;
    }

    Downloader &dl_;
    Versions versions_;
    std::wstring work_;
};

// ---------------------------------------------------------------- records

std::wstring today() {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    wchar_t text[16];
    wcsftime(text, std::size(text), L"%Y-%m-%d", &local);
    return text;
}

// A part's keys in components.ini: version, source, sha256 and instead_of in
// a section of its own (mpv, reshade); NAME, NAME_source, ... in [dlss5].
std::wstring record_key(const std::wstring &part, const wchar_t *what) {
    if (part.empty()) return what;
    return wcscmp(what, L"version") == 0 ? part : part + L"_" + what;
}

class Record {
public:
    explicit Record(const std::wstring &engine_dir) : ini_(engine_dir + L"\\components.ini") {}
    void set(const wchar_t *section, const std::wstring &key, const std::wstring &value) {
        WritePrivateProfileStringW(section, key.c_str(), value.c_str(), ini_.c_str());
    }
    void set(const wchar_t *section, const std::wstring &part, const Got &got) {
        set(section, record_key(part, L"version"), got.version);
        set(section, record_key(part, L"source"), got.url);
        set(section, record_key(part, L"sha256"), widen(got.sha256));
        if (!got.instead_of.empty()) set(section, record_key(part, L"instead_of"), got.instead_of);
    }
    void clear(const wchar_t *section) { WritePrivateProfileStringW(section, nullptr, nullptr, ini_.c_str()); }

private:
    std::wstring ini_;
};

std::wstring recorded(const std::wstring &engine_dir, const wchar_t *section, const std::wstring &key) {
    wchar_t value[512] = {};
    GetPrivateProfileStringW(section, key.c_str(), L"", value, DWORD(std::size(value)),
                             (engine_dir + L"\\components.ini").c_str());
    return value;
}

// part: the tested set's name for it; key: "" for a section of its own.
bool part_as_tested(const std::wstring &engine_dir, const char *part, const wchar_t *section, const std::wstring &key) {
    const Pin *pin = tested_components().find(part);
    if (!pin) return true;  // the set names no version of it
    if (recorded(engine_dir, section, record_key(key, L"instead_of")) == pin->version) return true;
    if (recorded(engine_dir, section, record_key(key, L"version")) != pin->version) return false;
    // Glasir up to 0.1.3 kept no SHA-256 for DLSS 5's parts: the version says.
    const std::wstring sha256 = recorded(engine_dir, section, record_key(key, L"sha256"));
    return sha256.empty() || sha256 == widen(pin->sha256);
}

// Puts several files and folders into the engine folder together. Each is
// copied in beside its place first (NAME.new); only once every copy is there
// are they renamed into place. A copy that fails (a full disk) leaves the
// engine folder as it was. A rename could still fail halfway (a file held
// open by another program); setup holds the one-instance lock, so mpv is not
// running then.
class EngineSwap {
public:
    void file(const std::wstring &from, const std::wstring &to) { files_.push_back({from, to}); }
    void folder(const std::wstring &from, const std::wstring &to) { folders_.push_back({from, to}); }

    bool run(std::wstring &error) {
        std::error_code ec;
        auto abandon = [&] {
            for (const auto &[from, to] : files_) DeleteFileW((to + L".new").c_str());
            for (const auto &[from, to] : folders_) fs::remove_all(to + L".new", ec);
        };
        for (const auto &[from, to] : files_) {
            fs::create_directories(fs::path(to).parent_path(), ec);
            if (!CopyFileW(from.c_str(), (to + L".new").c_str(), FALSE)) {
                error = std::format(L"could not write {}.new (error {})", to, GetLastError());
                abandon();
                return false;
            }
        }
        for (const auto &[from, to] : folders_) {
            fs::remove_all(to + L".new", ec);
            fs::create_directories(to + L".new", ec);
            fs::copy(from, to + L".new", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error = L"could not copy to " + to + L".new: " + widen(ec.message());
                abandon();
                return false;
            }
        }
        for (const auto &[from, to] : files_)
            if (!MoveFileExW((to + L".new").c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                error = in_use(to, GetLastError());
                abandon();
                return false;
            }
        for (const auto &[from, to] : folders_) {
            fs::remove_all(to + L".old", ec);
            if (fs::exists(to, ec)) fs::rename(to, to + L".old", ec);
            if (!ec) fs::rename(to + L".new", to, ec);
            if (ec) {
                const std::wstring why = widen(ec.message());
                if (!fs::exists(to) && fs::exists(to + L".old")) fs::rename(to + L".old", to, ec);  // put it back
                error = L"could not replace " + to + L": " + why + L" (close mpv and Glasir)";
                abandon();
                return false;
            }
            fs::remove_all(to + L".old", ec);
        }
        return true;
    }

private:
    static std::wstring in_use(const std::wstring &file, DWORD e) {
        return e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED
                   ? L"could not replace " + file + L": it is in use or read-only (close mpv and Glasir)"
                   : std::format(L"could not replace {} (error {})", file, e);
    }

    std::vector<std::pair<std::wstring, std::wstring>> files_, folders_;
};

constexpr const char *kDlss5Parts[] ={"feeder", "renodx", "dlssnr", "dlss", "reshade_fxh"};

// ---------------------------------------------------------------- mpv

bool install_mpv(const std::wstring &engine, Fetcher &f, std::wstring &error) {
    const Lookup newest = [&](Source &s, std::wstring &e) {
        const auto release = f.dl().json(kGitHubApi + L"zhongfly/mpv-winbuild/releases/latest", e);
        if (!release) return false;
        // The plain x86_64 build: not x86_64-v3 (needs AVX2), not libmpv.
        const auto asset = find_asset(*release, std::wregex(LR"(mpv-x86_64-\d{8}-git-[0-9a-f]+\.7z)"));
        if (!asset) return e = L"no x86_64 build in release " + widen(release->str("tag_name")), false;
        s = {std::regex_replace(asset->name, std::wregex(LR"(^mpv-x86_64-(.+)\.7z$)"), L"$1"), asset->url,
             asset->sha256, asset->size};
        return true;
    };
    Got got;
    if (!f.fetch("mpv", L"mpv", newest, got, error)) return false;
    const std::wstring unpacked = f.work() + L"\\mpv";
    if (!unpack(got.file, unpacked, {}, error)) return error = L"mpv: " + error, false;
    const std::wstring exe = find_file(unpacked, L"mpv.exe");
    if (exe.empty()) return error = L"mpv: no mpv.exe in the download", false;
    if (!place(exe, engine + L"\\mpv.exe", error)) return false;

    Record record(engine);
    record.clear(L"mpv");
    record.set(L"mpv", L"", got);
    record.set(L"mpv", L"installed", today());
    log(L"installed mpv {}", got.version);
    return true;
}

// ---------------------------------------------------------------- ReShade

bool install_reshade(const std::wstring &engine, Fetcher &f, std::wstring &error) {
    // reshade.me links "ReShade with full add-on support" from its front page.
    // It asks for its downloads to come from there, not to be shared.
    const std::wstring site = L"https://reshade.me";
    const Headers headers = {{L"Referer", site + L"/"}};
    const Lookup newest = [&](Source &s, std::wstring &e) {
        std::string page;
        if (!f.dl().get(site, headers, page, e)) return false;
        std::smatch m;
        if (!std::regex_search(page, m, std::regex(R"(/downloads/ReShade_Setup_([\d.]+)_Addon\.exe)", std::regex::icase)))
            return e = L"no download link for the add-on build on reshade.me", false;
        s = {widen(m[1].str()), site + widen(m[0].str()), "", 0};
        return true;
    };
    Got got;
    if (!f.fetch("reshade", L"ReShade", newest, got, error, headers)) return false;
    {
        std::ifstream in(got.file, std::ios::binary);
        char mz[2] = {};
        in.read(mz, 2);
        if (fs::file_size(got.file) < (1u << 20) || mz[0] != 'M' || mz[1] != 'Z')
            return error = L"ReShade: the download is not the ReShade installer", false;
    }
    // The installer is an exe with a zip appended; the DLLs are in it.
    const std::wstring unpacked = f.work() + L"\\reshade";
    if (!unpack(got.file, unpacked, {L"ReShade64.dll"}, error)) return error = L"ReShade: " + error, false;
    const std::wstring dll = unpacked + L"\\ReShade64.dll";
    if (!file_exists(dll)) return error = L"ReShade: no ReShade64.dll in the installer", false;
    const std::wstring product = product_name(dll);
    if (product.find(L"ReShade") == std::wstring::npos)
        return error = L"ReShade: ReShade64.dll is not ReShade (" + product + L")", false;
    // mpv loads it as dxgi.dll from its own folder.
    if (!place(dll, engine + L"\\dxgi.dll", error)) return false;

    Record record(engine);
    record.clear(L"reshade");
    record.set(L"reshade", L"", got);
    record.set(L"reshade", L"installed", today());
    log(L"installed ReShade {} (add-on support)", got.version);
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

bool install_dlss5(const std::wstring &engine, Fetcher &f, std::wstring &error) {
    // Everything is downloaded and checked into stage first; only then is
    // anything in engine replaced.
    const std::wstring &work = f.work();
    const std::wstring stage = work + L"\\dlss5";
    const std::wstring shaders = stage + L"\\reshade-shaders\\Shaders";
    std::error_code ec;
    fs::create_directories(shaders + L"\\DLSS5Feeder", ec);
    std::vector<std::pair<std::wstring, Got>> parts;
    auto fail = [&](const std::wstring &what) {
        error = L"DLSS 5: " + what;
        return false;
    };
    // Into the stage; any failure (a full disk) stops here.
    auto stage_copy = [&](const std::wstring &from, const std::wstring &to) {
        std::error_code copy_ec;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) error = L"DLSS 5: could not copy " + fs::path(from).filename().wstring() + L": " +
                             widen(copy_ec.message());
        return !copy_ec;
    };

    // DLSS5-Feeder: the add-on and its ReShade shader.
    const Lookup newest_feeder = [&](Source &s, std::wstring &e) {
        const auto release = f.dl().json(kGitHubApi + L"jlrouzies-fr/DLSS5-Feeder/releases/latest", e);
        if (!release) return false;
        const auto zip = find_asset(*release, std::wregex(LR"(DLSS5-Feeder-.*\.zip)", std::regex::icase));
        if (!zip) return e = L"no DLSS5-Feeder zip in release " + widen(release->str("tag_name")), false;
        s = {widen(release->str("tag_name")), zip->url, zip->sha256, zip->size};
        return true;
    };
    Got feeder;
    if (!f.fetch("feeder", L"DLSS5-Feeder", newest_feeder, feeder, error)) return false;
    if (!unpack(feeder.file, work + L"\\feeder", {}, error)) return fail(error);
    const std::wstring addon = find_file(work + L"\\feeder", L"dlss5-feed.addon64");
    const std::wstring feed_fx = find_file(work + L"\\feeder", L"DLSS5_Feed.fx");
    if (addon.empty() || feed_fx.empty()) return fail(L"the DLSS5-Feeder zip is missing its add-on or shader");
    if (!stage_copy(addon, stage + L"\\dlss5-feed.addon64") ||
        !stage_copy(feed_fx, shaders + L"\\DLSS5Feeder\\DLSS5_Feed.fx"))
        return false;
    parts.push_back({L"feeder", feeder});

    // RenoDX-DLSS5 and the neural rendering runtime, from RHI's mirror.
    struct RhiPart {
        const char *part;
        const wchar_t *prefix, *label, *file, *key;
    };
    std::optional<Json> rhi;  // its releases, asked for once
    for (const RhiPart part : {RhiPart{"renodx", L"renodx-dlss5", L"RenoDX-DLSS5", L"renodx-dlss5.addon64", L"renodx"},
                               RhiPart{"dlssnr", L"dlssnr", L"DLSS neural rendering", L"nvngx_dlssnr.dll", L"dlssnr"}}) {
        const Lookup newest = [&](Source &s, std::wstring &e) {
            if (!rhi) rhi = f.dl().json(kGitHubApi + L"RankFTW/rhi-repo/releases?per_page=100", e);
            if (!rhi) return false;
            const Json *release = newest_rhi(*rhi, part.prefix);
            if (!release) return e = std::wstring(L"no ") + part.prefix + L" release on RankFTW/rhi-repo", false;
            const auto zip = zip_asset(*release);
            if (!zip) return e = std::wstring(L"the ") + part.prefix + L" release has no download", false;
            s = {widen(release->str("tag_name")), zip->url, zip->sha256, zip->size};
            return true;
        };
        Got got;
        if (!f.fetch(part.part, part.label, newest, got, error)) return false;
        const std::wstring unpacked = work + L"\\" + part.prefix;
        if (!unpack(got.file, unpacked, {}, error)) return fail(error);
        const std::wstring found = find_file(unpacked, part.file);
        if (found.empty()) return fail(std::wstring(L"no ") + part.file + L" in the " + part.label + L" download");
        if (!stage_copy(found, stage + L"\\" + part.file)) return false;
        parts.push_back({part.key, got});
    }

    // DLSS itself from NVIDIA's SDK, as published there, signed by NVIDIA.
    const Lookup newest_dlss = [&](Source &s, std::wstring &e) {
        const auto release = f.dl().json(kGitHubApi + L"NVIDIA/DLSS/releases/latest", e);
        if (!release) return false;
        const std::wstring tag = widen(release->str("tag_name"));
        s = {tag, L"https://github.com/NVIDIA/DLSS/raw/" + tag + L"/lib/Windows_x86_64/rel/nvngx_dlss.dll", "", 0};
        return true;
    };
    Got dlss;
    if (!f.fetch("dlss", L"NVIDIA DLSS", newest_dlss, dlss, error)) return false;
    std::wstring signer;
    if (!signed_by(dlss.file, L"NVIDIA Corporation", signer))
        return fail(L"nvngx_dlss.dll is not signed by NVIDIA (" + signer + L")");
    if (!stage_copy(dlss.file, stage + L"\\nvngx_dlss.dll")) return false;
    parts.push_back({L"dlss", dlss});

    // ReShade.fxh, which DLSS5_Feed.fx includes: as of the commit that last
    // changed it (the branch moves on with other files).
    const Lookup newest_fxh = [&](Source &s, std::wstring &e) {
        const auto commits = f.dl().json(
            kGitHubApi + L"crosire/reshade-shaders/commits?sha=slim&path=Shaders/ReShade.fxh&per_page=1", e);
        if (!commits) return false;
        const std::wstring sha = commits->items().empty() ? L"" : widen(commits->items().front().str("sha"));
        if (sha.size() != 40) return e = L"no commit for ReShade.fxh on crosire/reshade-shaders slim", false;
        s = {sha.substr(0, 7),
             L"https://raw.githubusercontent.com/crosire/reshade-shaders/" + sha + L"/Shaders/ReShade.fxh", "", 0};
        return true;
    };
    Got fxh;
    if (!f.fetch("reshade_fxh", L"ReShade.fxh", newest_fxh, fxh, error)) return false;
    if (!stage_copy(fxh.file, shaders + L"\\ReShade.fxh")) return false;
    parts.push_back({L"reshade_fxh", fxh});

    // All there: replace what engine has, all together (EngineSwap). The
    // shader folder is replaced whole (no files left from an older version);
    // the user's settings (ReShade.ini, ReShadePreset.ini, dlss5-feed.cfg)
    // are not touched.
    f.dl().step(L"Installing DLSS 5");
    const std::wstring engine_shaders = engine + L"\\reshade-shaders\\Shaders";
    EngineSwap swap;
    for (const wchar_t *file : {L"dlss5-feed.addon64", L"renodx-dlss5.addon64", L"nvngx_dlssnr.dll", L"nvngx_dlss.dll"})
        swap.file(stage + L"\\" + file, engine + L"\\" + file);
    swap.file(shaders + L"\\ReShade.fxh", engine_shaders + L"\\ReShade.fxh");
    swap.folder(shaders + L"\\DLSS5Feeder", engine_shaders + L"\\DLSS5Feeder");
    if (!swap.run(error)) return fail(error);
    // LumeniteFX gave DLSS5_Feed.fx its motion vectors up to Glasir 0.1.3;
    // the Feeder makes its own now (reshade_config.cpp).
    for (const wchar_t *old : {L"\\reshade-shaders\\Shaders\\LumeniteFX", L"\\reshade-shaders\\Textures\\LumeniteFX"})
        if (fs::exists(engine + old, ec)) {
            fs::remove_all(engine + old, ec);
            log(L"removed {} (no longer used)", engine + old);
        }

    Record record(engine);
    record.clear(L"dlss5");
    for (const auto &[key, got] : parts) record.set(L"dlss5", key, got);
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
    case Component::Mpv: return file_exists(engine_dir + L"\\mpv.exe");
    case Component::ReShade: return file_exists(engine_dir + L"\\dxgi.dll");
    case Component::Dlss5:
        for (const wchar_t *file : {L"\\dlss5-feed.addon64", L"\\renodx-dlss5.addon64", L"\\nvngx_dlssnr.dll",
                                    L"\\nvngx_dlss.dll", L"\\reshade-shaders\\Shaders\\DLSS5Feeder\\DLSS5_Feed.fx"})
            if (!file_exists(engine_dir + file)) return false;
        return true;
    }
    return false;
}

std::vector<std::wstring> installed_versions(const std::wstring &engine_dir) {
    const std::wstring ini = engine_dir + L"\\components.ini";
    std::vector<std::wstring> out;
    for (const wchar_t *section : {L"mpv", L"reshade", L"dlss5"}) {
        std::vector<wchar_t> keys(16384);
        const DWORD n = GetPrivateProfileSectionW(section, keys.data(), DWORD(keys.size()), ini.c_str());
        std::wstring line;
        for (const wchar_t *k = keys.data(); k < keys.data() + n && *k; k += wcslen(k) + 1) {
            const std::wstring entry = k;
            // Not where it came from or its SHA-256: the file has those.
            const std::wstring key = entry.substr(0, entry.find(L'='));
            if (key.ends_with(L"source") || key.ends_with(L"sha256")) continue;
            line += (line.empty() ? L"" : L", ") + entry;
        }
        if (!line.empty()) out.push_back(std::wstring(section) + L": " + line);
    }
    return out;
}

const Pin *PinSet::find(std::string_view part) const {
    for (const auto &[name, pin] : parts)
        if (name == part) return &pin;
    return nullptr;
}

bool read_pins(const std::string &json, PinSet &pins, std::wstring &error) {
    pins.parts.clear();
    const auto root = Json::parse(json);
    if (!root || !root->is(Json::Type::Object)) {
        error = L"not a JSON object";
        return false;
    }
    for (const auto &[name, value] : root->members()) {
        if (!value.is(Json::Type::Object)) continue;
        Pin pin;
        pin.version = widen(value.str("version"));
        pin.url = widen(value.str("url"));
        pin.sha256 = value.str("sha256");
        for (char &c : pin.sha256) c = char(tolower(static_cast<unsigned char>(c)));
        if (const Json *size = value.get("size")) pin.size = uint64_t(size->number());
        const bool hex = pin.sha256.size() == 64 && pin.sha256.find_first_not_of("0123456789abcdef") == std::string::npos;
        if (pin.version.empty() || pin.url.rfind(L"https://", 0) != 0 || !hex) {
            error = widen(name) + L" needs a version, an https address and a SHA-256";
            pins.parts.clear();
            return false;
        }
        pins.parts.emplace_back(name, std::move(pin));
    }
    return true;
}

const PinSet &tested_components() {
    static const PinSet set = [] {
        PinSet pins;
        std::wstring error;
        wchar_t file[MAX_PATH] = {};
        if (const DWORD n = GetEnvironmentVariableW(L"GLASIR_COMPONENTS", file, MAX_PATH); n > 0 && n < MAX_PATH) {
            std::string text;
            if (!read_shared(file, text)) error = L"it cannot be read";
            if (error.empty() && read_pins(text, pins, error)) {
                log(L"components: the versions in {} (GLASIR_COMPONENTS)", file);
                return pins;
            }
            log(L"components: {} is not used ({}); the built-in versions are", file, error);
        }
        if (!read_pins(kTestedComponents, pins, error))
            log(L"components: the built-in versions cannot be read ({}): the newest are installed", error);
        return pins;
    }();
    return set;
}

bool component_as_tested(const std::wstring &engine_dir, Component c) {
    switch (c) {
    case Component::Mpv: return part_as_tested(engine_dir, "mpv", L"mpv", L"");
    case Component::ReShade: return part_as_tested(engine_dir, "reshade", L"reshade", L"");
    case Component::Dlss5:
        for (const char *part : kDlss5Parts)
            if (!part_as_tested(engine_dir, part, L"dlss5", widen(part))) return false;
        return true;
    }
    return true;
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

bool install_components(const std::wstring &engine_dir, const std::vector<Component> &which, Versions versions,
                        SetupProgress &progress, const std::atomic<bool> &cancel, std::wstring &error) {
    std::error_code ec;
    fs::create_directories(engine_dir, ec);
    WorkDir work;
    Downloader dl(progress, cancel);
    Fetcher fetcher(dl, versions, work.path());
    log(L"setup: {}", versions == Versions::Tested ? L"the versions this Glasir was tested with"
                                                   : L"the newest versions (--newest)");
    int index = 0;
    for (Component c : which) {
        progress.set_index(++index, int(which.size()));
        progress.step(std::wstring(L"Getting ") + component_name(c));
        bool ok = false;
        switch (c) {
        case Component::Mpv: ok = install_mpv(engine_dir, fetcher, error); break;
        case Component::ReShade: ok = install_reshade(engine_dir, fetcher, error); break;
        case Component::Dlss5: ok = install_dlss5(engine_dir, fetcher, error); break;
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
