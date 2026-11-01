// SPDX-License-Identifier: MIT
// File helpers shared by setup, the self-update and the bug report.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace glasir {

bool file_exists(const std::wstring &path);
// Lower-case hex SHA-256 of the file, "" if it cannot be read.
std::string sha256_file(const std::wstring &path);
// The file has a valid Authenticode signature whose signer's name contains
// signer (e.g. "NVIDIA Corporation"); who is the signer (or why not).
bool signed_by(const std::wstring &path, const std::wstring &signer, std::wstring &who);
// The version resource's ProductName, "" if none.
std::wstring product_name(const std::wstring &path);
// The version resource's file version (major.minor.patch); false if none.
bool file_version(const std::wstring &path, int (&version)[3]);

// A new folder under %TEMP% (glasir-<what>-...), removed with what is in it.
class WorkDir {
public:
    explicit WorkDir(const wchar_t *what = L"setup");
    WorkDir(const WorkDir &) = delete;
    WorkDir &operator=(const WorkDir &) = delete;
    ~WorkDir();
    const std::wstring &path() const { return path_; }

private:
    std::wstring path_;
};

// Runs Windows' tar.exe (libarchive) with these arguments and waits for it
// (at most 5 minutes). below_normal: at a lower priority (while a game runs).
// out: its standard output goes there (an inheritable handle), else nowhere.
bool run_tar(const std::vector<std::wstring> &args, bool below_normal, std::wstring &error, HANDLE out = nullptr);
// Unpacks a zip or 7z into folder. members: only these.
bool unpack(const std::wstring &archive, const std::wstring &folder, const std::vector<std::wstring> &members,
            std::wstring &error);
// The first file called name anywhere under folder, or "".
std::wstring find_file(const std::wstring &folder, const std::wstring &name);
// Puts file at dest, replacing what is there. The copy is made beside it and
// renamed over, so dest is never half written.
bool place(const std::wstring &file, const std::wstring &dest, std::wstring &error);
// Replaces folder dest with a copy of folder src.
bool place_folder(const std::wstring &src, const std::wstring &dest, std::wstring &error);

// Reads a whole file, also one another program has open for writing (a log).
bool read_shared(const std::wstring &path, std::string &data);

}  // namespace glasir
