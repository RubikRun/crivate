#include "image_load.h"

#include "cri.h"
#include "crypto.h"

#include <limits>
#include <new>
#include <vector>

#include <windows.h>
#include <objbase.h>
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

ImageLoadStatus decode_frame(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
                             RgbImage* out) {
    UINT width = 0;
    UINT height = 0;
    HRESULT hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) {
        return ImageLoadStatus::NotAnImage;
    }

    uint64_t rgb_bytes = 0;
    if (!cri_check_dimensions(width, height, &rgb_bytes)) {
        return ImageLoadStatus::TooLarge;
    }

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

    const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
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

    uint8_t* dst = out->rgb.data();
    const uint8_t* src = bgra.data();
    for (uint64_t i = 0; i < pixels; ++i) {
        const uint8_t b = src[0];
        const uint8_t g = src[1];
        const uint8_t r = src[2];
        const uint8_t a = src[3];
        dst[0] = blend_on_white(r, a);
        dst[1] = blend_on_white(g, a);
        dst[2] = blend_on_white(b, a);
        src += 4;
        dst += 3;
    }

    crypto_wipe(bgra.data(), bgra.size());
    out->width = width;
    out->height = height;
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
