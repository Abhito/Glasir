// SPDX-License-Identifier: MIT
#include "files.h"

#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>

#include <filesystem>
#include <format>
#include <fstream>

#include "log.h"
#include "mpv_launch.h"

namespace fs = std::filesystem;

namespace glasir {

bool file_exists(const std::wstring &path) {
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

namespace {

std::vector<BYTE> version_info(const std::wstring &path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    std::vector<BYTE> info(size);
    if (!size || !GetFileVersionInfoW(path.c_str(), 0, size, info.data())) info.clear();
    return info;
}

}  // namespace

std::wstring product_name(const std::wstring &path) {
    std::vector<BYTE> info = version_info(path);
    if (info.empty()) return L"";
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

bool file_version(const std::wstring &path, int (&version)[3]) {
    std::vector<BYTE> info = version_info(path);
    VS_FIXEDFILEINFO *fixed = nullptr;
    UINT n = 0;
    if (info.empty() || !VerQueryValueW(info.data(), L"\\", reinterpret_cast<void **>(&fixed), &n) ||
        n < sizeof(VS_FIXEDFILEINFO))
        return false;
    version[0] = HIWORD(fixed->dwFileVersionMS);
    version[1] = LOWORD(fixed->dwFileVersionMS);
    version[2] = HIWORD(fixed->dwFileVersionLS);
    return true;
}

WorkDir::WorkDir(const wchar_t *what) {
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    path_ = std::format(L"{}glasir-{}-{}-{}", temp, what, GetCurrentProcessId(), GetTickCount64());
    std::error_code ec;
    fs::create_directories(path_, ec);
}

WorkDir::~WorkDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
}

bool run_tar(const std::vector<std::wstring> &args, bool below_normal, std::wstring &error, HANDLE out) {
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring tar = std::wstring(system) + L"\\tar.exe";
    std::wstring command = join_command_line(tar, args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (out) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out;
        si.hStdError = out;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, out ? TRUE : FALSE,
                        CREATE_NO_WINDOW | (below_normal ? BELOW_NORMAL_PRIORITY_CLASS : 0), nullptr, nullptr, &si,
                        &pi)) {
        error = std::format(L"could not run {} (error {})", tar, GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 5 * 60 * 1000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        error = std::format(L"tar exit code {}", code);
        return false;
    }
    return true;
}

bool unpack(const std::wstring &archive, const std::wstring &folder, const std::vector<std::wstring> &members,
            std::wstring &error) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    std::vector<std::wstring> args = {L"-xf", archive, L"-C", folder};
    args.insert(args.end(), members.begin(), members.end());
    if (!run_tar(args, false, error)) {
        error = std::format(L"could not unpack {} ({})", fs::path(archive).filename().wstring(), error);
        return false;
    }
    return true;
}

std::wstring find_file(const std::wstring &folder, const std::wstring &name) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && _wcsicmp(it->path().filename().c_str(), name.c_str()) == 0)
            return it->path().wstring();
    return L"";
}

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

bool read_shared(const std::wstring &path, std::string &data) {
    data.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char buffer[64 * 1024];
    DWORD got = 0;
    bool ok = true;
    while ((ok = ReadFile(file, buffer, sizeof(buffer), &got, nullptr) != FALSE) && got) data.append(buffer, got);
    CloseHandle(file);
    return ok;
}

}  // namespace glasir
