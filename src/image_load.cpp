#include "image_load.h"

#include "cri.h"
#include "crypto.h"

#include <limits>
#include <new>
#include <vector>

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <wincodec.h>

namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    T** put() {
        reset();
        return &ptr_;
    }

    void reset() {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

struct ComScope {
    bool active = false;

    bool enter() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) {
            active = true;
            return true;
        }
        // Already initialized on this thread in a different mode; COM is still usable.
        return hr == RPC_E_CHANGED_MODE;
    }

    ~ComScope() {
        if (active) {
            CoUninitialize();
        }
    }
};

bool file_is_missing(const wchar_t* path) {
    const DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
               err == ERROR_INVALID_NAME;
    }
    return false;
}

bool file_is_directory(const wchar_t* path) {
    const DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

uint8_t blend_on_white(uint8_t channel, uint8_t alpha) {
    const uint32_t a = alpha;
    const uint32_t inv = 255u - a;
    return static_cast<uint8_t>((static_cast<uint32_t>(channel) * a + 255u * inv + 127u) / 255u);
}

// Where each stored pixel is written. Steps are in pixels.
// Phone cameras store a landscape buffer plus an EXIF orientation tag. WIC
// returns that buffer unchanged; these steps turn it upright.
struct PixelPlace {
    uint32_t out_width;
    uint32_t out_height;
    int64_t origin;
    int64_t x_step;
    int64_t y_step;
};

bool variant_orientation(const PROPVARIANT& value, uint16_t* out) {
    uint32_t n = 0;
    switch (value.vt) {
        case VT_UI2:
            n = value.uiVal;
            break;
        case VT_I2:
            if (value.iVal < 0) {
                return false;
            }
            n = static_cast<uint16_t>(value.iVal);
            break;
        case VT_UI4:
            n = value.ulVal;
            break;
        case VT_I4:
            if (value.lVal < 0) {
                return false;
            }
            n = static_cast<uint32_t>(value.lVal);
            break;
        default:
            return false;
    }
    if (n < 1 || n > 8) {
        return false;
    }
    *out = static_cast<uint16_t>(n);
    return true;
}

// EXIF tag 274. Missing or unrecognized means the stored pixels are already upright.
uint16_t read_orientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.put()))) {
        return 1;
    }

    static const wchar_t* kPaths[] = {
        L"/app1/ifd/{ushort=274}",
        L"/ifd/{ushort=274}",
    };
    for (const wchar_t* path : kPaths) {
        PROPVARIANT value;
        PropVariantInit(&value);
        const HRESULT hr = reader->GetMetadataByName(path, &value);
        uint16_t orientation = 1;
        const bool found = SUCCEEDED(hr) && variant_orientation(value, &orientation);
        PropVariantClear(&value);
        if (found) {
            return orientation;
        }
    }
    return 1;
}

PixelPlace pixel_place(uint16_t orientation, uint32_t width, uint32_t height) {
    const int64_t w = width;
    const int64_t h = height;
    switch (orientation) {
        case 2:  // flip horizontally
            return {width, height, w - 1, -1, w};
        case 3:  // rotate 180
            return {width, height, w * h - 1, -1, -w};
        case 4:  // flip vertically
            return {width, height, (h - 1) * w, 1, -w};
        case 5:  // transpose
            return {height, width, 0, h, 1};
        case 6:  // rotate 90 clockwise
            return {height, width, h - 1, h, -1};
        case 7:  // transverse
            return {height, width, (w - 1) * h + (h - 1), -h, -1};
        case 8:  // rotate 90 counter-clockwise
            return {height, width, (w - 1) * h, -h, 1};
        default:
            return {width, height, 0, 1, w};
    }
}

ImageLoadStatus decode_frame(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
                             RgbImage* out) {
    UINT width = 0;
    UINT height = 0;
    HRESULT hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) {
        return ImageLoadStatus::NotAnImage;
    }

    const uint16_t orientation = read_orientation(frame);
    uint32_t out_width = width;
    uint32_t out_height = height;
    if (orientation >= 5 && orientation <= 8) {
        out_width = height;
        out_height = width;
    }

    uint64_t rgb_bytes = 0;
    if (!cri_check_dimensions(out_width, out_height, &rgb_bytes)) {
        return ImageLoadStatus::TooLarge;
    }
    const PixelPlace place = pixel_place(orientation, width, height);

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.put());
    if (FAILED(hr)) {
        return ImageLoadStatus::IoError;
    }

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                               nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return ImageLoadStatus::NotAnImage;
    }

    const uint64_t pixels = rgb_bytes / 3u;
    const uint64_t bgra_bytes = pixels * 4u;
    if (bgra_bytes > (std::numeric_limits<UINT>::max)()) {
        return ImageLoadStatus::TooLarge;
    }

    std::vector<uint8_t> bgra;
    try {
        bgra.resize(static_cast<size_t>(bgra_bytes));
        out->rgb.resize(static_cast<size_t>(rgb_bytes));
    } catch (const std::bad_alloc&) {
        crypto_wipe(bgra.data(), bgra.size());
        out->wipe();
        return ImageLoadStatus::IoError;
    }

    const UINT stride = width * 4u;
    hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(bgra.size()), bgra.data());
    if (FAILED(hr)) {
        crypto_wipe(bgra.data(), bgra.size());
        out->wipe();
        return ImageLoadStatus::NotAnImage;
    }

    const uint8_t* src = bgra.data();
    uint8_t* dst_base = out->rgb.data();
    int64_t index = place.origin;
    for (uint32_t y = 0; y < height; ++y) {
        int64_t at = index;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t b = src[0];
            const uint8_t g = src[1];
            const uint8_t r = src[2];
            const uint8_t a = src[3];
            uint8_t* dst = dst_base + static_cast<uint64_t>(at) * 3u;
            dst[0] = blend_on_white(r, a);
            dst[1] = blend_on_white(g, a);
            dst[2] = blend_on_white(b, a);
            src += 4;
            at += place.x_step;
        }
        index += place.y_step;
    }

    crypto_wipe(bgra.data(), bgra.size());
    out->width = place.out_width;
    out->height = place.out_height;
    return ImageLoadStatus::Ok;
}

}  // namespace

RgbImage::~RgbImage() { wipe(); }

void RgbImage::wipe() {
    if (!rgb.empty()) {
        crypto_wipe(rgb.data(), rgb.size());
        rgb.clear();
        rgb.shrink_to_fit();
    }
    width = 0;
    height = 0;
}

ImageLoadStatus image_load_rgb(const wchar_t* path, RgbImage* out) {
    if (out == nullptr) {
        return ImageLoadStatus::IoError;
    }
    out->wipe();

    if (path == nullptr || path[0] == L'\0') {
        return ImageLoadStatus::NotFound;
    }
    if (file_is_missing(path)) {
        return ImageLoadStatus::NotFound;
    }
    if (file_is_directory(path)) {
        return ImageLoadStatus::NotAnImage;
    }

    ComScope com;
    if (!com.enter()) {
        return ImageLoadStatus::IoError;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWICImagingFactory, reinterpret_cast<void**>(factory.put()));
    if (FAILED(hr)) {
        return ImageLoadStatus::IoError;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, decoder.put());
    if (FAILED(hr)) {
        return ImageLoadStatus::NotAnImage;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.put());
    if (FAILED(hr)) {
        return ImageLoadStatus::NotAnImage;
    }

    return decode_frame(factory.get(), frame.get(), out);
}
