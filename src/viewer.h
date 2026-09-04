#pragma once

#include <cstddef>
#include <cstdint>

enum class ViewerStatus {
    Ok,
    InvalidInput,
    DeviceError,
    OutOfMemory,
};

// Blocks until Esc or the window is closed. Does not write image files.
ViewerStatus viewer_show(uint32_t width, uint32_t height, const uint8_t* rgb, size_t rgb_len);
