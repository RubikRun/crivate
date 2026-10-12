#pragma once

#include <cstddef>
#include <cstdint>

enum class ViewerStatus {
    Ok,
    InvalidInput,
    DeviceError,
    OutOfMemory,
};

// delta is -1 or +1. Return false to keep the current image.
// On true, *rgb is the new packed RGB. The previous buffer is no longer valid.
using ViewerLoadFn = bool (*)(void* user, int delta, uint32_t* width, uint32_t* height,
                              const uint8_t** rgb, size_t* rgb_len);

// Blocks until Esc or the window is closed. Does not write image files.
// The window size follows the first image. Later images letterbox in that window.
ViewerStatus viewer_show(uint32_t width, uint32_t height, const uint8_t* rgb, size_t rgb_len,
                         ViewerLoadFn load_delta, void* user);
