// SPDX-License-Identifier: MIT
#include "update.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <regex>
#include <vector>

#include "files.h"
#include "glasir_version.h"
#include "http.h"
#include "json.h"
#include "log.h"

namespace fs = std::filesystem;

namespace glasir {
namespace {

const std::vector<std::pair<std::wstring, std::wstring>> kGitHubHeaders = {
    {L"Accept", L"application/vnd.github+json"}, {L"X-GitHub-Api-Version", L"2022-11-28"}};

// Every file under root, as paths relative to it.
std::vector<fs::path> files_under(const fs::path &root) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) out.push_back(fs::relative(it->path(), root, ec));
    return out;
}

// Never part of an update: what the user made or Glasir writes while running.
bool kept(const fs::path &relative) {
    const std::wstring first = relative.begin()->wstring();
    if (_wcsicmp(relative.c_str(), L"glasir.ini") == 0 || _wcsicmp(relative.c_str(), L"update-journal.txt") == 0)
        return true;
    for (const wchar_t *dir : {L"logs", L"Screenshots", L"Captures", L"Bug reports"})
        if (_wcsicmp(first.c_str(), dir) == 0) return true;
    return false;
}

// The archive's members stay inside the folder it is unpacked to: no
// absolute paths, drive letters or "..". (Windows' tar refuses those
// itself; this is checked before anything is written.)
bool safe_members(const std::wstring &zip, std::wstring &error) {
    WorkDir work(L"update-list");
    const std::wstring list = work.path() + L"\\members.txt";
    // tar -t writes the names to its standard output: a file here.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(list.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE) return error = L"could not list the download's files", false;
    const bool listed = run_tar({L"-tf", zip}, false, error, out);
    CloseHandle(out);
    if (!listed) return error = L"could not list the download's files (" + error + L")", false;
    std::string names;
    read_shared(list, names);
    size_t count = 0;
    for (size_t at = 0; at < names.size();) {
        size_t nl = names.find('\n', at);
        if (nl == std::string::npos) nl = names.size();
        std::string name = names.substr(at, nl - at);
        at = nl + 1;
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (name.empty()) continue;
        ++count;
        std::string slashed = "/" + name + "/";
        for (char &c : slashed)
            if (c == '\\') c = '/';
        if (name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos ||
            slashed.find("/../") != std::string::npos)
            return error = L"the download has a file outside its folder: " + widen(name), false;
    }
    if (count == 0) return error = L"the download is empty", false;
    return true;
}

// The release unpacked in folder: its root (the Glasir.exe with Glasir's
// frame generation beside it), checked to be Glasir of that version.
bool check_unpacked(const std::wstring &folder, const int (&expect)[3], std::wstring &root, std::wstring &error) {
    std::wstring exe;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && _wcsicmp(it->path().filename().c_str(), L"Glasir.exe") == 0 &&
            file_exists((it->path().parent_path() / L"engine" / L"zz-glasir-framegen.addon64").wstring())) {
            exe = it->path().wstring();
            break;
        }
    if (exe.empty()) return error = L"the download has no Glasir.exe with Glasir's frame generation", false;
    root = fs::path(exe).parent_path().wstring();
    if (product_name(exe) != L"Glasir") return error = L"the Glasir.exe in the download is not Glasir", false;
    int got[3];
    if (!file_version(exe, got) || got[0] != expect[0] || got[1] != expect[1] || got[2] != expect[2])
        return error = L"the Glasir.exe in the download is not version " + version_text(expect), false;
    return true;
}

// ---------------------------------------------------------------- the swap
//
// update-journal.txt in the app folder makes the swap one step that either
// happens or is undone, also if Glasir is ended halfway (power cut, killed):
//
//   1. every new file is copied beside the one it replaces (name.new);
//   2. the journal is written: "swapping", then each file and whether there
//      was one before it;
//   3. each old file is renamed to name.old and the new one into its place,
//      Glasir.exe last (so until the very end, the old Glasir starts);
//   4. the journal says "done"; the .old files are deleted, and the journal
//      once they all are (the running Glasir.exe.old only after it ends).
//
// A start that finds a journal finishes the job (4) if it says "done" or no
// file is left to swap, and otherwise puts the old files back. Files are only
// ever deleted once nothing can need them.

constexpr wchar_t kJournal[] = L"update-journal.txt";

struct Entry {
    fs::path file;  // relative to the app folder
    bool had_old = false;
};

std::wstring with(const fs::path &p, const wchar_t *suffix) { return p.wstring() + suffix; }

bool write_journal(const fs::path &app, bool done, const std::vector<Entry> &entries) {
    std::string text = done ? "done\n" : "swapping\n";
    for (const Entry &e : entries) text += (e.had_old ? "1|" : "0|") + narrow(e.file.wstring()) + "\n";
    const std::wstring path = (app / kJournal).wstring(), part = path + L".new";
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, text.data(), DWORD(text.size()), &written, nullptr) && written == text.size() &&
                    FlushFileBuffers(f);
    CloseHandle(f);
    return ok && MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool read_journal(const fs::path &app, bool &done, std::vector<Entry> &entries) {
    std::string text;
    if (!read_shared((app / kJournal).wstring(), text)) return false;
    entries.clear();
    bool first = true;
    for (size_t at = 0; at < text.size();) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(at, nl - at);
        at = nl + 1;
        if (first) {
            done = line == "done";
            first = false;
        } else if (line.size() > 2 && line[1] == '|') {
            entries.push_back({fs::path(widen(line.substr(2))), line[0] == '1'});
        }
    }
    return !first;
}

// Puts the old files back. The .new files go only once every old one is
// back, so a failure halfway can be retried by the next start.
bool roll_back(const fs::path &app, const std::vector<Entry> &entries) {
    bool all = true;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const fs::path dest = app / it->file;
        if (it->had_old) {
            if (file_exists(with(dest, L".old")) &&
                !MoveFileExW(with(dest, L".old").c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                log(L"update: could not put back {} (error {})", dest.wstring(), GetLastError());
                all = false;
            }
        } else if (file_exists(dest.wstring()) && !file_exists(with(dest, L".new")) && !DeleteFileW(dest.c_str())) {
            log(L"update: could not remove the new {} (error {})", dest.wstring(), GetLastError());
            all = false;
        }
    }
    if (!all) return false;
    for (const Entry &e : entries) DeleteFileW(with(app / e.file, L".new").c_str());
    DeleteFileW((app / kJournal).c_str());
    return true;
}

// The swap is done: the .old files go, then the journal. retry_ms: how long
// to keep trying a file in use (the old Glasir.exe while it ends).
bool finish(const fs::path &app, const std::vector<Entry> &entries, DWORD retry_ms) {
    bool all = true;
    for (const Entry &e : entries) {
        if (!e.had_old) continue;
        const std::wstring old = with(app / e.file, L".old");
        bool gone = !file_exists(old) || DeleteFileW(old.c_str());
        for (DWORD waited = 0; !gone && waited < retry_ms; waited += 100) {
            Sleep(100);
            gone = DeleteFileW(old.c_str()) || !file_exists(old);
        }
        all = all && gone;
    }
    if (all) DeleteFileW((app / kJournal).c_str());
    return all;
}

bool swap_in(const std::wstring &root, const std::wstring &app_dir, SetupProgress &progress, std::wstring &error) {
    progress.step(L"Installing the update");
    const fs::path app(app_dir);
    std::error_code ec;
    std::vector<Entry> entries;
    for (const fs::path &p : files_under(root))
        if (!kept(p)) entries.push_back({p, file_exists((app / p).wstring())});
    // Glasir.exe last: until it is swapped, a start runs the old one.
    std::stable_partition(entries.begin(), entries.end(),
                          [](const Entry &e) { return _wcsicmp(e.file.c_str(), L"Glasir.exe") != 0; });

    // 1. Beside the old files; a failure here changes nothing.
    for (size_t i = 0; i < entries.size(); ++i) {
        const fs::path dest = app / entries[i].file;
        fs::create_directories(dest.parent_path(), ec);
        if (!CopyFileW((fs::path(root) / entries[i].file).c_str(), with(dest, L".new").c_str(), FALSE)) {
            error = std::format(L"could not write {} (error {})", with(dest, L".new"), GetLastError());
            for (size_t k = 0; k <= i; ++k) DeleteFileW(with(app / entries[k].file, L".new").c_str());
            return false;
        }
    }
    // 2.
    if (!write_journal(app, false, entries)) {
        error = L"could not write " + (app / kJournal).wstring();
        for (const Entry &e : entries) DeleteFileW(with(app / e.file, L".new").c_str());
        return false;
    }
    // 3.
    for (const Entry &e : entries) {
        const fs::path dest = app / e.file;
        DWORD failed = 0;
        if (e.had_old && !MoveFileExW(dest.c_str(), with(dest, L".old").c_str(), MOVEFILE_REPLACE_EXISTING))
            failed = GetLastError();
        else if (!MoveFileExW(with(dest, L".new").c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING))
            failed = GetLastError();
        if (failed) {
            error = std::format(L"could not replace {} (error {}; is something using it?)", dest.wstring(), failed);
            if (!roll_back(app, entries))
                error += L". Glasir's folder is not complete: start Glasir again to put the old version back";
            return false;
        }
    }
    // 4.
    if (!write_journal(app, true, entries)) log(L"update: could not mark the journal done");
    finish(app, entries, 0);  // the running Glasir.exe.old stays until the next start
    log(L"update: {} files replaced", entries.size());
    return true;
}

}  // namespace

bool parse_version(const std::wstring &tag, int (&version)[3]) {
    std::wsmatch m;
    if (!std::regex_match(tag, m, std::wregex(LR"([vV]?(\d{1,5})\.(\d{1,5})\.(\d{1,5}))"))) return false;
    for (int i = 0; i < 3; ++i) version[i] = std::stoi(m[i + 1].str());
    return true;
}

bool newer(const int (&a)[3], const int (&b)[3]) { return std::lexicographical_compare(b, b + 3, a, a + 3); }

void current_version(int (&version)[3]) {
    version[0] = GLASIR_VERSION_MAJOR;
    version[1] = GLASIR_VERSION_MINOR;
    version[2] = GLASIR_VERSION_PATCH;
}

std::wstring version_text(const int (&version)[3]) {
    return std::format(L"{}.{}.{}", version[0], version[1], version[2]);
}

std::wstring update_repo() {
    wchar_t repo[128] = {};
    const DWORD n = GetEnvironmentVariableW(L"GLASIR_UPDATE_REPO", repo, DWORD(std::size(repo)));
    if (n > 0 && n < std::size(repo)) return repo;
    const std::wstring home = widen(GLASIR_HOMEPAGE);  // https://github.com/Abhito/Glasir
    const std::wstring prefix = L"https://github.com/";
    return home.rfind(prefix, 0) == 0 ? home.substr(prefix.size()) : L"Abhito/Glasir";
}

bool read_release(const std::string &json, Release &release, std::wstring &error) {
    const auto j = Json::parse(json);
    if (!j || !j->is(Json::Type::Object)) return error = L"unexpected answer from GitHub", false;
    release = Release{};
    release.tag = widen(j->str("tag_name"));
    release.page = widen(j->str("html_url"));
    if (const Json *draft = j->get("draft"); draft && draft->boolean()) return error = L"a draft release", false;
    if (const Json *pre = j->get("prerelease"); pre && pre->boolean()) return error = L"a pre-release", false;
    if (!parse_version(release.tag, release.version))
        return error = L"the newest release's tag is not a version: " + release.tag, false;
    const std::wstring want = L"Glasir-" + version_text(release.version) + L".zip";
    if (const Json *assets = j->get("assets"))
        for (const Json &a : assets->items()) {
            if (_wcsicmp(widen(a.str("name")).c_str(), want.c_str()) != 0) continue;
            release.zip_name = want;
            release.zip_url = widen(a.str("browser_download_url"));
            if (const Json *size = a.get("size")) release.zip_size = uint64_t(size->number());
            const std::string &digest = a.str("digest");
            if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64) release.zip_sha256 = digest.substr(7);
        }
    if (release.zip_url.empty()) return error = L"release " + release.tag + L" has no " + want, false;
    return true;
}

bool latest_release(const std::wstring &repo, Release &release, std::wstring &error,
                    const std::atomic<bool> *cancel) {
    // For tests: GitHub's answer from a file.
    wchar_t test[MAX_PATH] = {};
    if (const DWORD n = GetEnvironmentVariableW(L"GLASIR_UPDATE_TEST_RELEASE", test, MAX_PATH); n > 0 && n < MAX_PATH) {
        std::string body;
        if (!read_shared(test, body)) return error = std::wstring(L"cannot read ") + test, false;
        return read_release(body, release, error);
    }
    HttpRequest r;
    r.url = L"https://api.github.com/repos/" + repo + L"/releases/latest";
    r.headers = kGitHubHeaders;
    r.cancel = cancel;
    // In the background (cancel set): short, so Glasir closing never waits long on it.
    if (cancel) r.timeout_ms = 5000;
    std::string body;
    if (!http_get(r, body, error)) {
        // GitHub answers 404 for a repository without releases (or private).
        if (error == L"the server answered HTTP 404") error = L"no releases published at github.com/" + repo;
        return false;
    }
    return read_release(body, release, error);
}

bool latest_release(const std::wstring &repo, Release &release, std::wstring &error) {
    return latest_release(repo, release, error, nullptr);
}

bool install_update(const std::wstring &app_dir, const Release &release, SetupProgress &progress,
                    const std::atomic<bool> &cancel, std::wstring &error) {
    log(L"update: to Glasir {} from {}", version_text(release.version), release.zip_url);
    if (release.zip_sha256.empty())
        return error = L"GitHub publishes no SHA-256 for " + release.zip_name + L"; not installed", false;
    WorkDir work(L"update");
    const std::wstring zip = work.path() + L"\\" + release.zip_name;
    progress.step(L"Downloading Glasir " + version_text(release.version));
    HttpRequest r;
    r.url = release.zip_url;
    r.cancel = &cancel;
    r.progress = [&](uint64_t done, uint64_t total) { progress.bytes(done, total ? total : release.zip_size); };
    if (!http_download(r, zip, error)) return false;
    const std::string got = sha256_file(zip);
    if (got != release.zip_sha256)
        return error = L"the download does not match its published SHA-256 (" + widen(got) + L")", false;
    log(L"update: {} SHA-256 {} matches", release.zip_name, widen(got));
    if (cancel.load()) return error = L"cancelled", false;
    return install_update_zip(app_dir, zip, release.version, progress, cancel, error);
}

bool install_update_zip(const std::wstring &app_dir, const std::wstring &zip, const int (&expect)[3],
                        SetupProgress &progress, const std::atomic<bool> &cancel, std::wstring &error) {
    progress.step(L"Unpacking the update");
    WorkDir work(L"update");
    const std::wstring unpacked = work.path() + L"\\new";
    const std::wstring archive = fs::absolute(zip).wstring();  // never read as a tar option
    if (!safe_members(archive, error) || !unpack(archive, unpacked, {}, error)) return false;
    std::wstring root;
    if (!check_unpacked(unpacked, expect, root, error)) return false;
    // The last moment Cancel can stop it: once files are swapped, it finishes.
    if (cancel.load()) return error = L"cancelled", false;
    if (!swap_in(root, app_dir, progress, error)) return false;
    log(L"update: Glasir {} installed", version_text(expect));
    return true;
}

void finish_update(const std::wstring &app_dir) {
    const fs::path app(app_dir);
    bool done = false;
    std::vector<Entry> entries;
    if (!read_journal(app, done, entries)) {
        // An update from before the journal, or a journal-less leftover.
        const std::wstring old = app_dir + L"\\Glasir.exe.old";
        if (file_exists(old) && DeleteFileW(old.c_str())) log(L"update: removed the old Glasir.exe");
        return;
    }
    // Every new file in place (none left to swap) counts as done too: Glasir
    // ended between the last swap and marking the journal.
    bool left = false;
    for (const Entry &e : entries) left = left || file_exists(with(app / e.file, L".new"));
    if (done || !left) {
        // Right after an update the old Glasir may still be ending.
        if (finish(app, entries, 3000)) log(L"update: finished (old files removed)");
        else log(L"update: some old files are still in use; removed at a later start");
    } else if (roll_back(app, entries)) {
        log(L"update: the last update did not finish; the version before it is back");
    } else {
        log(L"update: the last update did not finish, and its old files could not all be put back yet");
    }
}

UpdateCheck::~UpdateCheck() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

void UpdateCheck::start() {
    State off = State::Off;
    if (!state_.compare_exchange_strong(off, State::Checking)) return;
    thread_ = std::thread([this] {
        Release r;
        std::wstring error;
        if (!latest_release(update_repo(), r, error, &cancel_)) {
            log(L"update check: {}", error);
            state_ = State::Failed;
            return;
        }
        int now[3];
        current_version(now);
        if (!newer(r.version, now)) {
            log(L"update check: {} is the newest", version_text(now));
            state_ = State::UpToDate;
            return;
        }
        log(L"update check: Glasir {} is out ({})", version_text(r.version), r.page);
        {
            std::lock_guard lock(m_);
            release_ = r;
        }
        state_ = State::Available;
    });
}

Release UpdateCheck::release() const {
    std::lock_guard lock(m_);
    return release_;
}

}  // namespace glasir
