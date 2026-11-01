// SPDX-License-Identifier: MIT
// Glasir's own updates, from its GitHub releases.
//
// The launcher asks GitHub for the newest release in the background
// ([update] check = 1). Updating downloads its Glasir-x.y.z.zip, checks it
// against the SHA-256 GitHub publishes, unpacks it and checks that it is
// Glasir of that version. Then the files are swapped in as one step that
// either happens or is undone, also when Glasir is ended halfway (a journal
// in the app folder; see update.cpp). Glasir.exe cannot be replaced while it
// runs, but it can be renamed: it becomes Glasir.exe.old, which the next
// start deletes. glasir.ini, the logs and the downloaded programs (mpv,
// ReShade, DLSS 5) are not in a release and are not touched.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "components.h"

namespace glasir {

struct Release {
    std::wstring tag;           // "v0.2.0"
    int version[3] = {0, 0, 0};
    std::wstring page;          // the release's web page (what's new)
    std::wstring zip_name, zip_url;
    uint64_t zip_size = 0;
    std::string zip_sha256;     // as GitHub publishes it
};

// "v1.2.3" or "1.2.3"; false for anything else (a pre-release "1.2.3-rc1").
bool parse_version(const std::wstring &tag, int (&version)[3]);
// a is newer than b.
bool newer(const int (&a)[3], const int (&b)[3]);
// This build's version.
void current_version(int (&version)[3]);
std::wstring version_text(const int (&version)[3]);

// The repository updates come from: owner/name ("Abhito/Glasir", or
// GLASIR_UPDATE_REPO for tests).
std::wstring update_repo();

// Reads a release as GitHub's API returns it (releases/latest): its version
// and its Glasir-x.y.z.zip. False with error set if it has none.
bool read_release(const std::string &json, Release &release, std::wstring &error);
// Asks GitHub for the newest release (GLASIR_UPDATE_TEST_RELEASE=<file>:
// reads that answer from a file instead, for tests).
bool latest_release(const std::wstring &repo, Release &release, std::wstring &error);

// Downloads, checks and installs the release into app_dir (Glasir.exe's
// folder). Nothing may be running from it (the one-instance lock is held).
bool install_update(const std::wstring &app_dir, const Release &release, SetupProgress &progress,
                    const std::atomic<bool> &cancel, std::wstring &error);
// Installs from a release zip on disk (Glasir.exe --update-from FILE).
bool install_update_zip(const std::wstring &app_dir, const std::wstring &zip, const int (&expect)[3],
                        SetupProgress &progress, const std::atomic<bool> &cancel, std::wstring &error);
// At every start: finishes an update that ended halfway, or puts the old
// version back if it had not got far enough; deletes the old files.
void finish_update(const std::wstring &app_dir);

// The background check the launcher shows.
class UpdateCheck {
public:
    enum class State { Off, Checking, UpToDate, Available, Failed };
    UpdateCheck() = default;
    UpdateCheck(const UpdateCheck &) = delete;
    UpdateCheck &operator=(const UpdateCheck &) = delete;
    ~UpdateCheck();

    void start();  // once; later calls do nothing
    State state() const { return state_.load(); }
    // The newer release (state Available).
    Release release() const;

private:
    std::atomic<State> state_{State::Off};
    std::atomic<bool> cancel_{false};
    mutable std::mutex m_;
    Release release_;
    std::thread thread_;
};

}  // namespace glasir
