#include "folder.h"

#include <cstdint>
#include <cstring>
#include <windows.h>

namespace {

constexpr uint32_t kCrivateVersion = 1;
constexpr uint32_t kPbkdf2Iters = 600000;

#pragma pack(push, 1)
struct CrivateHeader {
    char magic[8];
    uint32_t version;
    uint32_t kdf_iters;
    uint8_t salt[16];
    uint8_t verifier_nonce[12];
    uint8_t verifier_tag[16];
    uint8_t verifier_ciphertext[32];
};
#pragma pack(pop)

static_assert(sizeof(CrivateHeader) == 92, "folder header must be 92 bytes");

void strip_trailing_slash(wchar_t* path) {
    const size_t n = wcslen(path);
    if (n > 1 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) {
        path[n - 1] = L'\0';
    }
}

}  // namespace

DirState folder_dir_state() {
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW(L"*", &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return DirState::Error;
    }

    DirState state = DirState::Empty;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        state = DirState::NotEmpty;
        break;
    } while (FindNextFileW(find, &fd));

    FindClose(find);
    return state;
}

bool folder_marker_exists() {
    return GetFileAttributesW(L".crivate") != INVALID_FILE_ATTRIBUTES;
}

bool folder_write_dummy_marker() {
    CrivateHeader header{};
    memcpy(header.magic, "CRIVATE1", 8);
    header.version = kCrivateVersion;
    header.kdf_iters = kPbkdf2Iters;

    const HANDLE file = CreateFileW(
        L".crivate",
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, &header, sizeof(header), &written, nullptr);
    CloseHandle(file);

    if (!ok || written != sizeof(header)) {
        DeleteFileW(L".crivate");
        return false;
    }
    return true;
}

bool folder_exe_in_cwd() {
    const DWORD attr = GetFileAttributesW(L"crivate.exe");
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return true;
    }

    wchar_t exe[MAX_PATH];
    const DWORD exe_len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (exe_len == 0 || exe_len >= MAX_PATH) {
        return false;
    }

    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash == nullptr) {
        slash = wcsrchr(exe, L'/');
    }
    if (slash == nullptr) {
        return false;
    }
    *slash = L'\0';
    strip_trailing_slash(exe);

    wchar_t cwd[MAX_PATH];
    const DWORD cwd_len = GetCurrentDirectoryW(MAX_PATH, cwd);
    if (cwd_len == 0 || cwd_len >= MAX_PATH) {
        return false;
    }
    strip_trailing_slash(cwd);

    return CompareStringOrdinal(exe, -1, cwd, -1, TRUE) == CSTR_EQUAL;
}
