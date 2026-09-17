// Implements the reusable D3D11/D2D1 rendering surface used by modal windows.
// The swap chain is flip-sequential and vertically synchronized. Device loss,
// resize, and DPI-dependent DirectWrite resources are handled in one place so
// individual dialogs only describe their visual tree and interaction state.

#include "DialogSurface.h"

#include <d2d1helper.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using Microsoft::WRL::ComPtr;

DialogPlacement ResolveDialogPlacement(HWND owner) noexcept {
    DialogPlacement placement{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &placement.workArea, 0)) {
        placement.workArea = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    placement.anchor = placement.workArea;
    if (!owner || !IsWindow(owner)) return placement;

    RECT ownerBounds{};
    if (GetWindowRect(owner, &ownerBounds)) placement.anchor = ownerBounds;
    const HMONITOR monitorHandle = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    if (monitorHandle) {
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(monitorHandle, &monitor)) placement.workArea = monitor.rcWork;
    }
    return placement;
}

D2D1_COLOR_F D2DDialogSurface::Color(std::uint32_t rgb) {
    return D2D1::ColorF(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f);
}

bool D2DDialogSurface::Initialize(HWND window) {
    window_ = window;
    scale_ = GetDpiForWindow(window) / 96.0f;
    RECT client{}; GetClientRect(window, &client);
    width_ = std::max<LONG>(1, client.right); height_ = std::max<LONG>(1, client.bottom);
    return CreateIndependentResources() && CreateDeviceResources();
}

bool D2DDialogSurface::CreateIndependentResources() {
    D2D1_FACTORY_OPTIONS options{};
#ifdef _DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options, d2dFactory_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &writeFactory_))) return false;
    return CreateFormats();
}

bool D2DDialogSurface::CreateFormats() {
    format_.Reset(); centerFormat_.Reset(); boldFormat_.Reset(); centerBoldFormat_.Reset();
    titleFormat_.Reset(); centerTitleFormat_.Reset(); captionFormat_.Reset();
    // All dialog controls share one typographic scale. DirectWrite text-format
    // properties are stateful, so keep all four weight/alignment combinations
    // instead of mutating a shared format while a frame is being recorded.
    const float size = 14.0f * scale_;
    if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ja-JP", &format_))) return false;
    if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ja-JP", &centerFormat_))) return false;
    if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ja-JP", &boldFormat_))) return false;
    if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ja-JP", &centerBoldFormat_))) return false;
    if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24.0f * scale_, L"ja-JP", &titleFormat_))) return false;
    if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24.0f * scale_, L"ja-JP", &centerTitleFormat_))) return false;
    if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * scale_, L"ja-JP", &captionFormat_))) return false;
    for (auto* format : {format_.Get(), centerFormat_.Get(), boldFormat_.Get(), centerBoldFormat_.Get()}) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    centerFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    centerBoldFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    for (auto* format : {titleFormat_.Get(), centerTitleFormat_.Get(), captionFormat_.Get()}) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    centerTitleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    return true;
}

bool D2DDialogSurface::CreateDeviceResources() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL created{};
    const std::array levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    // Match the main renderer's hardware/debug/WARP fallback policy so modal UI
    // remains available on remote desktops and software-rendered systems.
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels.data(),
        static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &d3dDevice_, &created, &d3dContext_);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG,
            levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &d3dDevice_, &created, &d3dContext_);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG,
            levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &d3dDevice_, &created, &d3dContext_);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDevice; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIFactory2> factory;
    if (FAILED(d3dDevice_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    // Dialogs redraw their complete small client area on demand; flip sequential
    // still minimizes composition overhead and supports modern presentation.
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_; desc.Height = height_; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.Scaling = DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(d3dDevice_.Get(), window_, &desc, nullptr, nullptr, &swapChain_))) return false;
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_)) ||
        FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_))) return false;
    return CreateTarget();
}

bool D2DDialogSurface::CreateTarget() {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (FAILED(context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &target_))) return false;
    context_->SetTarget(target_.Get());
    return SUCCEEDED(context_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_));
}

void D2DDialogSurface::DiscardDeviceResources() {
    brush_.Reset(); target_.Reset(); context_.Reset(); d2dDevice_.Reset(); swapChain_.Reset(); d3dContext_.Reset(); d3dDevice_.Reset();
}

void D2DDialogSurface::Resize(UINT width, UINT height) {
    width_ = std::max(1u, width); height_ = std::max(1u, height);
    if (!swapChain_) return;
    context_->SetTarget(nullptr); brush_.Reset(); target_.Reset();
    if (SUCCEEDED(swapChain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0))) CreateTarget();
}

void D2DDialogSurface::DpiChanged(UINT dpi) {
    scale_ = dpi / 96.0f;
    CreateFormats();
}

bool D2DDialogSurface::Begin(const Palette& palette) {
    if (!context_) return false;
    context_->BeginDraw();
    context_->Clear(Color(palette.background));
    return true;
}

bool D2DDialogSurface::End() {
    if (!context_) return false;
    const HRESULT draw = context_->EndDraw();
    // Returning false after recovery tells the dialog to queue another frame;
    // newly created swap-chain buffers do not contain the interrupted drawing.
    if (draw == D2DERR_RECREATE_TARGET) { DiscardDeviceResources(); CreateDeviceResources(); return false; }
    if (FAILED(draw)) return false;
    DXGI_PRESENT_PARAMETERS parameters{};
    const HRESULT present = swapChain_->Present1(1, 0, &parameters);
    if (present == DXGI_ERROR_DEVICE_REMOVED || present == DXGI_ERROR_DEVICE_RESET) {
        DiscardDeviceResources(); CreateDeviceResources(); return false;
    }
    return SUCCEEDED(present);
}

void D2DDialogSurface::Fill(D2D1_RECT_F rect, std::uint32_t color) { brush_->SetColor(Color(color)); context_->FillRectangle(rect, brush_.Get()); }
void D2DDialogSurface::Stroke(D2D1_RECT_F rect, std::uint32_t color, float width) { brush_->SetColor(Color(color)); context_->DrawRectangle(rect, brush_.Get(), width); }
void D2DDialogSurface::FillRounded(D2D1_RECT_F rect, float radius, std::uint32_t color) {
    brush_->SetColor(Color(color));
    context_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
}
void D2DDialogSurface::StrokeRounded(D2D1_RECT_F rect, float radius, std::uint32_t color, float width) {
    brush_->SetColor(Color(color));
    context_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get(), width);
}
void D2DDialogSurface::FillLinearGradient(D2D1_RECT_F rect, std::span<const DialogGradientStop> stops, bool vertical) {
    if (!context_ || stops.empty()) return;
    std::vector<D2D1_GRADIENT_STOP> nativeStops;
    nativeStops.reserve(stops.size());
    for (const auto& stop : stops) {
        D2D1_COLOR_F color = Color(stop.color);
        color.a = std::clamp(stop.opacity, 0.0f, 1.0f);
        nativeStops.push_back({std::clamp(stop.position, 0.0f, 1.0f), color});
    }
    ComPtr<ID2D1GradientStopCollection> collection;
    if (FAILED(context_->CreateGradientStopCollection(nativeStops.data(), static_cast<UINT32>(nativeStops.size()),
        D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &collection))) return;
    const D2D1_POINT_2F start = vertical ? D2D1::Point2F(rect.left, rect.top) : D2D1::Point2F(rect.left, rect.top);
    const D2D1_POINT_2F end = vertical ? D2D1::Point2F(rect.left, rect.bottom) : D2D1::Point2F(rect.right, rect.top);
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(context_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(start, end),
        collection.Get(), &gradient))) return;
    context_->FillRectangle(rect, gradient.Get());
}
void D2DDialogSurface::Line(D2D1_POINT_2F from, D2D1_POINT_2F to, std::uint32_t color, float width) { brush_->SetColor(Color(color)); context_->DrawLine(from, to, brush_.Get(), width); }
void D2DDialogSurface::Ellipse(D2D1_ELLIPSE ellipse, std::uint32_t color, float width, bool fill) {
    brush_->SetColor(Color(color));
    if (fill) context_->FillEllipse(ellipse, brush_.Get()); else context_->DrawEllipse(ellipse, brush_.Get(), width);
}

bool D2DDialogSurface::DrawIcon(HICON icon, D2D1_RECT_F rect) {
    if (!context_ || !icon) return false;

    // CreateBitmapFromHICON preserves the alpha channel embedded in modern ICO
    // resources. Conversion to premultiplied BGRA matches the swap-chain format
    // and avoids dark fringes around partially transparent icon pixels.
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> source;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateBitmapFromHICON(icon, &source)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(context_->CreateBitmapFromWicBitmap(converter.Get(), &bitmap))) {
        return false;
    }
    // The BinEdit mark is pixel art, so nearest-neighbor sampling retains its
    // deliberate hard edges at fractional Per-Monitor-V2 scaling factors.
    context_->DrawBitmap(bitmap.Get(), rect, 1.0f,
        D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    return true;
}

void D2DDialogSurface::Text(std::wstring_view text, D2D1_RECT_F rect, std::uint32_t color, bool centered, bool bold) {
    brush_->SetColor(Color(color));
    // Select by both independent attributes. The earlier bold-first selection
    // discarded `centered`, which placed primary-button captions at the left.
    IDWriteTextFormat* format = centered
        ? (bold ? centerBoldFormat_.Get() : centerFormat_.Get())
        : (bold ? boldFormat_.Get() : format_.Get());
    context_->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, rect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void D2DDialogSurface::TitleText(std::wstring_view text, D2D1_RECT_F rect, std::uint32_t color, bool centered) {
    brush_->SetColor(Color(color));
    context_->DrawTextW(text.data(), static_cast<UINT32>(text.size()), centered ? centerTitleFormat_.Get() : titleFormat_.Get(),
        rect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void D2DDialogSurface::CaptionText(std::wstring_view text, D2D1_RECT_F rect, std::uint32_t color) {
    brush_->SetColor(Color(color));
    context_->DrawTextW(text.data(), static_cast<UINT32>(text.size()), captionFormat_.Get(), rect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float D2DDialogSurface::TextPosition(std::wstring_view text, std::size_t position, float maxWidth) {
    // DirectWrite understands UTF-16 shaping and surrogate pairs better than
    // estimating advance from character count, so caret geometry uses hit testing.
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(writeFactory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format_.Get(),
        std::max(1.0f, maxWidth), 100.0f * scale_, &layout))) return 0.0f;
    FLOAT x{}, y{}; DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestTextPosition(static_cast<UINT32>(std::min(position, text.size())), FALSE, &x, &y, &metrics))) return 0.0f;
    return x;
}

void D2DDialogSurface::PushClip(D2D1_RECT_F rect) { context_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED); }
void D2DDialogSurface::PopClip() { context_->PopAxisAlignedClip(); }
