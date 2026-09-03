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

bool read_all(HANDLE file, void* data, size_t n) {
    auto* p = static_cast<uint8_t*>(data);
    while (n > 0) {
        const DWORD chunk = n > static_cast<size_t>((std::numeric_limits<DWORD>::max)())
                                ? (std::numeric_limits<DWORD>::max)()
                                : static_cast<DWORD>(n);
        DWORD got = 0;
        if (!ReadFile(file, p, chunk, &got, nullptr) || got == 0) {
            return false;
        }
        p += got;
        n -= got;
    }
    return true;
}

bool mul_u64(uint64_t a, uint64_t b, uint64_t* out) {
    if (out == nullptr) {
        return false;
    }
    if (a != 0 && b > (std::numeric_limits<uint64_t>::max)() / a) {
        return false;
    }
    *out = a * b;
    return true;
}

void clear_rgb(uint32_t* width, uint32_t* height, std::vector<uint8_t>* rgb) {
    if (rgb != nullptr && !rgb->empty()) {
        crypto_wipe(rgb->data(), rgb->size());
        rgb->clear();
        rgb->shrink_to_fit();
    }
    if (width != nullptr) {
        *width = 0;
    }
    if (height != nullptr) {
        *height = 0;
    }
}

}  // namespace

bool cri_check_dimensions(uint32_t width, uint32_t height, uint64_t* out_plaintext_bytes) {
    if (width == 0 || height == 0) {
        return false;
    }
    if (width > kCriMaxWidth || height > kCriMaxHeight) {
        return false;
    }

    uint64_t pixels = 0;
    if (!mul_u64(width, height, &pixels) || pixels > kCriMaxPixels) {
        return false;
    }

    uint64_t bytes = 0;
    if (!mul_u64(pixels, 3, &bytes) || bytes > kCriMaxPlaintextBytes) {
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

CriReadStatus cri_read(const wchar_t* path, const AesKey& key, uint32_t* width, uint32_t* height,
                       std::vector<uint8_t>* rgb) {
    clear_rgb(width, height, rgb);

    if (path == nullptr || path[0] == L'\0' || width == nullptr || height == nullptr ||
        rgb == nullptr) {
        return CriReadStatus::InvalidInput;
    }

    FileHandle file(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) {
        return CriReadStatus::OpenError;
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file.h, &file_size) || file_size.QuadPart < 0) {
        return CriReadStatus::IoError;
    }
    if (static_cast<uint64_t>(file_size.QuadPart) < kCriHeaderBytes) {
        return CriReadStatus::InvalidFile;
    }

    CriHeader header{};
    if (!read_all(file.h, &header, sizeof(header))) {
        crypto_wipe(&header, sizeof(header));
        return CriReadStatus::IoError;
    }

    uint64_t payload_bytes = 0;
    const bool header_ok = memcmp(header.magic, "CRI1", 4) == 0 &&
                           header.version == kCriVersion &&
                           cri_check_dimensions(header.width, header.height, &payload_bytes);
    if (!header_ok) {
        crypto_wipe(&header, sizeof(header));
        return CriReadStatus::InvalidFile;
    }

    const uint64_t expected_size = static_cast<uint64_t>(kCriHeaderBytes) + payload_bytes;
    if (static_cast<uint64_t>(file_size.QuadPart) != expected_size) {
        crypto_wipe(&header, sizeof(header));
        return CriReadStatus::InvalidFile;
    }

    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> plaintext;
    try {
        ciphertext.resize(static_cast<size_t>(payload_bytes));
        plaintext.resize(static_cast<size_t>(payload_bytes));
    } catch (const std::bad_alloc&) {
        crypto_wipe(ciphertext.data(), ciphertext.size());
        crypto_wipe(plaintext.data(), plaintext.size());
        crypto_wipe(&header, sizeof(header));
        return CriReadStatus::OutOfMemory;
    }

    if (!read_all(file.h, ciphertext.data(), ciphertext.size())) {
        crypto_wipe(ciphertext.data(), ciphertext.size());
        crypto_wipe(plaintext.data(), plaintext.size());
        crypto_wipe(&header, sizeof(header));
        return CriReadStatus::IoError;
    }

    const uint32_t out_w = header.width;
    const uint32_t out_h = header.height;

    const bool ok = crypto_gcm_decrypt(key, header.nonce, reinterpret_cast<const uint8_t*>(&header),
                                       kHeaderAadBytes, ciphertext.data(), ciphertext.size(),
                                       header.tag, plaintext.data());
    crypto_wipe(ciphertext.data(), ciphertext.size());
    ciphertext.clear();
    crypto_wipe(&header, sizeof(header));

    if (!ok) {
        crypto_wipe(plaintext.data(), plaintext.size());
        return CriReadStatus::AuthFailed;
    }

    *width = out_w;
    *height = out_h;
    rgb->swap(plaintext);
    return CriReadStatus::Ok;
}
