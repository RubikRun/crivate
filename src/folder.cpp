#include "folder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
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

static_assert(sizeof(CrivateHeader) == kCrivateFileBytes, "folder header must be 92 bytes");
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

bool hex_digit(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

// GetTempFileNameW(".", prefix, 0) with a 3-letter prefix: prefix, 1 to 4 hex
// digits, then ".tmp". Short values are not zero-padded (criB.tmp, crpFFFF.tmp).
enum class WorkTempKind {
    None,
    Disposable,
    Parked,
};

WorkTempKind work_temp_kind(const wchar_t* name) {
    if (name == nullptr) {
        return WorkTempKind::None;
    }
    const size_t n = wcslen(name);
    if (n < 8 || n > 11) {
        return WorkTempKind::None;
    }
    if (CompareStringOrdinal(name + (n - 4), 4, L".tmp", 4, TRUE) != CSTR_EQUAL) {
        return WorkTempKind::None;
    }
    const size_t hex_len = n - 7;
    for (size_t i = 0; i < hex_len; ++i) {
        if (!hex_digit(name[3 + i])) {
            return WorkTempKind::None;
        }
    }
    if (CompareStringOrdinal(name, 3, L"crb", 3, TRUE) == CSTR_EQUAL) {
        return WorkTempKind::Parked;
    }
    if (CompareStringOrdinal(name, 3, L"cri", 3, TRUE) == CSTR_EQUAL ||
        CompareStringOrdinal(name, 3, L"crp", 3, TRUE) == CSTR_EQUAL ||
        CompareStringOrdinal(name, 3, L"crh", 3, TRUE) == CSTR_EQUAL) {
        return WorkTempKind::Disposable;
    }
    return WorkTempKind::None;
}

enum class DeleteTemp {
    Removed,
    Busy,
    Missing,
};

DeleteTemp exclusive_delete(const wchar_t* name) {
    const DWORD attr = GetFileAttributesW(name);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return DeleteTemp::Missing;
        }
        return DeleteTemp::Busy;
    }
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0 || (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return DeleteTemp::Busy;
    }

    const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE handle = CreateFileW(name, DELETE, 0, nullptr, OPEN_EXISTING, flags, nullptr);
    bool cleared_readonly = false;
    if (handle == INVALID_HANDLE_VALUE && (attr & FILE_ATTRIBUTE_READONLY) != 0 &&
        GetLastError() == ERROR_ACCESS_DENIED) {
        if (SetFileAttributesW(name, attr & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)) == 0) {
            return DeleteTemp::Busy;
        }
        cleared_readonly = true;
        handle = CreateFileW(name, DELETE, 0, nullptr, OPEN_EXISTING, flags, nullptr);
    }
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (cleared_readonly) {
            SetFileAttributesW(name, attr);
        }
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return DeleteTemp::Missing;
        }
        return DeleteTemp::Busy;
    }

    CloseHandle(handle);
    if (GetFileAttributesW(name) != INVALID_FILE_ATTRIBUTES) {
        return DeleteTemp::Busy;
    }
    return DeleteTemp::Removed;
}

bool collect_work_temps(std::vector<std::wstring>* disposable, std::vector<std::wstring>* parked) {
    WIN32_FIND_DATAW fd{};
    FindHandle find(FindFirstFileW(L"*.tmp", &fd));
    if (!find.valid()) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const WorkTempKind kind = work_temp_kind(fd.cFileName);
        if (kind == WorkTempKind::Disposable) {
            disposable->emplace_back(fd.cFileName);
        } else if (kind == WorkTempKind::Parked) {
            parked->emplace_back(fd.cFileName);
        }
    } while (FindNextFileW(find.h, &fd));

    return GetLastError() == ERROR_NO_MORE_FILES;
}

uint64_t fnv1a64(const wchar_t* text, size_t n) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) {
        hash ^= static_cast<uint16_t>(text[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

// Stable for concurrent processes in this folder. Not required to survive reboot.
bool activity_mutex_name(std::wstring* out) {
    if (out == nullptr) {
        return false;
    }
    out->clear();

    FileHandle dir(CreateFileW(L".", FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!dir.valid()) {
        return false;
    }

    std::wstring path(512, L'\0');
    DWORD n = 0;
    for (;;) {
        n = GetFinalPathNameByHandleW(dir.h, path.data(), static_cast<DWORD>(path.size()),
                                      VOLUME_NAME_NT);
        if (n == 0) {
            return false;
        }
        if (n < path.size()) {
            break;
        }
        if (n > 32768) {
            return false;
        }
        path.resize(static_cast<size_t>(n));
    }
    path.resize(n);

    std::wstring folded;
    const int src_n = static_cast<int>(path.size());
    const int mapped = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.c_str(), src_n,
                                     nullptr, 0, nullptr, nullptr, 0);
    if (mapped > 0) {
        folded.assign(static_cast<size_t>(mapped), L'\0');
        if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.c_str(), src_n, folded.data(),
                          mapped, nullptr, nullptr, 0) == mapped) {
            path.swap(folded);
        }
    }

    wchar_t name[40];
    const int wrote = _snwprintf_s(name, _TRUNCATE, L"Local\\crivate-%016llx",
                                   static_cast<unsigned long long>(fnv1a64(path.data(), path.size())));
    if (wrote <= 0) {
        return false;
    }
    out->assign(name);
    return true;
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

bool folder_kdf_iters(uint32_t* out) {
    if (out == nullptr) {
        return false;
    }

    CrivateHeader header{};
    if (!read_header(&header)) {
        crypto_wipe(&header, sizeof(header));
        return false;
    }
    *out = header.kdf_iters;
    crypto_wipe(&header, sizeof(header));
    return true;
}

void folder_wipe_rekey(FolderRekey* rekey) {
    if (rekey == nullptr) {
        return;
    }
    crypto_wipe_key(&rekey->key);
    crypto_wipe(rekey->header, sizeof(rekey->header));
}

bool folder_prepare_rekey(const uint8_t* password, size_t password_len, uint32_t kdf_iters,
                          FolderRekey* out) {
    if (out == nullptr) {
        return false;
    }
    folder_wipe_rekey(out);
    if (password == nullptr || password_len == 0 || kdf_iters < kMinKdfIters ||
        kdf_iters > kMaxKdfIters) {
        return false;
    }

    CrivateHeader header{};
    memcpy(header.magic, "CRIVATE1", 8);
    header.version = kCrivateVersion;
    header.kdf_iters = kdf_iters;

    AesKey key{};
    uint8_t canary[kVerifierPlaintextBytes];
    memset(canary, kVerifierCanary, sizeof(canary));

    const bool ok =
        crypto_random(header.salt, sizeof(header.salt)) &&
        crypto_random(header.verifier_nonce, sizeof(header.verifier_nonce)) &&
        crypto_derive_key(password, password_len, header.salt, sizeof(header.salt),
                          header.kdf_iters, &key) &&
        crypto_gcm_encrypt(key, header.verifier_nonce, reinterpret_cast<const uint8_t*>(&header),
                           kHeaderAadBytes, canary, sizeof(canary), header.verifier_ciphertext,
                           header.verifier_tag);
    if (ok) {
        memcpy(out->key.bytes, key.bytes, kAesKeyBytes);
        memcpy(out->header, &header, sizeof(header));
    }

    crypto_wipe_key(&key);
    crypto_wipe(canary, sizeof(canary));
    crypto_wipe(&header, sizeof(header));
    return ok;
}

static bool path_ends_with_cri(const wchar_t* path) {
    if (path == nullptr) {
        return false;
    }
    const size_t n = wcslen(path);
    if (n < 4) {
        return false;
    }
    return CompareStringOrdinal(path + (n - 4), 4, L".cri", 4, TRUE) == CSTR_EQUAL;
}

bool folder_write_rekey_temp(const FolderRekey& rekey, std::wstring* out_temp) {
    if (out_temp == nullptr) {
        return false;
    }
    out_temp->clear();

    wchar_t temp_path[MAX_PATH];
    if (GetTempFileNameW(L".", L"crh", 0, temp_path) == 0) {
        return false;
    }

    bool ok = false;
    {
        FileHandle file(CreateFileW(temp_path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.valid()) {
            DWORD written = 0;
            if (WriteFile(file.h, rekey.header, static_cast<DWORD>(sizeof(rekey.header)), &written,
                          nullptr) &&
                written == sizeof(rekey.header) && FlushFileBuffers(file.h)) {
                ok = true;
            }
        }
    }

    if (!ok || path_ends_with_cri(temp_path)) {
        DeleteFileW(temp_path);
        return false;
    }

    try {
        *out_temp = temp_path;
    } catch (const std::bad_alloc&) {
        DeleteFileW(temp_path);
        return false;
    }
    return true;
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

bool folder_delete_cri(const wchar_t* basename) {
    if (basename == nullptr || basename[0] == L'\0') {
        return false;
    }
    if (wcschr(basename, L'\\') != nullptr || wcschr(basename, L'/') != nullptr ||
        wcschr(basename, L':') != nullptr) {
        return false;
    }
    if (wcscmp(basename, L".") == 0 || wcscmp(basename, L"..") == 0) {
        return false;
    }
    if (!name_has_cri_extension(basename)) {
        return false;
    }

    const DWORD attr = GetFileAttributesW(basename);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    if (DeleteFileW(basename) != 0) {
        return true;
    }

    if ((attr & FILE_ATTRIBUTE_READONLY) == 0) {
        return false;
    }
    if (SetFileAttributesW(basename, attr & ~FILE_ATTRIBUTE_READONLY) == 0) {
        return false;
    }
    return DeleteFileW(basename) != 0;
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

FolderActivity::~FolderActivity() {
    release();
    if (mutex_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mutex_));
        mutex_ = nullptr;
    }
}

bool FolderActivity::open_mutex() {
    if (mutex_ != nullptr) {
        return true;
    }
    std::wstring name;
    if (!activity_mutex_name(&name)) {
        return false;
    }
    const HANDLE created = CreateMutexW(nullptr, FALSE, name.c_str());
    if (created == nullptr) {
        return false;
    }
    mutex_ = created;
    return true;
}

void FolderActivity::release() {
    if (!owned_) {
        return;
    }
    ReleaseMutex(static_cast<HANDLE>(mutex_));
    owned_ = false;
}

FolderActivityState FolderActivity::try_acquire() {
    if (owned_) {
        return FolderActivityState::Held;
    }
    if (!open_mutex()) {
        return FolderActivityState::Error;
    }
    const DWORD waited = WaitForSingleObject(static_cast<HANDLE>(mutex_), 0);
    if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
        owned_ = true;
        return FolderActivityState::Held;
    }
    if (waited == WAIT_TIMEOUT) {
        return FolderActivityState::Busy;
    }
    return FolderActivityState::Error;
}

FolderActivityState FolderActivity::acquire() {
    if (owned_) {
        return FolderActivityState::Held;
    }
    if (!open_mutex()) {
        return FolderActivityState::Error;
    }
    const DWORD waited = WaitForSingleObject(static_cast<HANDLE>(mutex_), INFINITE);
    if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
        owned_ = true;
        return FolderActivityState::Held;
    }
    return FolderActivityState::Error;
}

OrphanTempResult folder_reclaim_orphan_temps() {
    OrphanTempResult result;
    try {
        std::vector<std::wstring> disposable;
        std::vector<std::wstring> parked;
        if (!collect_work_temps(&disposable, &parked)) {
            return result;
        }
        std::sort(disposable.begin(), disposable.end(), cri_name_less);
        std::sort(parked.begin(), parked.end(), cri_name_less);
        result.scanned = true;
        for (const std::wstring& name : disposable) {
            const DeleteTemp deleted = exclusive_delete(name.c_str());
            if (deleted == DeleteTemp::Removed) {
                if (result.removed < 0xFFFFFFFFu) {
                    ++result.removed;
                }
            } else if (deleted == DeleteTemp::Busy) {
                result.busy.push_back(name);
            }
        }
        result.parked = std::move(parked);
    } catch (const std::bad_alloc&) {
        return result;
    }
    return result;
}
