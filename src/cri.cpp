#include "cri.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include <windows.h>

namespace {

#pragma pack(push, 1)
struct CriHeader {
    char magic[4];
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint8_t nonce[kGcmNonceBytes];
    uint8_t tag[kGcmTagBytes];
};
#pragma pack(pop)

static_assert(sizeof(CriHeader) == kCriHeaderBytes, "cri header must be 44 bytes");
static_assert(offsetof(CriHeader, nonce) == 16, "AAD is magic + version + width + height");

constexpr size_t kHeaderAadBytes = offsetof(CriHeader, nonce);

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

const wchar_t* file_name(const wchar_t* path) {
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            base = p + 1;
        }
    }
    return base;
}

bool write_all(HANDLE file, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (n > 0) {
        const DWORD chunk = n > static_cast<size_t>((std::numeric_limits<DWORD>::max)())
                                ? (std::numeric_limits<DWORD>::max)()
                                : static_cast<DWORD>(n);
        DWORD written = 0;
        if (!WriteFile(file, p, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        p += written;
        n -= written;
    }
    return true;
}

}  // namespace

bool cri_check_dimensions(uint32_t width, uint32_t height, uint64_t* out_plaintext_bytes) {
    if (width == 0 || height == 0) {
        return false;
    }
    if (width > kCriMaxWidth || height > kCriMaxHeight) {
        return false;
    }

    const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixels > kCriMaxPixels) {
        return false;
    }

    const uint64_t bytes = pixels * 3u;
    if (bytes > kCriMaxPlaintextBytes) {
        return false;
    }

    if (out_plaintext_bytes != nullptr) {
        *out_plaintext_bytes = bytes;
    }
    return true;
}

bool cri_dest_name(const wchar_t* image_path, std::wstring* dest_basename) {
    if (image_path == nullptr || dest_basename == nullptr || image_path[0] == L'\0') {
        return false;
    }

    const wchar_t* base = file_name(image_path);
    if (base[0] == L'\0' || wcscmp(base, L".") == 0 || wcscmp(base, L"..") == 0) {
        return false;
    }

    const wchar_t* dot = wcsrchr(base, L'.');
    const size_t stem_len = (dot != nullptr && dot != base) ? static_cast<size_t>(dot - base)
                                                            : wcslen(base);
    if (stem_len == 0 || stem_len > 250) {
        return false;
    }

    dest_basename->assign(base, stem_len);
    dest_basename->append(L".cri");
    return true;
}

bool cri_exists(const wchar_t* dest_basename) {
    if (dest_basename == nullptr || dest_basename[0] == L'\0') {
        return false;
    }
    return GetFileAttributesW(dest_basename) != INVALID_FILE_ATTRIBUTES;
}

CriWriteStatus cri_write(const wchar_t* dest_basename, const AesKey& key, uint32_t width,
                         uint32_t height, const uint8_t* rgb, size_t rgb_len) {
    if (dest_basename == nullptr || dest_basename[0] == L'\0' || rgb == nullptr) {
        return CriWriteStatus::InvalidInput;
    }
    if (wcschr(dest_basename, L'\\') != nullptr || wcschr(dest_basename, L'/') != nullptr) {
        return CriWriteStatus::InvalidInput;
    }

    uint64_t expected = 0;
    if (!cri_check_dimensions(width, height, &expected) || rgb_len != expected) {
        return CriWriteStatus::InvalidInput;
    }

    if (cri_exists(dest_basename)) {
        return CriWriteStatus::Exists;
    }

    CriHeader header{};
    memcpy(header.magic, "CRI1", 4);
    header.version = kCriVersion;
    header.width = width;
    header.height = height;

    std::vector<uint8_t> ciphertext;
    try {
        ciphertext.resize(rgb_len);
    } catch (const std::bad_alloc&) {
        return CriWriteStatus::IoError;
    }

    if (!crypto_random(header.nonce, sizeof(header.nonce))) {
        crypto_wipe(ciphertext.data(), ciphertext.size());
        return CriWriteStatus::CryptoError;
    }

    if (!crypto_gcm_encrypt(key, header.nonce, reinterpret_cast<const uint8_t*>(&header),
                            kHeaderAadBytes, rgb, rgb_len, ciphertext.data(), header.tag)) {
        crypto_wipe(ciphertext.data(), ciphertext.size());
        crypto_wipe(&header, sizeof(header));
        return CriWriteStatus::CryptoError;
    }

    wchar_t temp_path[MAX_PATH];
    if (GetTempFileNameW(L".", L"cri", 0, temp_path) == 0) {
        crypto_wipe(ciphertext.data(), ciphertext.size());
        crypto_wipe(&header, sizeof(header));
        return CriWriteStatus::IoError;
    }

    bool keep_temp = false;
    bool ok = false;
    {
        FileHandle file(CreateFileW(temp_path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (file.valid()) {
            ok = write_all(file.h, &header, sizeof(header)) &&
                 write_all(file.h, ciphertext.data(), ciphertext.size()) &&
                 FlushFileBuffers(file.h);
        }
    }

    crypto_wipe(ciphertext.data(), ciphertext.size());
    ciphertext.clear();
    crypto_wipe(&header, sizeof(header));

    if (ok) {
        if (cri_exists(dest_basename)) {
            DeleteFileW(temp_path);
            return CriWriteStatus::Exists;
        }
        ok = MoveFileW(temp_path, dest_basename) != 0;
        if (ok) {
            keep_temp = true;
        }
    }

    if (!keep_temp) {
        DeleteFileW(temp_path);
        return CriWriteStatus::IoError;
    }
    return CriWriteStatus::Ok;
}
