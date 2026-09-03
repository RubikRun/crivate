#pragma once

#include <cstdint>
#include <vector>

enum class ImageLoadStatus {
    Ok,
    NotFound,
    NotAnImage,
    TooLarge,
    IoError,
};

struct RgbImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;

    RgbImage() = default;
    RgbImage(const RgbImage&) = delete;
    RgbImage& operator=(const RgbImage&) = delete;
    ~RgbImage();

    void wipe();
};

ImageLoadStatus image_load_rgb(const wchar_t* path, RgbImage* out);
