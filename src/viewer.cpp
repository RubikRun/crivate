#include "viewer.h"

#include "cri.h"
#include "crypto.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

namespace {

constexpr UINT kDrawBatches = 100;
constexpr uint32_t kNStepPercent = 20;
constexpr int kWorkAreaPercent = 80;
constexpr wchar_t kViewerClass[] = L"crivate.Viewer.1";

const char kShaderSrc[] = R"(
struct VSIn {
    float2 corner : POSITION;
    float4 rect   : TEXCOORD;
    float4 color  : COLOR;
};

struct VSOut {
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

VSOut VSMain(VSIn input) {
    VSOut output;
    output.pos = float4(input.rect.xy + input.rect.zw * input.corner, 0.0f, 1.0f);
    output.color = input.color;
    return output;
}

float4 PSMain(VSOut input) : SV_TARGET {
    return input.color;
}
)";

struct SquareInstance {
    float x;
    float y;
    float w;
    float h;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

static_assert(sizeof(SquareInstance) == 20, "instance stride must stay packed at 20 bytes");

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

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

struct ViewerContext {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    bool class_registered = false;
    bool running = false;

    UINT client_w = 1;
    UINT client_h = 1;
    double letter_x = 0.0;
    double letter_y = 0.0;
    double letter_w = 1.0;
    double letter_h = 1.0;

    uint32_t img_w = 0;
    uint32_t img_h = 0;
    uint32_t n_max = 1;
    uint32_t target_n = 1;
    uint32_t grid_cols = 0;
    uint32_t grid_rows = 0;
    const uint8_t* rgb = nullptr;
    bool have_grid = false;
    bool grid_dirty = false;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> quad_vb;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth_off;
    ComPtr<ID3D11Buffer> batches[kDrawBatches];
    UINT batch_counts[kDrawBatches]{};

    ViewerContext() = default;
    ViewerContext(const ViewerContext&) = delete;
    ViewerContext& operator=(const ViewerContext&) = delete;

    ~ViewerContext() { shutdown(); }

    void shutdown() {
        if (context) {
            context->ClearState();
            context->Flush();
        }
        rtv.reset();
        for (auto& batch : batches) {
            batch.reset();
        }
        quad_vb.reset();
        layout.reset();
        vs.reset();
        ps.reset();
        raster.reset();
        depth_off.reset();
        swapchain.reset();
        context.reset();
        device.reset();

        if (hwnd != nullptr) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        if (class_registered && instance != nullptr) {
            UnregisterClassW(kViewerClass, instance);
            class_registered = false;
        }
    }
};

LRESULT CALLBACK viewer_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    auto* viewer = reinterpret_cast<ViewerContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (viewer == nullptr) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
        case WM_CLOSE:
            viewer->running = false;
            return 0;
        case WM_KEYDOWN: {
            const bool repeat = (lparam & (1u << 30)) != 0;
            if (wparam == VK_ESCAPE) {
                viewer->running = false;
                return 0;
            }
            if (!repeat && (wparam == 'W' || wparam == 'S')) {
                const uint64_t n = viewer->target_n;
                uint32_t delta = static_cast<uint32_t>((n * kNStepPercent) / 100u);
                if (delta < 1u) {
                    delta = 1u;
                }
                uint32_t next = viewer->target_n;
                if (wparam == 'W') {
                    const uint64_t up = static_cast<uint64_t>(viewer->target_n) + delta;
                    next = up > viewer->n_max ? viewer->n_max : static_cast<uint32_t>(up);
                } else if (viewer->target_n <= delta) {
                    next = 1;
                } else {
                    next = viewer->target_n - delta;
                }
                if (next != viewer->target_n) {
                    viewer->target_n = next;
                    viewer->grid_dirty = true;
                }
                return 0;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            viewer->running = false;
            viewer->hwnd = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void enable_dpi_awareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        return;
    }

    using SetDpiContextFn = BOOL(WINAPI*)(HANDLE);
    auto set_ctx = reinterpret_cast<SetDpiContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_ctx != nullptr) {
        set_ctx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));
        return;
    }

    using SetDpiAwareFn = BOOL(WINAPI*)();
    auto set_aware =
        reinterpret_cast<SetDpiAwareFn>(GetProcAddress(user32, "SetProcessDPIAware"));
    if (set_aware != nullptr) {
        set_aware();
    }
}

bool work_area(RECT* out) {
    HWND console = GetConsoleWindow();
    HMONITOR monitor = MonitorFromWindow(console, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
        *out = info.rcWork;
        return true;
    }

    RECT fallback{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0)) {
        *out = fallback;
        return true;
    }

    const int w = GetSystemMetrics(SM_CXSCREEN);
    const int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) {
        return false;
    }
    out->left = 0;
    out->top = 0;
    out->right = w;
    out->bottom = h;
    return true;
}

bool fit_client_size(uint32_t img_w, uint32_t img_h, int max_w, int max_h, int* out_w,
                     int* out_h) {
    if (out_w == nullptr || out_h == nullptr || max_w < 1 || max_h < 1) {
        return false;
    }

    const uint64_t iw = img_w;
    const uint64_t ih = img_h;
    uint64_t cw = static_cast<uint64_t>(max_w);
    uint64_t ch = cw * ih / iw;
    if (ch == 0) {
        ch = 1;
    }
    if (ch > static_cast<uint64_t>(max_h)) {
        ch = static_cast<uint64_t>(max_h);
        cw = ch * iw / ih;
        if (cw == 0) {
            cw = 1;
        }
    }
    if (cw > static_cast<uint64_t>(max_w)) {
        cw = static_cast<uint64_t>(max_w);
    }

    *out_w = static_cast<int>(cw);
    *out_h = static_cast<int>(ch);
    return *out_w >= 1 && *out_h >= 1;
}

void batch_range(uint32_t batch, uint32_t count, uint32_t* begin, uint32_t* end) {
    *begin = static_cast<uint32_t>((static_cast<uint64_t>(batch) * count) / kDrawBatches);
    *end = static_cast<uint32_t>((static_cast<uint64_t>(batch + 1) * count) / kDrawBatches);
}

uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

uint32_t round_to_u32_min1(double x) {
    if (!(x > 0.0)) {
        return 1;
    }
    const double r = std::round(x);
    if (r < 1.0) {
        return 1;
    }
    const double max_u = static_cast<double>((std::numeric_limits<uint32_t>::max)());
    if (r >= max_u) {
        return (std::numeric_limits<uint32_t>::max)();
    }
    return static_cast<uint32_t>(r);
}

void choose_grid(uint32_t n, uint32_t img_w, uint32_t img_h, uint32_t n_max, uint32_t* cols,
                 uint32_t* rows) {
    if (n >= n_max) {
        *cols = img_w;
        *rows = img_h;
        return;
    }

    const double inner =
        static_cast<double>(n) * static_cast<double>(img_w) / static_cast<double>(img_h);
    uint32_t c = round_to_u32_min1(std::sqrt(inner));
    c = clamp_u32(c, 1, img_w);
    uint32_t r = round_to_u32_min1(static_cast<double>(n) / static_cast<double>(c));
    r = clamp_u32(r, 1, img_h);
    *cols = c;
    *rows = r;
}

bool choose_grid_self_check() {
    uint32_t c = 0;
    uint32_t r = 0;
    choose_grid(12, 4, 3, 12, &c, &r);
    if (c != 4 || r != 3) {
        return false;
    }
    choose_grid(6, 4, 3, 12, &c, &r);
    if (c != 3 || r != 2) {
        return false;
    }
    choose_grid(1, 4, 3, 12, &c, &r);
    if (c != 1 || r != 1) {
        return false;
    }
    choose_grid(2073600, 1920, 1080, 2073600, &c, &r);
    return c == 1920 && r == 1080;
}

void cell_bounds(uint32_t index, uint32_t divisions, uint32_t extent, uint32_t* a0, uint32_t* a1) {
    *a0 = static_cast<uint32_t>((static_cast<uint64_t>(index) * extent) / divisions);
    *a1 = static_cast<uint32_t>(((static_cast<uint64_t>(index) + 1u) * extent) / divisions);
    if (*a0 >= *a1) {
        if (*a0 >= extent) {
            *a0 = extent - 1;
        }
        *a1 = *a0 + 1;
    }
}

void cell_average(const uint8_t* rgb, uint32_t width, uint32_t x0, uint32_t x1, uint32_t y0,
                  uint32_t y1, uint8_t* r, uint8_t* g, uint8_t* b) {
    uint64_t sr = 0;
    uint64_t sg = 0;
    uint64_t sb = 0;
    uint64_t n = 0;
    for (uint32_t y = y0; y < y1; ++y) {
        const uint8_t* px = rgb + (static_cast<size_t>(y) * width + x0) * 3u;
        for (uint32_t x = x0; x < x1; ++x) {
            sr += px[0];
            sg += px[1];
            sb += px[2];
            px += 3;
            ++n;
        }
    }
    if (n == 0) {
        *r = 0;
        *g = 0;
        *b = 0;
        return;
    }
    *r = static_cast<uint8_t>((sr + n / 2u) / n);
    *g = static_cast<uint8_t>((sg + n / 2u) / n);
    *b = static_cast<uint8_t>((sb + n / 2u) / n);
}

void compute_letterbox(ViewerContext* viewer, uint32_t img_w, uint32_t img_h) {
    const double client_w = static_cast<double>(viewer->client_w);
    const double client_h = static_cast<double>(viewer->client_h);
    const double iw = static_cast<double>(img_w);
    const double ih = static_cast<double>(img_h);
    const double img_aspect = iw / ih;
    const double win_aspect = client_w / client_h;
    if (win_aspect > img_aspect) {
        viewer->letter_h = client_h;
        viewer->letter_w = viewer->letter_h * img_aspect;
        viewer->letter_x = 0.5 * (client_w - viewer->letter_w);
        viewer->letter_y = 0.0;
    } else {
        viewer->letter_w = client_w;
        viewer->letter_h = viewer->letter_w / img_aspect;
        viewer->letter_x = 0.0;
        viewer->letter_y = 0.5 * (client_h - viewer->letter_h);
    }
}

HRESULT create_immutable_vb(ID3D11Device* device, const void* data, UINT bytes,
                            ID3D11Buffer** out) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    return device->CreateBuffer(&desc, &init, out);
}

bool create_window(ViewerContext* viewer, uint32_t img_w, uint32_t img_h) {
    viewer->instance = GetModuleHandleW(nullptr);
    if (viewer->instance == nullptr) {
        return false;
    }

    RECT work{};
    if (!work_area(&work)) {
        return false;
    }

    const int avail_w = work.right - work.left;
    const int avail_h = work.bottom - work.top;
    if (avail_w < 1 || avail_h < 1) {
        return false;
    }

    int max_w = static_cast<int>((static_cast<long long>(avail_w) * kWorkAreaPercent) / 100);
    int max_h = static_cast<int>((static_cast<long long>(avail_h) * kWorkAreaPercent) / 100);
    if (max_w < 1) {
        max_w = 1;
    }
    if (max_h < 1) {
        max_h = 1;
    }

    int client_w = 0;
    int client_h = 0;
    if (!fit_client_size(img_w, img_h, max_w, max_h, &client_w, &client_h)) {
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = viewer_wndproc;
    wc.hInstance = viewer->instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kViewerClass;
    ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        UnregisterClassW(kViewerClass, viewer->instance);
        atom = RegisterClassExW(&wc);
    }
    if (atom == 0) {
        return false;
    }
    viewer->class_registered = true;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD ex_style = WS_EX_APPWINDOW;

    RECT wr = {0, 0, client_w, client_h};
    if (!AdjustWindowRectEx(&wr, style, FALSE, ex_style)) {
        return false;
    }

    const int win_w = wr.right - wr.left;
    const int win_h = wr.bottom - wr.top;
    const int x = work.left + (avail_w - win_w) / 2;
    const int y = work.top + (avail_h - win_h) / 2;

    viewer->hwnd = CreateWindowExW(ex_style, kViewerClass, L"crivate", style, x, y, win_w, win_h,
                                   nullptr, nullptr, viewer->instance, viewer);
    if (viewer->hwnd == nullptr) {
        return false;
    }

    RECT cr{};
    if (!GetClientRect(viewer->hwnd, &cr)) {
        return false;
    }
    const int actual_w = cr.right - cr.left;
    const int actual_h = cr.bottom - cr.top;
    viewer->client_w = actual_w > 0 ? static_cast<UINT>(actual_w) : 1u;
    viewer->client_h = actual_h > 0 ? static_cast<UINT>(actual_h) : 1u;
    return true;
}

ViewerStatus create_device(ViewerContext* viewer) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = viewer->client_w;
    sd.BufferDesc.Height = viewer->client_h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = viewer->hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    auto try_create = [&](D3D_DRIVER_TYPE type, UINT create_flags) -> HRESULT {
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
        return D3D11CreateDeviceAndSwapChain(
            nullptr, type, nullptr, create_flags, levels, 1, D3D11_SDK_VERSION, &sd,
            viewer->swapchain.put(), viewer->device.put(), &got, viewer->context.put());
    };

    HRESULT hr = try_create(D3D_DRIVER_TYPE_HARDWARE, flags);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = try_create(D3D_DRIVER_TYPE_HARDWARE, flags);
    }
    if (FAILED(hr)) {
        hr = try_create(D3D_DRIVER_TYPE_WARP, flags);
    }
    if (FAILED(hr)) {
        return ViewerStatus::DeviceError;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    if (SUCCEEDED(viewer->device->QueryInterface(__uuidof(IDXGIDevice),
                                                 reinterpret_cast<void**>(dxgi_device.put())))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgi_device->GetAdapter(adapter.put()))) {
            ComPtr<IDXGIFactory> factory;
            if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory),
                                             reinterpret_cast<void**>(factory.put())))) {
                factory->MakeWindowAssociation(viewer->hwnd, DXGI_MWA_NO_ALT_ENTER);
            }
        }
    }

    ComPtr<ID3D11Texture2D> back_buffer;
    hr = viewer->swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                      reinterpret_cast<void**>(back_buffer.put()));
    if (FAILED(hr)) {
        return ViewerStatus::DeviceError;
    }
    hr = viewer->device->CreateRenderTargetView(back_buffer.get(), nullptr, viewer->rtv.put());
    if (FAILED(hr)) {
        return ViewerStatus::DeviceError;
    }
    return ViewerStatus::Ok;
}

ViewerStatus create_pipeline(ViewerContext* viewer) {
    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    ComPtr<ID3DBlob> errors;

    UINT compile_flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compile_flags |= D3DCOMPILE_DEBUG;
#else
    compile_flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr =
        D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr, "VSMain",
                   "vs_5_0", compile_flags, 0, vs_blob.put(), errors.put());
    if (FAILED(hr)) {
#if defined(_DEBUG)
        if (errors) {
            fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        }
#endif
        return ViewerStatus::DeviceError;
    }

    errors.reset();
    hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, nullptr, nullptr, nullptr, "PSMain",
                    "ps_5_0", compile_flags, 0, ps_blob.put(), errors.put());
    if (FAILED(hr)) {
#if defined(_DEBUG)
        if (errors) {
            fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        }
#endif
        return ViewerStatus::DeviceError;
    }

    hr = viewer->device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                            nullptr, viewer->vs.put());
    if (FAILED(hr)) {
        return ViewerStatus::DeviceError;
    }
    hr = viewer->device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                           nullptr, viewer->ps.put());
    if (FAILED(hr)) {
        return ViewerStatus::DeviceError;
    }

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    hr = viewer->device->CreateInputLayout(layout, 3, vs_blob->GetBufferPointer(),
                                           vs_blob->GetBufferSize(), viewer->layout.put());
    if (FAILED(hr)) {
        return ViewerStatus::DeviceError;
    }

    const float quad[] = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f,
    };
    hr = create_immutable_vb(viewer->device.get(), quad, sizeof(quad), viewer->quad_vb.put());
    if (FAILED(hr)) {
        return ViewerStatus::DeviceError;
    }

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    hr = viewer->device->CreateRasterizerState(&rs, viewer->raster.put());
    if (FAILED(hr)) {
        return ViewerStatus::DeviceError;
    }

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    ds.StencilEnable = FALSE;
    hr = viewer->device->CreateDepthStencilState(&ds, viewer->depth_off.put());
    if (FAILED(hr)) {
        return ViewerStatus::DeviceError;
    }

    return ViewerStatus::Ok;
}

ViewerStatus upload_grid(ViewerContext* viewer, uint32_t cols, uint32_t rows,
                         ComPtr<ID3D11Buffer> out_batches[kDrawBatches], UINT out_counts[kDrawBatches]) {
    const uint64_t square_count64 = static_cast<uint64_t>(cols) * static_cast<uint64_t>(rows);
    if (square_count64 == 0 || square_count64 > (std::numeric_limits<uint32_t>::max)()) {
        return ViewerStatus::InvalidInput;
    }
    const uint32_t count = static_cast<uint32_t>(square_count64);

    const double client_w = static_cast<double>(viewer->client_w);
    const double client_h = static_cast<double>(viewer->client_h);
    const double img_w = static_cast<double>(viewer->img_w);
    const double img_h = static_cast<double>(viewer->img_h);

    auto fill_instance = [&](uint32_t index, SquareInstance* out) {
        const uint32_t c = index % cols;
        const uint32_t r = index / cols;
        uint32_t x0 = 0;
        uint32_t x1 = 0;
        uint32_t y0 = 0;
        uint32_t y1 = 0;
        cell_bounds(c, cols, viewer->img_w, &x0, &x1);
        cell_bounds(r, rows, viewer->img_h, &y0, &y1);

        const double left = viewer->letter_x + static_cast<double>(x0) * viewer->letter_w / img_w;
        const double right = viewer->letter_x + static_cast<double>(x1) * viewer->letter_w / img_w;
        const double top = viewer->letter_y + static_cast<double>(y0) * viewer->letter_h / img_h;
        const double bottom = viewer->letter_y + static_cast<double>(y1) * viewer->letter_h / img_h;

        out->x = static_cast<float>(2.0 * left / client_w - 1.0);
        out->y = static_cast<float>(1.0 - 2.0 * bottom / client_h);
        out->w = static_cast<float>(2.0 * (right - left) / client_w);
        out->h = static_cast<float>(2.0 * (bottom - top) / client_h);

        cell_average(viewer->rgb, viewer->img_w, x0, x1, y0, y1, &out->r, &out->g, &out->b);
        out->a = 255;
    };

    for (uint32_t batch = 0; batch < kDrawBatches; ++batch) {
        uint32_t begin = 0;
        uint32_t end = 0;
        batch_range(batch, count, &begin, &end);
        const uint32_t n = end - begin;
        out_counts[batch] = n;
        if (n == 0) {
            continue;
        }

        std::vector<SquareInstance> cpu;
        try {
            cpu.resize(n);
        } catch (const std::bad_alloc&) {
            return ViewerStatus::OutOfMemory;
        }

        for (uint32_t i = 0; i < n; ++i) {
            fill_instance(begin + i, &cpu[i]);
        }

        const UINT bytes = static_cast<UINT>(n * sizeof(SquareInstance));
        const HRESULT hr =
            create_immutable_vb(viewer->device.get(), cpu.data(), bytes, out_batches[batch].put());
        crypto_wipe(cpu.data(), cpu.size() * sizeof(SquareInstance));
        if (FAILED(hr)) {
            return hr == E_OUTOFMEMORY ? ViewerStatus::OutOfMemory : ViewerStatus::DeviceError;
        }
    }

    return ViewerStatus::Ok;
}

ViewerStatus rebuild_grid(ViewerContext* viewer) {
    uint32_t cols = 0;
    uint32_t rows = 0;
    choose_grid(viewer->target_n, viewer->img_w, viewer->img_h, viewer->n_max, &cols, &rows);

#if defined(_DEBUG)
    if (viewer->target_n >= viewer->n_max && (cols != viewer->img_w || rows != viewer->img_h)) {
        return ViewerStatus::InvalidInput;
    }
#endif

    if (viewer->have_grid && cols == viewer->grid_cols && rows == viewer->grid_rows) {
        return ViewerStatus::Ok;
    }

    ComPtr<ID3D11Buffer> new_batches[kDrawBatches];
    UINT new_counts[kDrawBatches]{};
    const ViewerStatus status = upload_grid(viewer, cols, rows, new_batches, new_counts);
    if (status != ViewerStatus::Ok) {
        return status;
    }

    for (uint32_t i = 0; i < kDrawBatches; ++i) {
        viewer->batches[i] = std::move(new_batches[i]);
        viewer->batch_counts[i] = new_counts[i];
    }
    viewer->grid_cols = cols;
    viewer->grid_rows = rows;
    viewer->have_grid = true;
    return ViewerStatus::Ok;
}

bool render_frame(ViewerContext* viewer) {
    ID3D11DeviceContext* ctx = viewer->context.get();
    ID3D11RenderTargetView* rtv = viewer->rtv.get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(viewer->client_w);
    vp.Height = static_cast<float>(viewer->client_h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, clear);

    ctx->IASetInputLayout(viewer->layout.get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(viewer->vs.get(), nullptr, 0);
    ctx->PSSetShader(viewer->ps.get(), nullptr, 0);
    ctx->RSSetState(viewer->raster.get());
    ctx->OMSetDepthStencilState(viewer->depth_off.get(), 0);

    ID3D11Buffer* quad = viewer->quad_vb.get();
    const UINT quad_stride = 2u * sizeof(float);
    const UINT offset0 = 0;
    ctx->IASetVertexBuffers(0, 1, &quad, &quad_stride, &offset0);

    const UINT inst_stride = sizeof(SquareInstance);
#if defined(_DEBUG)
    UINT draws = 0;
#endif
    for (UINT i = 0; i < kDrawBatches; ++i) {
        const UINT n = viewer->batch_counts[i];
        if (n > 0) {
            ID3D11Buffer* inst = viewer->batches[i].get();
            const UINT offset1 = 0;
            ctx->IASetVertexBuffers(1, 1, &inst, &inst_stride, &offset1);
        }
        ctx->DrawInstanced(6, n, 0, 0);
#if defined(_DEBUG)
        ++draws;
#endif
    }
#if defined(_DEBUG)
    assert(draws == kDrawBatches);
#endif

    const HRESULT hr = viewer->swapchain->Present(1, 0);
    if (hr == DXGI_STATUS_OCCLUDED) {
        Sleep(16);
        return true;
    }
    return SUCCEEDED(hr);
}

}  // namespace

ViewerStatus viewer_show(uint32_t width, uint32_t height, const uint8_t* rgb, size_t rgb_len) {
    uint64_t expected = 0;
    if (!cri_check_dimensions(width, height, &expected) || rgb == nullptr ||
        rgb_len != expected) {
        return ViewerStatus::InvalidInput;
    }
    if (!choose_grid_self_check()) {
        return ViewerStatus::InvalidInput;
    }

    enable_dpi_awareness();

    ViewerContext viewer;
    if (!create_window(&viewer, width, height)) {
        return ViewerStatus::DeviceError;
    }

    viewer.img_w = width;
    viewer.img_h = height;
    viewer.rgb = rgb;
    viewer.n_max = static_cast<uint32_t>(static_cast<uint64_t>(width) * static_cast<uint64_t>(height));
    viewer.target_n = viewer.n_max;
    compute_letterbox(&viewer, width, height);

    ViewerStatus status = create_device(&viewer);
    if (status != ViewerStatus::Ok) {
        return status;
    }
    status = create_pipeline(&viewer);
    if (status != ViewerStatus::Ok) {
        return status;
    }
    status = rebuild_grid(&viewer);
    if (status != ViewerStatus::Ok) {
        return status;
    }

    if (!render_frame(&viewer)) {
        return ViewerStatus::DeviceError;
    }
    viewer.running = true;
    ShowWindow(viewer.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(viewer.hwnd);

    ViewerStatus loop_error = ViewerStatus::Ok;
    while (viewer.running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                viewer.running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!viewer.running) {
            break;
        }
        if (viewer.grid_dirty) {
            viewer.grid_dirty = false;
            status = rebuild_grid(&viewer);
            if (status != ViewerStatus::Ok) {
                loop_error = status;
                break;
            }
        }
        if (!render_frame(&viewer)) {
            break;
        }
    }

    return loop_error;
}
