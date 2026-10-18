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
// The window is fitted once to the first image, inside 80% of the work area.
// Either client edge that would be shorter than half the smaller cap is raised
// to that floor, so a very wide or tall opener cannot collapse the window.
// Later images letterbox in that fixed client area.
ViewerStatus viewer_show(uint32_t width, uint32_t height, const uint8_t* rgb, size_t rgb_len,
                         ViewerLoadFn load_delta, void* user);
