#include "folder.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <windows.h>

namespace {

constexpr uint32_t kCrivateVersion = 1;
constexpr uint32_t kPbkdf2Iters = 600000;
constexpr uint32_t kMinKdfIters = 1;
constexpr uint32_t kMaxKdfIters = 10000000;
constexpr uint8_t kVerifierCanary = 0xA5;
constexpr size_t kVerifierPlaintextBytes = 32;

#pragma pack(push, 1)
struct CrivateHeader {
    char magic[8];
    uint32_t version;
    uint32_t kdf_iters;
    uint8_t salt[kSaltBytes];
    uint8_t verifier_nonce[kGcmNonceBytes];
    uint8_t verifier_tag[kGcmTagBytes];
    uint8_t verifier_ciphertext[kVerifierPlaintextBytes];
};
#pragma pack(pop)

static_assert(sizeof(CrivateHeader) == 92, "folder header must be 92 bytes");
static_assert(offsetof(CrivateHeader, verifier_nonce) == 32,
              "AAD is magic + version + kdf_iters + salt");

constexpr size_t kHeaderAadBytes = offsetof(CrivateHeader, verifier_nonce);

struct FileHandle {
    HANDLE h = INVALID_HANDLE_VALUE;

    explicit FileHandle(HANDLE handle) : h(handle) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    ~FileHandle() {
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
    }

    bool valid() const { return h != INVALID_HANDLE_VALUE; }
};

struct FindHandle {
    HANDLE h = INVALID_HANDLE_VALUE;

    explicit FindHandle(HANDLE handle) : h(handle) {}
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;

    ~FindHandle() {
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            h = INVALID_HANDLE_VALUE;
        }
    }

    bool valid() const { return h != INVALID_HANDLE_VALUE; }
};

void strip_trailing_slash(wchar_t* path) {
    const size_t n = wcslen(path);
    if (n > 1 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) {
        path[n - 1] = L'\0';
    }
}

bool header_structurally_valid(const CrivateHeader& header) {
    if (memcmp(header.magic, "CRIVATE1", 8) != 0) {
        return false;
    }
    if (header.version != kCrivateVersion) {
        return false;
    }
    if (header.kdf_iters < kMinKdfIters || header.kdf_iters > kMaxKdfIters) {
        return false;
    }
    return true;
}

bool read_header(CrivateHeader* out) {
    if (out == nullptr) {
        return false;
    }

    FileHandle file(CreateFileW(L".crivate", GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.h, &size) || size.QuadPart != static_cast<LONGLONG>(sizeof(CrivateHeader))) {
        return false;
    }

    DWORD read = 0;
    if (!ReadFile(file.h, out, sizeof(*out), &read, nullptr) || read != sizeof(*out)) {
        crypto_wipe(out, sizeof(*out));
        return false;
    }
    return header_structurally_valid(*out);
}

bool name_has_cri_extension(const wchar_t* name) {
    const size_t n = wcslen(name);
    if (n < 4) {
        return false;
    }
    return CompareStringOrdinal(name + (n - 4), 4, L".cri", 4, TRUE) == CSTR_EQUAL;
}

bool cri_name_less(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
}

}  // namespace

DirState folder_dir_state() {
    WIN32_FIND_DATAW fd{};
    FindHandle find(FindFirstFileW(L"*", &fd));
    if (!find.valid()) {
        return DirState::Error;
    }

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        return DirState::NotEmpty;
    } while (FindNextFileW(find.h, &fd));

    return GetLastError() == ERROR_NO_MORE_FILES ? DirState::Empty : DirState::Error;
}

bool folder_marker_exists() {
    return GetFileAttributesW(L".crivate") != INVALID_FILE_ATTRIBUTES;
}

bool folder_is_initialized() {
    CrivateHeader header{};
    const bool ok = read_header(&header);
    crypto_wipe(&header, sizeof(header));
    return ok;
}

bool folder_init(const uint8_t* password, size_t password_len) {
    if (password == nullptr || password_len == 0) {
        return false;
    }
    if (folder_marker_exists()) {
        return false;
    }

    CrivateHeader header{};
    memcpy(header.magic, "CRIVATE1", 8);
    header.version = kCrivateVersion;
    header.kdf_iters = kPbkdf2Iters;

    AesKey key{};
    uint8_t canary[kVerifierPlaintextBytes];
    memset(canary, kVerifierCanary, sizeof(canary));

    bool ok = false;
    if (crypto_random(header.salt, sizeof(header.salt)) &&
        crypto_random(header.verifier_nonce, sizeof(header.verifier_nonce)) &&
        crypto_derive_key(password, password_len, header.salt, sizeof(header.salt),
                          header.kdf_iters, &key) &&
        crypto_gcm_encrypt(key, header.verifier_nonce,
                           reinterpret_cast<const uint8_t*>(&header), kHeaderAadBytes,
                           canary, sizeof(canary), header.verifier_ciphertext,
                           header.verifier_tag) &&
        !folder_marker_exists() && folder_dir_state() == DirState::Empty) {
        wchar_t temp_path[MAX_PATH];
        if (GetTempFileNameW(L".", L"crv", 0, temp_path) != 0) {
            bool keep_temp = false;
            {
                FileHandle file(CreateFileW(temp_path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL, nullptr));
                if (file.valid()) {
                    DWORD written = 0;
                    if (WriteFile(file.h, &header, sizeof(header), &written, nullptr) &&
                        written == sizeof(header) && FlushFileBuffers(file.h)) {
                        ok = true;
                    }
                }
            }

            if (ok) {
                ok = MoveFileW(temp_path, L".crivate") != 0;
                if (ok) {
                    keep_temp = true;
                }
            }
            if (!keep_temp) {
                DeleteFileW(temp_path);
                ok = false;
            }
        }
    }

    crypto_wipe_key(&key);
    crypto_wipe(canary, sizeof(canary));
    crypto_wipe(&header, sizeof(header));
    return ok;
}

bool folder_unlock(const uint8_t* password, size_t password_len, AesKey* out_key) {
    if (out_key == nullptr) {
        return false;
    }
    crypto_wipe_key(out_key);

    if (password == nullptr || password_len == 0) {
        return false;
    }

    CrivateHeader header{};
    if (!read_header(&header)) {
        crypto_wipe(&header, sizeof(header));
        return false;
    }

    AesKey key{};
    uint8_t canary[kVerifierPlaintextBytes];

    bool ok = crypto_derive_key(password, password_len, header.salt, sizeof(header.salt),
                                header.kdf_iters, &key);
    if (ok) {
        ok = crypto_gcm_decrypt(key, header.verifier_nonce,
                                reinterpret_cast<const uint8_t*>(&header), kHeaderAadBytes,
                                header.verifier_ciphertext, sizeof(header.verifier_ciphertext),
                                header.verifier_tag, canary);
    }
    if (ok) {
        for (size_t i = 0; i < sizeof(canary); ++i) {
            if (canary[i] != kVerifierCanary) {
                ok = false;
                break;
            }
        }
    }

    if (ok) {
        memcpy(out_key->bytes, key.bytes, kAesKeyBytes);
    }

    crypto_wipe_key(&key);
    crypto_wipe(canary, sizeof(canary));
    crypto_wipe(&header, sizeof(header));
    if (!ok) {
        crypto_wipe_key(out_key);
    }
    return ok;
}

bool folder_list_cri(std::vector<std::wstring>* names) {
    if (names == nullptr) {
        return false;
    }
    names->clear();

    WIN32_FIND_DATAW fd{};
    FindHandle find(FindFirstFileW(L"*", &fd));
    if (!find.valid()) {
        return false;
    }

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (!name_has_cri_extension(fd.cFileName)) {
            continue;
        }
        names->emplace_back(fd.cFileName);
    } while (FindNextFileW(find.h, &fd));

    if (GetLastError() != ERROR_NO_MORE_FILES) {
        names->clear();
        return false;
    }

    std::sort(names->begin(), names->end(), cri_name_less);
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
