#include "esshook.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#include <windows.h>

#include "overlay/notifications.h"
#include "util/detour.h"
#include "util/fileutils.h"
#include "util/logging.h"

/*
 * ess.dll is the shared e-amusement service layer of newer Konami games. It keeps downloaded
 * updates on fixed drive letters: F:/<model>-001/{bcast,update} for staging and
 * E:/<model>-001/ess_binary for broadcast binaries. Those drives only exist on a cabinet, so access
 * to them is redirected into dev/vfs/drive_e and dev/vfs/drive_f next to the game. A drive is only
 * redirected if it does not exist on this machine, so real drives keep working.
 *
 * Staged updates end up in dev/vfs/drive_f/<model>-001/update/ex, which is picked up by
 * misc/otaupdate.cpp on the next launch.
 */

static bool REDIRECT_E = false;
static bool REDIRECT_F = false;

static decltype(GetFileAttributesA) *GetFileAttributesA_orig = nullptr;
static decltype(FindFirstFileA) *FindFirstFileA_orig = nullptr;
static decltype(CreateDirectoryA) *CreateDirectoryA_orig = nullptr;
static decltype(CreateDirectoryW) *CreateDirectoryW_orig = nullptr;
static decltype(CreateFileA) *CreateFileA_orig = nullptr;
static decltype(CreateFileW) *CreateFileW_orig = nullptr;
static decltype(MoveFileA) *MoveFileA_orig = nullptr;
static decltype(CopyFileA) *CopyFileA_orig = nullptr;
static decltype(DeleteFileA) *DeleteFileA_orig = nullptr;
static decltype(RemoveDirectoryA) *RemoveDirectoryA_orig = nullptr;
static decltype(SetFileAttributesA) *SetFileAttributesA_orig = nullptr;

// true if the path is the directory ess.dll moves a fully downloaded and extracted update to
static bool is_staged_update_dir(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    std::transform(path.begin(), path.end(), path.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });

    static const std::string suffix = "\\update\\ex";
    return path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static void notify_update_staged() {
    static bool notified = false;
    if (notified)
        return;
    notified = true;

    log_info("ess", "update downloaded, it is installed on the next launch");
    overlay::notifications::add(overlay::notifications::Severity::Success,
            "Update downloaded. Restart the game to install it.");
}

static bool is_redirected(char drive) {
    switch (std::toupper(static_cast<unsigned char>(drive))) {
        case 'E':
            return REDIRECT_E;
        case 'F':
            return REDIRECT_F;
        default:
            return false;
    }
}

static bool is_redirected(const char *path) {
    return path && path[0] && path[1] == ':' && is_redirected(path[0]);
}

static bool is_redirected(const wchar_t *path) {
    return path && path[0] < 0x80 && path[1] == L':' && is_redirected(static_cast<char>(path[0]));
}

static std::string rewrite_path(const char *path) {
    if (!is_redirected(path))
        return path ? path : "";

    std::string result = "dev\\vfs\\drive_";
    result += static_cast<char>(std::tolower(static_cast<unsigned char>(path[0])));
    result += &path[2];
    return result;
}

static std::wstring rewrite_path(const wchar_t *path) {
    if (!is_redirected(path))
        return path ? path : L"";

    std::wstring result = L"dev\\vfs\\drive_";
    result += static_cast<wchar_t>(std::towlower(path[0]));
    result += &path[2];
    return result;
}

static DWORD WINAPI GetFileAttributesA_hook(LPCSTR lpFileName) {
    return GetFileAttributesA_orig(rewrite_path(lpFileName).c_str());
}

static HANDLE WINAPI FindFirstFileA_hook(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
    return FindFirstFileA_orig(rewrite_path(lpFileName).c_str(), lpFindFileData);
}

static BOOL WINAPI CreateDirectoryA_hook(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    if (!is_redirected(lpPathName))
        return CreateDirectoryA_orig(lpPathName, lpSecurityAttributes);

    // the virtual drive has none of the parent directories ess.dll expects to exist
    auto path = rewrite_path(lpPathName);
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        fileutils::dir_create_recursive(parent);

    return CreateDirectoryA_orig(path.c_str(), lpSecurityAttributes);
}

static BOOL WINAPI CreateDirectoryW_hook(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    if (!is_redirected(lpPathName))
        return CreateDirectoryW_orig(lpPathName, lpSecurityAttributes);

    auto path = rewrite_path(lpPathName);
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        fileutils::dir_create_recursive(parent);

    return CreateDirectoryW_orig(path.c_str(), lpSecurityAttributes);
}

static HANDLE WINAPI CreateFileA_hook(
        LPCSTR lpFileName,
        DWORD dwDesiredAccess,
        DWORD dwShareMode,
        LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        DWORD dwCreationDisposition,
        DWORD dwFlagsAndAttributes,
        HANDLE hTemplateFile)
{
    return CreateFileA_orig(rewrite_path(lpFileName).c_str(), dwDesiredAccess, dwShareMode,
            lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

static HANDLE WINAPI CreateFileW_hook(
        LPCWSTR lpFileName,
        DWORD dwDesiredAccess,
        DWORD dwShareMode,
        LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        DWORD dwCreationDisposition,
        DWORD dwFlagsAndAttributes,
        HANDLE hTemplateFile)
{
    return CreateFileW_orig(rewrite_path(lpFileName).c_str(), dwDesiredAccess, dwShareMode,
            lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

static BOOL WINAPI MoveFileA_hook(LPCSTR lpExistingFileName, LPCSTR lpNewFileName) {
    auto source = rewrite_path(lpExistingFileName);
    auto destination = rewrite_path(lpNewFileName);

    auto result = MoveFileA_orig(source.c_str(), destination.c_str());

    log_misc("ess", "MoveFileA {} -> {}: {}", source, destination, result ? "ok" : "failed");

    // the update is complete once ess.dll has moved the extracted files into update/ex
    if (result && is_staged_update_dir(destination))
        notify_update_staged();

    return result;
}

static BOOL WINAPI CopyFileA_hook(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, BOOL bFailIfExists) {
    return CopyFileA_orig(rewrite_path(lpExistingFileName).c_str(), rewrite_path(lpNewFileName).c_str(),
            bFailIfExists);
}

static BOOL WINAPI DeleteFileA_hook(LPCSTR lpFileName) {
    return DeleteFileA_orig(rewrite_path(lpFileName).c_str());
}

static BOOL WINAPI RemoveDirectoryA_hook(LPCSTR lpPathName) {
    return RemoveDirectoryA_orig(rewrite_path(lpPathName).c_str());
}

static BOOL WINAPI SetFileAttributesA_hook(LPCSTR lpFileName, DWORD dwFileAttributes) {
    return SetFileAttributesA_orig(rewrite_path(lpFileName).c_str(), dwFileAttributes);
}

void hooks::ess::init(HMODULE module) {
    log_info("ess", "initializing");

    // only redirect drives that are not available on this machine
    const DWORD drives = GetLogicalDrives();
    REDIRECT_E = !(drives & (1 << ('E' - 'A')));
    REDIRECT_F = !(drives & (1 << ('F' - 'A')));

    if (REDIRECT_E)
        fileutils::dir_create_recursive_log("ess", "dev\\vfs\\drive_e");
    if (REDIRECT_F)
        fileutils::dir_create_recursive_log("ess", "dev\\vfs\\drive_f");

    GetFileAttributesA_orig = detour::iat_try("GetFileAttributesA", GetFileAttributesA_hook, module);
    FindFirstFileA_orig = detour::iat_try("FindFirstFileA", FindFirstFileA_hook, module);
    CreateDirectoryA_orig = detour::iat_try("CreateDirectoryA", CreateDirectoryA_hook, module);
    CreateDirectoryW_orig = detour::iat_try("CreateDirectoryW", CreateDirectoryW_hook, module);
    CreateFileA_orig = detour::iat_try("CreateFileA", CreateFileA_hook, module);
    CreateFileW_orig = detour::iat_try("CreateFileW", CreateFileW_hook, module);
    MoveFileA_orig = detour::iat_try("MoveFileA", MoveFileA_hook, module);
    CopyFileA_orig = detour::iat_try("CopyFileA", CopyFileA_hook, module);
    DeleteFileA_orig = detour::iat_try("DeleteFileA", DeleteFileA_hook, module);
    RemoveDirectoryA_orig = detour::iat_try("RemoveDirectoryA", RemoveDirectoryA_hook, module);
    SetFileAttributesA_orig = detour::iat_try("SetFileAttributesA", SetFileAttributesA_hook, module);
}
