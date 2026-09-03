#pragma once

#include "crypto.h"

#include <cstdint>
#include <string>

constexpr uint32_t kCriVersion = 1;
constexpr uint32_t kCriMaxWidth = 16384;
constexpr uint32_t kCriMaxHeight = 16384;
constexpr uint64_t kCriMaxPixels = 50000000;
constexpr uint64_t kCriMaxPlaintextBytes = 150000000;
constexpr uint32_t kCriHeaderBytes = 44;

enum class CriWriteStatus {
    Ok,
    Exists,
    InvalidInput,
    CryptoError,
    IoError,
};

bool cri_check_dimensions(uint32_t width, uint32_t height, uint64_t* out_plaintext_bytes);

// `{stem}.cri` in cwd from an image path. Example: C:\pics\Holiday.JPG -> Holiday.cri
bool cri_dest_name(const wchar_t* image_path, std::wstring* dest_basename);

bool cri_exists(const wchar_t* dest_basename);

CriWriteStatus cri_write(const wchar_t* dest_basename, const AesKey& key, uint32_t width,
                         uint32_t height, const uint8_t* rgb, size_t rgb_len);
