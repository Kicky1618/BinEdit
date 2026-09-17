// Implements the main hex-editor renderer and R2PR damage pipeline.
// D3D11 provides the DXGI swap chain, D2D1 draws the retained back buffers, and
// DirectWrite renders text. Present1 receives the same dirty rectangle used as
// the D2D clip, minimizing both raster work and desktop-compositor updates.

#include "Renderer.h"

#include <d2d1helper.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace {
// IntersectRect writes an all-zero RECT for disjoint inputs; wrapping it keeps
// damage clipping readable at call sites.
RECT IntersectWith(RECT a, RECT b) {
    RECT out{};
    if (!IntersectRect(&out, &a, &b)) return {};
    return out;
}

bool EmptyRect(const RECT& rect) { return rect.right <= rect.left || rect.bottom <= rect.top; }

constexpr std::size_t CompleteVisibleRows(float bodyHeight, float rowHeight) noexcept {
    // The editor may have a fractional row of vertical space above the status
    // band. Only complete rows are interactive and drawable; rounding upward
    // would expose an isolated offset label with a clipped byte row.
    if (bodyHeight < rowHeight || rowHeight <= 0.0f) return 1;
    return static_cast<std::size_t>(bodyHeight / rowHeight);
}

// Representative 100% and 125% default viewports must not regress to the old
// ceiling-based results of 25 and 19 rows respectively.
static_assert(CompleteVisibleRows(592.0f, 24.0f) == 24);
static_assert(CompleteVisibleRows(560.0f, 30.0f) == 18);

std::wstring OffsetText(std::size_t value) {
    // Eight digits preserve the familiar compact layout while the 64-bit cast
    // allows offsets beyond 4 GiB to expand naturally when necessary.
    wchar_t text[32]{};
    swprintf_s(text, L"%08llX", static_cast<unsigned long long>(value));
    return text;
}
}

void R2PRDamage::Require(RECT rect) {
    if (full_ || EmptyRect(rect)) return;
    // A single bounding rectangle is cheaper than replaying the complete scene
    // for many tiny clips and maps directly to one Present1 dirty rectangle.
    if (!dirty_) dirty_ = rect;
    else {
        RECT joined{};
        UnionRect(&joined, &*dirty_, &rect);
        dirty_ = joined;
    }
}

void R2PRDamage::RequireFull() { full_ = true; dirty_.reset(); }

std::optional<RECT> R2PRDamage::Consume(RECT bounds) {
    // Consuming transfers ownership of pending damage to exactly one frame.
    if (full_) { full_ = false; return bounds; }
    if (!dirty_) return std::nullopt;
    RECT result = IntersectWith(*dirty_, bounds);
    dirty_.reset();
    if (EmptyRect(result)) return std::nullopt;
    return result;
}

D2D1_COLOR_F Renderer::Color(std::uint32_t rgb, float alpha) {
    return D2D1::ColorF(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha);
}

bool Renderer::Initialize(HWND window) {
    window_ = window;
    // D2D is configured at 96 DPI, therefore logical UI sizes are explicitly
    // scaled into physical pixels using the HWND's current monitor DPI.
    dpiScale_ = GetDpiForWindow(window_) / 96.0f;
    RECT client{}; GetClientRect(window_, &client);
    pixelWidth_ = std::max<LONG>(1, client.right);
    pixelHeight_ = std::max<LONG>(1, client.bottom);
    if (!CreateDeviceIndependentResources() || !CreateDeviceResources()) return false;
    Require(R2PRRegion::Full);
    return true;
}

bool Renderer::CreateDeviceIndependentResources() {
    D2D1_FACTORY_OPTIONS options{};
#ifdef _DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options, d2dFactory_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &writeFactory_))) return false;
    return CreateTextFormats();
}

bool Renderer::CreateTextFormats() {
    monoFormat_.Reset(); uiFormat_.Reset(); uiStrongFormat_.Reset();
    tabEllipsis_.Reset(); tooltipFormat_.Reset();
    const float monoSize = 14.0f * dpiScale_;
    const float uiSize = 12.0f * dpiScale_;
    // Cascadia Mono keeps byte columns stable. DirectWrite performs font fallback
    // automatically when the face is not installed or a glyph is unavailable.
    if (FAILED(writeFactory_->CreateTextFormat(L"Cascadia Mono", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, monoSize, L"ja-JP", &monoFormat_))) return false;
    if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, uiSize, L"ja-JP", &uiFormat_))) return false;
    if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, uiSize, L"ja-JP", &uiStrongFormat_))) return false;
    if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, uiSize, L"ja-JP", &tooltipFormat_))) return false;
    if (FAILED(writeFactory_->CreateEllipsisTrimmingSign(uiStrongFormat_.Get(), &tabEllipsis_))) return false;
    monoFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    monoFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    monoFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    uiFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    uiFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    uiStrongFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    uiStrongFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tooltipFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
    tooltipFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    tooltipFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    return true;
}

bool Renderer::CreateDeviceResources() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL created{};
    const std::array levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    // Prefer hardware, retry without the optional debug layer, then fall back to
    // WARP. This keeps diagnostics useful without making deployment depend on the
    // Graphics Tools optional Windows feature.
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels.data(),
                                   static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &d3dDevice_, &created, &d3dContext_);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG,
                               levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
                               &d3dDevice_, &created, &d3dContext_);
    }
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG,
                               levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
                               &d3dDevice_, &created, &d3dContext_);
    }
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(d3dDevice_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    // FLIP_SEQUENTIAL preserves buffer contents, which is required for partial
    // redraws and dirty-rectangle presentation.
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = pixelWidth_; desc.Height = pixelHeight_; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.Scaling = DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(d3dDevice_.Get(), window_, &desc, nullptr, nullptr, &swapChain_))) return false;
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_)) ||
        FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_))) return false;
    return CreateTargetBitmap();
}

bool Renderer::CreateTargetBitmap() {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    // BGRA plus alpha-ignore matches both D2D and the opaque HWND swap chain.
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &targetBitmap_))) return false;
    d2dContext_->SetTarget(targetBitmap_.Get());
    if (FAILED(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_))) return false;
    UpdateMetrics();
    // Both buffers in the flip-sequential chain must receive a complete frame
    // before partial R2PR updates can safely rely on preserved pixels.
    fullRedrawsRemaining_ = 2;
    damage_.RequireFull();
    return true;
}

void Renderer::DiscardDeviceResources() {
    brush_.Reset(); targetBitmap_.Reset(); d2dContext_.Reset(); d2dDevice_.Reset();
    swapChain_.Reset(); d3dContext_.Reset(); d3dDevice_.Reset();
}

void Renderer::Resize(UINT width, UINT height) {
    width = std::max(1u, width); height = std::max(1u, height);
    pixelWidth_ = width; pixelHeight_ = height;
    if (!swapChain_) return;
    // All references to the old DXGI back buffer must be released before
    // ResizeBuffers; retaining the D2D target would make the call fail.
    d2dContext_->SetTarget(nullptr); brush_.Reset(); targetBitmap_.Reset();
    if (SUCCEEDED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) CreateTargetBitmap();
    Require(R2PRRegion::Full);
}

void Renderer::DpiChanged(UINT dpi) {
    const float scale = dpi / 96.0f;
    if (std::abs(scale - dpiScale_) < 0.001f) return;
    dpiScale_ = scale;
    CreateTextFormats();
    UpdateMetrics();
    Require(R2PRRegion::Full);
}

void Renderer::UpdateMetrics() {
    // Measurements intentionally use rounded physical pixels. Aligned grid lines
    // and cell backgrounds remain crisp at fractional Windows scaling factors.
    metrics_.width = static_cast<float>(pixelWidth_); metrics_.height = static_cast<float>(pixelHeight_);
    metrics_.tabStripHeight = std::round(44.0f * dpiScale_);
    metrics_.headerHeight = metrics_.tabStripHeight + std::round(48.0f * dpiScale_);
    metrics_.statusHeight = std::round(36.0f * dpiScale_);
    metrics_.rowHeight = std::round(24.0f * dpiScale_);
    const float inset = std::round(8.0f * dpiScale_);
    const float scrollBarGutter = std::round(20.0f * dpiScale_);
    metrics_.scrollBarRight = std::max(0.0f, metrics_.width - inset);
    metrics_.scrollBarLeft = std::max(0.0f, metrics_.scrollBarRight - scrollBarGutter);
    metrics_.scrollBarTrackTop = metrics_.headerHeight + std::round(4.0f * dpiScale_);
    metrics_.scrollBarTrackBottom = std::max(metrics_.scrollBarTrackTop,
        metrics_.height - metrics_.statusHeight - std::round(4.0f * dpiScale_));
    const float effectiveWidth = metrics_.width / std::max(0.01f, dpiScale_);
    const bool compact = effectiveWidth < 900.0f;
    metrics_.addressX = std::round((compact ? 20.0f : 24.0f) * dpiScale_);
    metrics_.hexX = std::round((compact ? 104.0f : 124.0f) * dpiScale_);
    metrics_.hexCellWidth = std::round((compact ? 25.0f : 28.0f) * dpiScale_);
    metrics_.textX = metrics_.hexX + metrics_.hexCellWidth * LayoutMetrics::bytesPerRow +
        std::round((compact ? 16.0f : 22.0f) * dpiScale_);
    metrics_.textCellWidth = std::round((compact ? 14.0f : 15.0f) * dpiScale_);
    const float textRight = metrics_.textX + metrics_.textCellWidth * LayoutMetrics::bytesPerRow + std::round(16.0f * dpiScale_);
    metrics_.textPaneVisible = textRight <= metrics_.scrollBarLeft;
    const float body = std::max(0.0f, metrics_.height - metrics_.headerHeight - metrics_.statusHeight);
    metrics_.visibleRows = CompleteVisibleRows(body, metrics_.rowHeight);
}

void Renderer::RequireRect(RECT rect) { damage_.Require(rect); }

void Renderer::Require(R2PRRegion region, std::optional<std::size_t> byteOffset, std::size_t firstRow) {
    if (region == R2PRRegion::Full) { damage_.RequireFull(); return; }
    RECT rect{0, 0, static_cast<LONG>(pixelWidth_), static_cast<LONG>(pixelHeight_)};
    if (region == R2PRRegion::Tabs) rect.bottom = static_cast<LONG>(std::ceil(metrics_.tabStripHeight));
    else if (region == R2PRRegion::Header) rect.bottom = static_cast<LONG>(std::ceil(metrics_.headerHeight));
    else if (region == R2PRRegion::Status) rect.top = static_cast<LONG>(std::floor(metrics_.height - metrics_.statusHeight));
    else if (region == R2PRRegion::ScrollBar) {
        rect.left = static_cast<LONG>(std::floor(metrics_.scrollBarLeft));
        rect.top = static_cast<LONG>(std::floor(metrics_.headerHeight));
        rect.bottom = static_cast<LONG>(std::ceil(metrics_.height - metrics_.statusHeight));
    }
    else if (region == R2PRRegion::Data) {
        rect.top = static_cast<LONG>(std::floor(metrics_.headerHeight));
        rect.bottom = static_cast<LONG>(std::ceil(metrics_.height - metrics_.statusHeight));
        rect.right = static_cast<LONG>(std::ceil(metrics_.scrollBarLeft));
    } else if (region == R2PRRegion::IoProgress) {
        // Animation frames touch only the centered ring/text area. The initial
        // transition requests Full once to establish the opaque disabled layer.
        const float halfWidth = std::min(metrics_.width * 0.5f,
            std::round(320.0f * dpiScale_));
        const float halfHeight = std::round(72.0f * dpiScale_);
        rect.left = static_cast<LONG>(std::floor(metrics_.width * 0.5f - halfWidth));
        rect.right = static_cast<LONG>(std::ceil(metrics_.width * 0.5f + halfWidth));
        rect.top = static_cast<LONG>(std::floor(metrics_.height * 0.5f - halfHeight));
        rect.bottom = static_cast<LONG>(std::ceil(metrics_.height * 0.5f + halfHeight));
    } else if (region == R2PRRegion::ByteCell && byteOffset) {
        // Both the hexadecimal and character cells share one row-level damage
        // rectangle because editing one byte changes both representations.
        const std::size_t row = *byteOffset / LayoutMetrics::bytesPerRow;
        if (row < firstRow || row >= firstRow + metrics_.visibleRows) return;
        rect.top = static_cast<LONG>(metrics_.headerHeight + (row - firstRow) * metrics_.rowHeight);
        rect.bottom = static_cast<LONG>(rect.top + metrics_.rowHeight + 1);
        rect.left = static_cast<LONG>(metrics_.hexX);
        rect.right = static_cast<LONG>(std::ceil(metrics_.scrollBarLeft));
    }
    damage_.Require(rect);
}

float Renderer::TabWidth(std::size_t tabCount) const {
    if (tabCount == 0) return 0;
    const float margin = std::round(8.0f * dpiScale_);
    const float available = std::max(1.0f, metrics_.width - margin * 2);
    // Tabs contract as the group grows, but retain enough width for a close hit
    // target. Text is clipped by DirectWrite inside the remaining title box.
    return std::max(std::round(56.0f * dpiScale_),
        std::min(std::round(220.0f * dpiScale_), available / static_cast<float>(tabCount)));
}

D2D1_RECT_F Renderer::TabBounds(std::size_t index, std::size_t tabCount,
                                std::size_t firstVisibleTab) const {
    const float margin = std::round(8.0f * dpiScale_);
    const float gap = std::max(1.0f, std::round(2.0f * dpiScale_));
    const float width = TabWidth(tabCount);
    const auto relative = static_cast<std::ptrdiff_t>(index) -
        static_cast<std::ptrdiff_t>(firstVisibleTab);
    const float left = margin + static_cast<float>(relative) * width;
    return D2D1::RectF(left, std::round(5.0f * dpiScale_),
        std::min(metrics_.width - margin, left + width - gap), metrics_.tabStripHeight);
}

std::size_t Renderer::MaximumFirstVisibleTab(std::size_t tabCount) const noexcept {
    if (tabCount == 0) return 0;
    const float margin = std::round(8.0f * dpiScale_);
    const float available = std::max(1.0f, metrics_.width - margin * 2.0f);
    const float width = std::max(1.0f, TabWidth(tabCount));
    const auto capacity = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::floor(available / width)));
    return tabCount > capacity ? tabCount - capacity : 0;
}

bool Renderer::CreateTabTitleLayout(const std::wstring& title, float width, float height,
                                    ComPtr<IDWriteTextLayout>& layout) const {
    layout.Reset();
    if (!writeFactory_ || !uiStrongFormat_ || !tabEllipsis_ || width <= 0.0f || height <= 0.0f) return false;
    if (FAILED(writeFactory_->CreateTextLayout(title.c_str(), static_cast<UINT32>(title.size()),
        uiStrongFormat_.Get(), width, height, &layout))) return false;
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    return SUCCEEDED(layout->SetTrimming(&trimming, tabEllipsis_.Get()));
}

bool Renderer::IsTextLayoutTrimmed(IDWriteTextLayout* layout) noexcept {
    if (!layout) return false;
    DWRITE_LINE_METRICS line{};
    UINT32 actual{};
    return SUCCEEDED(layout->GetLineMetrics(&line, 1, &actual)) && actual == 1 && line.isTrimmed;
}

ScrollBarGeometry Renderer::GetScrollBarGeometry(std::size_t firstRow,
                                                  std::size_t totalRows) const noexcept {
    ScrollBarGeometry geometry;
    geometry.left = metrics_.scrollBarLeft;
    geometry.right = metrics_.scrollBarRight;
    geometry.top = metrics_.scrollBarTrackTop;
    geometry.bottom = metrics_.scrollBarTrackBottom;
    const float trackHeight = std::max(0.0f, geometry.bottom - geometry.top);
    const std::size_t visibleRows = std::max<std::size_t>(1, metrics_.visibleRows);
    geometry.scrollable = trackHeight > 0.0f && totalRows > visibleRows;
    if (!geometry.scrollable) {
        geometry.thumbTop = geometry.top;
        geometry.thumbBottom = geometry.bottom;
        return geometry;
    }

    const float minimumThumb = std::min(trackHeight, std::round(28.0f * dpiScale_));
    const long double visibleRatio = static_cast<long double>(visibleRows) /
                                     static_cast<long double>(totalRows);
    const float thumbHeight = std::clamp(
        static_cast<float>(trackHeight * visibleRatio), minimumThumb, trackHeight);
    const float travel = std::max(0.0f, trackHeight - thumbHeight);
    const std::size_t maximumFirst = totalRows - visibleRows;
    firstRow = std::min(firstRow, maximumFirst);
    const long double positionRatio = static_cast<long double>(firstRow) /
                                      static_cast<long double>(maximumFirst);
    geometry.thumbTop = geometry.top + static_cast<float>(travel * positionRatio);
    geometry.thumbBottom = geometry.thumbTop + thumbHeight;
    return geometry;
}

ScrollBarPart Renderer::HitTestScrollBar(float x, float y, std::size_t firstRow,
                                         std::size_t totalRows) const noexcept {
    const ScrollBarGeometry geometry = GetScrollBarGeometry(firstRow, totalRows);
    if (!geometry.scrollable || x < geometry.left || x >= geometry.right ||
        y < geometry.top || y >= geometry.bottom) return ScrollBarPart::None;
    if (y >= geometry.thumbTop && y < geometry.thumbBottom) return ScrollBarPart::Thumb;
    return y < geometry.thumbTop ? ScrollBarPart::TrackBefore : ScrollBarPart::TrackAfter;
}

std::size_t Renderer::ScrollRowFromThumb(float pointerY, float grabOffset,
                                         std::size_t totalRows) const noexcept {
    const ScrollBarGeometry geometry = GetScrollBarGeometry(0, totalRows);
    const std::size_t visibleRows = std::max<std::size_t>(1, metrics_.visibleRows);
    if (!geometry.scrollable || totalRows <= visibleRows) return 0;
    const float thumbHeight = geometry.thumbBottom - geometry.thumbTop;
    const float travel = std::max(0.0f, geometry.bottom - geometry.top - thumbHeight);
    if (travel <= 0.0f) return 0;
    const float thumbTop = std::clamp(pointerY - grabOffset, geometry.top, geometry.top + travel);
    const long double ratio = static_cast<long double>(thumbTop - geometry.top) /
                              static_cast<long double>(travel);
    const std::size_t maximumFirst = totalRows - visibleRows;
    return std::min(maximumFirst, static_cast<std::size_t>(
        ratio * static_cast<long double>(maximumFirst) + 0.5L));
}

bool Renderer::IsInDataViewport(float x, float y) const noexcept {
    const float completeRowsBottom = metrics_.headerHeight +
        metrics_.visibleRows * metrics_.rowHeight;
    return x >= 0.0f && x < metrics_.scrollBarLeft &&
        y >= metrics_.headerHeight && y < completeRowsBottom;
}

RECT Renderer::AutoScrollMarkerBounds(float x, float y) const noexcept {
    const float radius = std::round(18.0f * dpiScale_);
    const float antialiasMargin = std::max(2.0f, std::round(2.0f * dpiScale_));
    return {
        static_cast<LONG>(std::floor(x - radius - antialiasMargin)),
        static_cast<LONG>(std::floor(y - radius - antialiasMargin)),
        static_cast<LONG>(std::ceil(x + radius + antialiasMargin)),
        static_cast<LONG>(std::ceil(y + radius + antialiasMargin))
    };
}

bool Renderer::Render(const RenderModel& model) {
    if (!d2dContext_ || !swapChain_) return false;
    RECT bounds{0, 0, static_cast<LONG>(pixelWidth_), static_cast<LONG>(pixelHeight_)};
    auto damage = damage_.Consume(bounds);
    if (!damage) return true;
    // After swap-chain creation each rotating buffer starts undefined. Initialize
    // both completely before honoring partial damage.
    if (fullRedrawsRemaining_ > 0) damage = bounds;
    const Palette palette = model.profile->ResolvePalette();
    d2dContext_->BeginDraw();
    d2dContext_->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(damage->left), static_cast<float>(damage->top),
        static_cast<float>(damage->right), static_cast<float>(damage->bottom)), D2D1_ANTIALIAS_MODE_ALIASED);
    DrawScene(model, palette, *damage);
    d2dContext_->PopAxisAlignedClip();
    const HRESULT drawResult = d2dContext_->EndDraw();
    if (drawResult == D2DERR_RECREATE_TARGET) { DiscardDeviceResources(); return CreateDeviceResources(); }
    if (FAILED(drawResult)) return false;

    // Propagate the exact D2D clip to DWM through Present1. Sync interval one
    // naturally supports displays up to and including 240 Hz without a busy loop.
    DXGI_PRESENT_PARAMETERS present{};
    present.DirtyRectsCount = 1;
    present.pDirtyRects = &*damage;
    const HRESULT result = swapChain_->Present1(1, 0, &present);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        DiscardDeviceResources(); return CreateDeviceResources();
    }
    if (SUCCEEDED(result) && fullRedrawsRemaining_ > 0) {
        --fullRedrawsRemaining_;
        if (fullRedrawsRemaining_ > 0) damage_.RequireFull();
    }
    return SUCCEEDED(result);
}

void Renderer::DrawScene(const RenderModel& model, const Palette& palette, const RECT& damage) {
    const auto& strings = GetStrings(model.language);
    const auto damageF = D2D1::RectF(static_cast<float>(damage.left), static_cast<float>(damage.top),
        static_cast<float>(damage.right), static_cast<float>(damage.bottom));
    brush_->SetColor(Color(palette.background)); d2dContext_->FillRectangle(damageF, brush_.Get());

    // The UI thread deliberately does not inspect a ByteDocument while its file
    // operation owns that object on a worker thread. An opaque disabled surface
    // therefore replaces the scene instead of attempting to render borrowed byte
    // spans concurrently with Load/Save state transitions.
    if (model.ioBusy) {
        DrawIoProgress(model, palette, damage);
        return;
    }
    const std::size_t dataSize = model.document ? model.document->Size() : 0;

    if (damage.top < metrics_.tabStripHeight) {
        const float margin = std::round(8.0f * dpiScale_);
        const float tabWidth = TabWidth(model.tabs.size());
        for (std::size_t index = 0; index < model.tabs.size(); ++index) {
            const auto relative = static_cast<std::ptrdiff_t>(index) -
                static_cast<std::ptrdiff_t>(model.firstVisibleTab);
            const float left = margin + static_cast<float>(relative) * tabWidth;
            if (left + tabWidth <= margin) continue;
            if (left >= metrics_.width - margin) break;
            const auto tabRect = TabBounds(index, model.tabs.size(), model.firstVisibleTab);
            const bool active = index == model.activeTab;
            brush_->SetColor(Color(active ? palette.surface : palette.background));
            d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(tabRect,
                std::round(7.0f * dpiScale_), std::round(7.0f * dpiScale_)), brush_.Get());
            brush_->SetColor(Color(active ? palette.caret : palette.grid));
            d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(tabRect,
                std::round(7.0f * dpiScale_), std::round(7.0f * dpiScale_)), brush_.Get(),
                active ? std::max(1.0f, 2.0f * dpiScale_) : std::max(1.0f, dpiScale_));

            const float closeWidth = std::round(28.0f * dpiScale_);
            const float dirtyWidth = model.tabs[index].dirty ? std::round(14.0f * dpiScale_) : 0.0f;
            const auto titleRect = D2D1::RectF(tabRect.left + std::round(10.0f * dpiScale_), tabRect.top,
                std::max(tabRect.left, tabRect.right - closeWidth - dirtyWidth), tabRect.bottom);
            brush_->SetColor(Color(active ? palette.text : palette.mutedText));
            ComPtr<IDWriteTextLayout> titleLayout;
            if (CreateTabTitleLayout(model.tabs[index].title, titleRect.right - titleRect.left,
                                     titleRect.bottom - titleRect.top, titleLayout)) {
                d2dContext_->DrawTextLayout({titleRect.left, titleRect.top}, titleLayout.Get(),
                    brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }

            // Keep the unsaved-state indicator outside the trimmed title so a
            // very long file name can never hide this important state cue.
            if (model.tabs[index].dirty) {
                const float dirtyCenterX = tabRect.right - closeWidth - dirtyWidth * 0.5f;
                const float dirtyCenterY = (tabRect.top + tabRect.bottom) * 0.5f;
                const float dirtyRadius = std::max(2.0f, std::round(2.0f * dpiScale_));
                d2dContext_->FillEllipse(D2D1::Ellipse({dirtyCenterX, dirtyCenterY},
                    dirtyRadius, dirtyRadius), brush_.Get());
            }

            const float centerX = tabRect.right - closeWidth * 0.5f;
            const float centerY = (tabRect.top + tabRect.bottom) * 0.5f;
            const float arm = std::round(4.0f * dpiScale_);
            brush_->SetColor(Color(active ? palette.text : palette.mutedText));
            d2dContext_->DrawLine({centerX - arm, centerY - arm}, {centerX + arm, centerY + arm},
                brush_.Get(), std::max(1.0f, 1.4f * dpiScale_));
            d2dContext_->DrawLine({centerX + arm, centerY - arm}, {centerX - arm, centerY + arm},
                brush_.Get(), std::max(1.0f, 1.4f * dpiScale_));
        }
    }

    // One rounded content layer mirrors a WinUI page/card hierarchy without
    // surrounding every editor subsection with another decorative container.
    // Drawing the complete card is inexpensive because the outer R2PR clip limits
    // actual rasterization to the current damage rectangle.
    const float inset = std::round(8.0f * dpiScale_);
    const float radius = std::round(8.0f * dpiScale_);
    const float statusTop = metrics_.height - metrics_.statusHeight;
    const auto editorRect = D2D1::RectF(inset, metrics_.tabStripHeight,
        std::max(inset, metrics_.width - inset), std::max(metrics_.tabStripHeight, statusTop));
    brush_->SetColor(Color(palette.surface));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(editorRect, radius, radius), brush_.Get());
    brush_->SetColor(Color(palette.grid));
    d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(editorRect, radius, radius), brush_.Get(), std::max(1.0f, dpiScale_));

    // Each band tests damage independently, avoiding row formatting when only the
    // status line or header changed.
    if (damage.top < metrics_.headerHeight) {
        brush_->SetColor(Color(palette.grid));
        d2dContext_->DrawLine(D2D1::Point2F(inset, metrics_.headerHeight - 1),
            D2D1::Point2F(std::max(inset, metrics_.width - inset), metrics_.headerHeight - 1), brush_.Get());
        brush_->SetColor(Color(palette.mutedText));
        const std::wstring address = strings.headerOffset;
        d2dContext_->DrawTextW(address.c_str(), static_cast<UINT32>(address.size()), uiStrongFormat_.Get(),
            D2D1::RectF(metrics_.addressX, metrics_.tabStripHeight, metrics_.hexX - 8, metrics_.headerHeight), brush_.Get());
        // The active input pane gets both stronger header text and an accent
        // underline. This remains visible even when the caret sits on a colored
        // byte whose own focus outline has low local contrast.
        brush_->SetColor(Color(model.hexPane ? palette.text : palette.mutedText));
        for (std::size_t i = 0; i < LayoutMetrics::bytesPerRow; ++i) {
            wchar_t text[3]{}; swprintf_s(text, L"%02X", static_cast<unsigned>(i));
            d2dContext_->DrawTextW(text, 2, monoFormat_.Get(),
                D2D1::RectF(metrics_.hexX + i * metrics_.hexCellWidth, metrics_.tabStripHeight,
                    metrics_.hexX + (i + 1) * metrics_.hexCellWidth, metrics_.headerHeight), brush_.Get());
        }
        const float cueY = metrics_.headerHeight - std::max(2.0f, 3.0f * dpiScale_);
        if (model.hexPane) {
            brush_->SetColor(Color(palette.caret));
            d2dContext_->DrawLine({metrics_.hexX, cueY},
                {metrics_.hexX + metrics_.hexCellWidth * LayoutMetrics::bytesPerRow - 2, cueY},
                brush_.Get(), std::max(2.0f, 2.0f * dpiScale_));
        }
        if (metrics_.textPaneVisible) {
            brush_->SetColor(Color(model.hexPane ? palette.mutedText : palette.text));
            const std::wstring chars = strings.headerCharacters;
            d2dContext_->DrawTextW(chars.c_str(), static_cast<UINT32>(chars.size()), uiStrongFormat_.Get(),
                D2D1::RectF(metrics_.textX, metrics_.tabStripHeight, metrics_.scrollBarLeft, metrics_.headerHeight), brush_.Get());
            if (!model.hexPane) {
                brush_->SetColor(Color(palette.caret));
                d2dContext_->DrawLine({metrics_.textX, cueY},
                    {std::min(metrics_.scrollBarLeft, metrics_.textX + metrics_.textCellWidth * LayoutMetrics::bytesPerRow), cueY},
                    brush_.Get(), std::max(2.0f, 2.0f * dpiScale_));
            }
        }
    }

    const float bodyBottom = statusTop;
    if (damage.right > 0 && damage.left < metrics_.scrollBarLeft &&
        damage.bottom > metrics_.headerHeight && damage.top < bodyBottom) {
        // A second clip protects the status band during complete frames as well
        // as partial frames. This is especially important for rounded byte-color
        // fills, whose antialiased edge must never survive below the final full
        // row after maximizing, restoring, or crossing a DPI boundary.
        const float completeRowsBottom = std::min(bodyBottom,
            metrics_.headerHeight + metrics_.visibleRows * metrics_.rowHeight);
        d2dContext_->PushAxisAlignedClip(
            D2D1::RectF(0.0f, metrics_.headerHeight, metrics_.scrollBarLeft, completeRowsBottom),
            D2D1_ANTIALIAS_MODE_ALIASED);
        const LONG relativeTop = std::max<LONG>(0, damage.top - static_cast<LONG>(metrics_.headerHeight));
        const std::size_t firstDamagedRow = static_cast<std::size_t>(relativeTop / std::max(1.0f, metrics_.rowHeight));
        const LONG relativeBottom = std::max<LONG>(0, damage.bottom - static_cast<LONG>(metrics_.headerHeight));
        const std::size_t lastDamagedRow = std::min(metrics_.visibleRows,
            static_cast<std::size_t>(relativeBottom / std::max(1.0f, metrics_.rowHeight) + 1));
        // Convert the clipped vertical extent back to visible row indices. Only
        // bytes in those rows issue DirectWrite calls.
        for (std::size_t visible = firstDamagedRow; visible < lastDamagedRow; ++visible) {
            const std::size_t row = model.firstRow + visible;
            const std::size_t base = row * LayoutMetrics::bytesPerRow;
            if (base > dataSize) break;
            const float y = metrics_.headerHeight + visible * metrics_.rowHeight;
            brush_->SetColor(Color(palette.mutedText));
            const auto offset = OffsetText(base);
            d2dContext_->DrawTextW(offset.c_str(), static_cast<UINT32>(offset.size()), monoFormat_.Get(),
                D2D1::RectF(metrics_.addressX, y, metrics_.hexX - 8, y + metrics_.rowHeight), brush_.Get());
            for (std::size_t column = 0; column < LayoutMetrics::bytesPerRow && base + column < dataSize; ++column) {
                DrawByteCell(model, palette, base + column, y, true);
                if (metrics_.textPaneVisible) DrawByteCell(model, palette, base + column, y, false);
            }
            // EOF is a real caret position but not a stored byte. Rendering it
            // separately avoids saving an artificial trailing zero merely to
            // give the user somewhere to type the next character.
            if (model.caret == dataSize && model.caret >= base &&
                model.caret < base + LayoutMetrics::bytesPerRow) {
                DrawEndOfFileCaret(model, palette, y, true);
                if (metrics_.textPaneVisible) DrawEndOfFileCaret(model, palette, y, false);
            }
        }
        d2dContext_->PopAxisAlignedClip();
    }

    // Subtle vertical dividers preserve the three-column reading rhythm without
    // recreating a dense legacy grid around every individual byte cell.
    brush_->SetColor(Color(palette.grid));
    const float addressDivider = metrics_.hexX - std::round(14.0f * dpiScale_);
    const float textDivider = metrics_.textX - std::round(14.0f * dpiScale_);
    if (addressDivider < editorRect.right)
        d2dContext_->DrawLine({addressDivider, metrics_.tabStripHeight}, {addressDivider, editorRect.bottom}, brush_.Get());
    if (metrics_.textPaneVisible && textDivider < editorRect.right)
        d2dContext_->DrawLine({textDivider, metrics_.tabStripHeight}, {textDivider, editorRect.bottom}, brush_.Get());

    if (damage.right > metrics_.scrollBarLeft && damage.left < metrics_.scrollBarRight &&
        damage.bottom > metrics_.headerHeight && damage.top < statusTop) {
        DrawScrollBar(model, palette);
    }

    if (model.autoScrollActive) {
        const RECT marker = AutoScrollMarkerBounds(model.autoScrollAnchorX, model.autoScrollAnchorY);
        if (damage.right > marker.left && damage.left < marker.right &&
            damage.bottom > marker.top && damage.top < marker.bottom) {
            DrawAutoScrollMarker(model, palette);
        }
    }

    if (damage.bottom > statusTop) {
        const std::size_t low = std::min(model.selectionAnchor, model.selectionActive);
        const std::size_t high = std::max(model.selectionAnchor, model.selectionActive);
        wchar_t status[512]{};
        const unsigned long long selectionBytes = low >= dataSize ? 0ull :
            static_cast<unsigned long long>(std::min(high, dataSize - 1) - low + 1);
        swprintf_s(status, strings.statusFormat,
            model.fileName.c_str(), model.dirty ? L" *" : L"", static_cast<unsigned long long>(model.caret),
            selectionBytes, EncodingName(model.encoding).c_str(),
            static_cast<unsigned long long>(dataSize),
            model.hexPane ? strings.inputHexadecimal : strings.inputCharacters,
            model.insertMode ? strings.editModeInsert : strings.editModeOverwrite);
        if (model.searchInProgress) {
            // Search completion is asynchronous; an explicit status cue confirms
            // that Find was accepted without introducing an animation/render loop.
            wcscat_s(status, L"    ");
            wcscat_s(status, strings.statusSearching);
        }
        brush_->SetColor(Color(palette.mutedText));
        d2dContext_->DrawTextW(status, static_cast<UINT32>(wcslen(status)), uiFormat_.Get(),
            D2D1::RectF(metrics_.addressX, statusTop, metrics_.width - inset, metrics_.height), brush_.Get());
    }

    // Tooltips are composited last because they intentionally float over the
    // header and editor card. The outer R2PR clip still limits all raster work
    // and Present1 propagation to the current damage rectangle.
    DrawTabTooltip(model, palette);
}

void Renderer::DrawIoProgress(const RenderModel& model, const Palette& palette,
                              const RECT& damage) {
    const auto damageRect = D2D1::RectF(static_cast<float>(damage.left),
        static_cast<float>(damage.top), static_cast<float>(damage.right),
        static_cast<float>(damage.bottom));
    // Solid coverage makes the disabled state independent of whatever data is
    // currently owned by the I/O worker. A subtle neutral tint distinguishes the
    // surface from the normal editor in both light and dark modes.
    brush_->SetColor(Color(palette.surface));
    d2dContext_->FillRectangle(damageRect, brush_.Get());
    if (!palette.highContrast) {
        brush_->SetColor(palette.dark ? D2D1::ColorF(0.65f, 0.65f, 0.65f, 0.08f) :
                                       D2D1::ColorF(0.20f, 0.20f, 0.20f, 0.08f));
        d2dContext_->FillRectangle(damageRect, brush_.Get());
    }

    const D2D1_POINT_2F center{metrics_.width * 0.5f,
        metrics_.height * 0.5f - std::round(15.0f * dpiScale_)};
    const float orbit = std::round(18.0f * dpiScale_);
    const float dotRadius = std::max(2.0f, std::round(2.4f * dpiScale_));
    constexpr unsigned dotCount = 12;
    constexpr float tau = 6.2831853071795864769f;
    const unsigned head = static_cast<unsigned>(model.ioProgressPhase) % dotCount;
    for (unsigned index = 0; index < dotCount; ++index) {
        const unsigned age = (head + dotCount - index) % dotCount;
        const float alpha = palette.highContrast ? 1.0f :
            std::max(0.18f, 1.0f - static_cast<float>(age) / dotCount);
        const float angle = tau * static_cast<float>(index) / dotCount - tau * 0.25f;
        const D2D1_POINT_2F point{center.x + std::cos(angle) * orbit,
            center.y + std::sin(angle) * orbit};
        brush_->SetColor(Color(palette.highContrast ? palette.text : palette.caret, alpha));
        d2dContext_->FillEllipse(D2D1::Ellipse(point, dotRadius, dotRadius), brush_.Get());
    }

    if (!model.ioProgressText.empty()) {
        const float top = center.y + orbit + std::round(14.0f * dpiScale_);
        const auto textRect = D2D1::RectF(std::round(20.0f * dpiScale_), top,
            std::max(std::round(20.0f * dpiScale_), metrics_.width - std::round(20.0f * dpiScale_)),
            top + std::round(32.0f * dpiScale_));
        uiStrongFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        brush_->SetColor(Color(palette.highContrast ? palette.text : palette.mutedText));
        d2dContext_->DrawTextW(model.ioProgressText.c_str(),
            static_cast<UINT32>(model.ioProgressText.size()), uiStrongFormat_.Get(),
            textRect, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        uiStrongFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void Renderer::DrawTabTooltip(const RenderModel& model, const Palette& palette) {
    if (!model.tabTooltip || *model.tabTooltip >= model.tabs.size() ||
        !writeFactory_ || !tooltipFormat_) return;

    const std::size_t index = *model.tabTooltip;
    const D2D1_RECT_F tabRect = TabBounds(index, model.tabs.size(), model.firstVisibleTab);
    if (tabRect.left >= metrics_.width || tabRect.right <= tabRect.left) return;

    const float closeWidth = std::round(28.0f * dpiScale_);
    const float dirtyWidth = model.tabs[index].dirty ? std::round(14.0f * dpiScale_) : 0.0f;
    const float titleLeft = tabRect.left + std::round(10.0f * dpiScale_);
    const float titleRight = std::max(tabRect.left, tabRect.right - closeWidth - dirtyWidth);
    ComPtr<IDWriteTextLayout> tabLayout;
    if (!CreateTabTitleLayout(model.tabs[index].title, titleRight - titleLeft,
                              tabRect.bottom - tabRect.top, tabLayout) ||
        !IsTextLayoutTrimmed(tabLayout.Get())) return;

    const float outerMargin = std::round(8.0f * dpiScale_);
    const float horizontalPadding = std::round(10.0f * dpiScale_);
    const float verticalPadding = std::round(7.0f * dpiScale_);
    const float availableWidth = std::max(1.0f, metrics_.width - outerMargin * 2.0f - horizontalPadding * 2.0f);
    const float textWidth = std::min(std::round(480.0f * dpiScale_), availableWidth);
    const float availableHeight = std::max(1.0f,
        metrics_.height - metrics_.tabStripHeight - outerMargin * 2.0f - verticalPadding * 2.0f);

    ComPtr<IDWriteTextLayout> tooltipLayout;
    if (FAILED(writeFactory_->CreateTextLayout(model.tabs[index].title.c_str(),
        static_cast<UINT32>(model.tabs[index].title.size()), tooltipFormat_.Get(),
        textWidth, availableHeight, &tooltipLayout))) return;

    DWRITE_TEXT_METRICS textMetrics{};
    if (FAILED(tooltipLayout->GetMetrics(&textMetrics))) return;
    const float contentWidth = std::max(1.0f,
        std::ceil(std::min(textWidth, textMetrics.widthIncludingTrailingWhitespace)));
    const float contentHeight = std::max(1.0f,
        std::ceil(std::min(availableHeight, textMetrics.height)));
    const float tooltipWidth = contentWidth + horizontalPadding * 2.0f;
    const float tooltipHeight = contentHeight + verticalPadding * 2.0f;
    float left = std::clamp(tabRect.left, outerMargin,
        std::max(outerMargin, metrics_.width - outerMargin - tooltipWidth));
    const float top = metrics_.tabStripHeight + std::round(4.0f * dpiScale_);
    const auto tooltipRect = D2D1::RectF(left, top, left + tooltipWidth, top + tooltipHeight);
    const float radius = std::round(6.0f * dpiScale_);

    // A small translucent shadow is omitted in high contrast so every rendered
    // edge uses only the user's system palette.
    if (!palette.highContrast) {
        const float shadowOffset = std::max(1.0f, std::round(2.0f * dpiScale_));
        brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f));
        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(tooltipRect.left + shadowOffset, tooltipRect.top + shadowOffset,
                        tooltipRect.right + shadowOffset, tooltipRect.bottom + shadowOffset),
            radius, radius), brush_.Get());
    }
    brush_->SetColor(Color(palette.background));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(tooltipRect, radius, radius), brush_.Get());
    brush_->SetColor(Color(palette.highContrast ? palette.text : palette.grid));
    d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(tooltipRect, radius, radius),
        brush_.Get(), std::max(1.0f, dpiScale_));
    brush_->SetColor(Color(palette.text));
    d2dContext_->DrawTextLayout({tooltipRect.left + horizontalPadding,
        tooltipRect.top + verticalPadding}, tooltipLayout.Get(), brush_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Renderer::DrawScrollBar(const RenderModel& model, const Palette& palette) {
    const std::size_t dataSize = model.document ? model.document->Size() : 0;
    const std::size_t totalRows = dataSize / LayoutMetrics::bytesPerRow + 1;
    const ScrollBarGeometry geometry = GetScrollBarGeometry(model.firstRow, totalRows);
    if (!geometry.scrollable) return;

    const float center = (geometry.left + geometry.right) * 0.5f;
    const float trackWidth = std::max(1.0f, std::round((palette.highContrast ? 2.0f : 1.5f) * dpiScale_));
    const float trackRadius = trackWidth * 0.5f;
    brush_->SetColor(Color(palette.highContrast ? palette.grid : palette.mutedText,
                           palette.highContrast ? 1.0f : 0.28f));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(center - trackWidth * 0.5f, geometry.top,
                    center + trackWidth * 0.5f, geometry.bottom),
        trackRadius, trackRadius), brush_.Get());

    const bool emphasized = model.scrollBarHovered || model.scrollBarDragging || palette.highContrast;
    const float thumbWidth = std::max(3.0f,
        std::round((emphasized ? 8.0f : 5.0f) * dpiScale_));
    const float thumbRadius = thumbWidth * 0.5f;
    const std::uint32_t thumbColor = palette.highContrast ?
        ((model.scrollBarHovered || model.scrollBarDragging) ? palette.selection : palette.text) :
        (model.scrollBarDragging ? palette.caret :
            (model.scrollBarHovered ? palette.text : palette.mutedText));
    brush_->SetColor(Color(thumbColor, palette.highContrast || model.scrollBarDragging ? 1.0f :
                                      (model.scrollBarHovered ? 0.82f : 0.62f)));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(center - thumbWidth * 0.5f, geometry.thumbTop,
                    center + thumbWidth * 0.5f, geometry.thumbBottom),
        thumbRadius, thumbRadius), brush_.Get());
}

void Renderer::DrawAutoScrollMarker(const RenderModel& model, const Palette& palette) {
    const float completeRowsBottom = std::min(metrics_.height - metrics_.statusHeight,
        metrics_.headerHeight + metrics_.visibleRows * metrics_.rowHeight);
    d2dContext_->PushAxisAlignedClip(
        D2D1::RectF(0.0f, metrics_.headerHeight, metrics_.scrollBarLeft, completeRowsBottom),
        D2D1_ANTIALIAS_MODE_ALIASED);

    const D2D1_POINT_2F center{model.autoScrollAnchorX, model.autoScrollAnchorY};
    const float radius = std::round(16.0f * dpiScale_);
    const float stroke = std::max(1.0f, std::round((palette.highContrast ? 2.0f : 1.0f) * dpiScale_));
    brush_->SetColor(Color(palette.surface, palette.highContrast ? 1.0f : 0.96f));
    d2dContext_->FillEllipse(D2D1::Ellipse(center, radius, radius), brush_.Get());
    brush_->SetColor(Color(palette.highContrast ? palette.text : palette.grid));
    d2dContext_->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush_.Get(), stroke);

    const std::uint32_t glyphColor = palette.highContrast ? palette.selection : palette.caret;
    brush_->SetColor(Color(glyphColor));
    const float arm = std::round(4.0f * dpiScale_);
    const float separation = std::round(6.0f * dpiScale_);
    const float glyphStroke = std::max(2.0f, std::round(2.0f * dpiScale_));
    d2dContext_->DrawLine({center.x - arm, center.y - separation + arm},
        {center.x, center.y - separation}, brush_.Get(), glyphStroke);
    d2dContext_->DrawLine({center.x, center.y - separation},
        {center.x + arm, center.y - separation + arm}, brush_.Get(), glyphStroke);
    d2dContext_->DrawLine({center.x - arm, center.y + separation - arm},
        {center.x, center.y + separation}, brush_.Get(), glyphStroke);
    d2dContext_->DrawLine({center.x, center.y + separation},
        {center.x + arm, center.y + separation - arm}, brush_.Get(), glyphStroke);
    d2dContext_->FillEllipse(D2D1::Ellipse(center,
        std::max(1.5f, 2.0f * dpiScale_), std::max(1.5f, 2.0f * dpiScale_)), brush_.Get());

    d2dContext_->PopAxisAlignedClip();
}

std::uint32_t Renderer::ByteBackground(const RenderModel& model, const Palette& palette, std::size_t offset) const {
    const std::size_t low = std::min(model.selectionAnchor, model.selectionActive);
    const std::size_t high = std::max(model.selectionAnchor, model.selectionActive);
    if (offset >= low && offset <= high) return palette.selection;
    if (model.searchLength) {
        // upper_bound identifies the only preceding match that could contain this
        // byte; sorted offsets avoid scanning every search result per cell.
        const auto it = std::upper_bound(model.searchOffsets.begin(), model.searchOffsets.end(), offset);
        if (it != model.searchOffsets.begin()) {
            const std::size_t start = *std::prev(it);
            if (offset - start < model.searchLength) return palette.search;
        }
    }
    // Custom colors are collapsed to the system selection color in high contrast.
    for (const auto& mark : model.markups) if (offset >= mark.first && offset <= mark.last) return palette.highContrast ? palette.selection : mark.color;
    const std::uint8_t value = model.document ? model.document->ByteAt(offset) : 0;
    if (!palette.highContrast && model.profile->byteColors[value]) return *model.profile->byteColors[value];
    return palette.surface;
}

void Renderer::DrawByteCell(const RenderModel& model, const Palette& palette, std::size_t offset, float y, bool hexPane) {
    const std::size_t column = offset % LayoutMetrics::bytesPerRow;
    const float x = hexPane ? metrics_.hexX + column * metrics_.hexCellWidth : metrics_.textX + column * metrics_.textCellWidth;
    const float width = hexPane ? metrics_.hexCellWidth - 2 : metrics_.textCellWidth;
    const auto rect = D2D1::RectF(x, y + 1, x + width, y + metrics_.rowHeight - 1);
    const auto background = ByteBackground(model, palette, offset);
    const float radius = std::max(2.0f, 3.0f * dpiScale_);
    if (background != palette.surface) {
        brush_->SetColor(Color(background));
        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
    }
    if (offset == model.caret && model.hexPane == hexPane) {
        brush_->SetColor(Color(palette.caret));
        d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get(), std::max(1.0f, 2.0f * dpiScale_));
    }
    brush_->SetColor(Color((background == palette.selection) ? palette.selectionText : palette.text));
    wchar_t text[3]{};
    UINT32 length = 1;
    if (hexPane) {
        swprintf_s(text, L"%02X", model.document ? model.document->ByteAt(offset) : 0);
        length = 2;
    }
    else text[0] = ByteGlyph(model, offset);
    d2dContext_->DrawTextW(text, length, monoFormat_.Get(), rect, brush_.Get());
}

void Renderer::DrawEndOfFileCaret(const RenderModel& model, const Palette& palette, float y, bool hexPane) {
    const std::size_t dataSize = model.document ? model.document->Size() : 0;
    const std::size_t column = dataSize % LayoutMetrics::bytesPerRow;
    const float x = hexPane ? metrics_.hexX + column * metrics_.hexCellWidth :
                              metrics_.textX + column * metrics_.textCellWidth;
    const float width = hexPane ? metrics_.hexCellWidth - 2 : metrics_.textCellWidth;
    const auto rect = D2D1::RectF(x, y + 1, x + width, y + metrics_.rowHeight - 1);
    const float radius = std::max(2.0f, 3.0f * dpiScale_);
    const bool active = model.hexPane == hexPane;
    brush_->SetColor(Color(active ? palette.caret : palette.grid));
    d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get(),
        active ? std::max(1.0f, 2.0f * dpiScale_) : std::max(1.0f, dpiScale_));
}

wchar_t Renderer::ByteGlyph(const RenderModel& model, std::size_t offset) const {
    const std::size_t dataSize = model.document ? model.document->Size() : 0;
    const auto byteAt = [&model](std::size_t at) {
        return model.document ? model.document->ByteAt(at) : std::uint8_t{};
    };
    const auto value = byteAt(offset);
    // Variable-width encodings still occupy one cell per byte. Lead positions show
    // the decoded character and continuation/trailing bytes show a middle dot.
    if (model.encoding == TextEncoding::Utf16Le) {
        if ((offset & 1) || offset + 1 >= dataSize) return L'·';
        const wchar_t ch = static_cast<wchar_t>(value | (byteAt(offset + 1) << 8));
        return ch >= 0x20 && ch != 0x7f ? ch : L'·';
    }
    if (model.encoding == TextEncoding::Utf8 && value >= 0x80) {
        if ((value & 0xC0) == 0x80) return L'·';
        const int count = value < 0xE0 ? 2 : (value < 0xF0 ? 3 : 4);
        if (offset + count <= dataSize) {
            std::array<char, 4> encoded{};
            for (int i = 0; i < count; ++i) encoded[i] = static_cast<char>(byteAt(offset + i));
            wchar_t decoded[2]{};
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, encoded.data(), count, decoded, 2) > 0) return decoded[0];
        }
        return L'·';
    }
    if (model.encoding == TextEncoding::ShiftJis && value >= 0x80) {
        const bool lead = (value >= 0x81 && value <= 0x9f) || (value >= 0xe0 && value <= 0xfc);
        if (lead && offset + 1 < dataSize) {
            const char encoded[2]{static_cast<char>(value), static_cast<char>(byteAt(offset + 1))};
            wchar_t decoded{};
            if (MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, encoded, 2, &decoded, 1) == 1) return decoded;
        }
        if (value >= 0xA1 && value <= 0xDF) {
            const char encoded = static_cast<char>(value); wchar_t decoded{};
            if (MultiByteToWideChar(932, 0, &encoded, 1, &decoded, 1) == 1) return decoded;
        }
        return L'·';
    }
    return value >= 0x20 && value < 0x7f ? static_cast<wchar_t>(value) : L'·';
}

std::optional<std::pair<std::size_t, bool>> Renderer::HitTest(float x, float y, std::size_t firstRow, std::size_t dataSize) const {
    // Header, status, address gutter, and the gap between panes are deliberately
    // non-interactive. The bool result selects hex (true) or character (false).
    if (y < metrics_.headerHeight || y >= metrics_.height - metrics_.statusHeight) return std::nullopt;
    const std::size_t visibleRow = static_cast<std::size_t>((y - metrics_.headerHeight) / metrics_.rowHeight);
    // Residual pixels that cannot contain a complete row are intentionally
    // blank and must not move the caret into an undisplayed following row.
    if (visibleRow >= metrics_.visibleRows) return std::nullopt;
    const std::size_t row = firstRow + visibleRow;
    std::size_t column{}; bool hex{};
    if (x >= metrics_.hexX && x < metrics_.hexX + metrics_.hexCellWidth * LayoutMetrics::bytesPerRow) {
        column = static_cast<std::size_t>((x - metrics_.hexX) / metrics_.hexCellWidth); hex = true;
    } else if (metrics_.textPaneVisible && x >= metrics_.textX && x < metrics_.textX + metrics_.textCellWidth * LayoutMetrics::bytesPerRow) {
        column = static_cast<std::size_t>((x - metrics_.textX) / metrics_.textCellWidth); hex = false;
    } else return std::nullopt;
    const std::size_t offset = row * LayoutMetrics::bytesPerRow + column;
    // Exactly dataSize selects the virtual EOF cell; larger offsets remain
    // non-interactive so clicks in unused row space cannot create sparse data.
    if (offset > dataSize) return std::nullopt;
    return std::pair{offset, hex};
}

std::optional<TabHit> Renderer::HitTestTab(float x, float y, std::size_t tabCount,
                                           std::size_t firstVisibleTab) const {
    if (!IsInTabStrip(y) || tabCount == 0) return std::nullopt;
    const float margin = std::round(8.0f * dpiScale_);
    const float width = TabWidth(tabCount);
    if (x < margin) return std::nullopt;
    if (firstVisibleTab > tabCount) return std::nullopt;
    const std::size_t relative = static_cast<std::size_t>((x - margin) / width);
    if (relative > std::numeric_limits<std::size_t>::max() - firstVisibleTab)
        return std::nullopt;
    const std::size_t index = firstVisibleTab + relative;
    if (index >= tabCount) return std::nullopt;
    const float right = std::min(metrics_.width - margin,
        margin + (relative + 1) * width - std::max(1.0f, std::round(2.0f * dpiScale_)));
    if (x >= right) return std::nullopt;
    return TabHit{index, x >= right - std::round(28.0f * dpiScale_)};
}

std::size_t Renderer::TabDropIndex(float x, std::size_t tabCount,
                                   std::size_t firstVisibleTab) const {
    if (tabCount == 0) return 0;
    const float margin = std::round(8.0f * dpiScale_);
    const float width = TabWidth(tabCount);
    if (x <= margin) return std::min(firstVisibleTab, tabCount);
    const std::size_t relative = static_cast<std::size_t>(
        (x - margin + width * 0.5f) / width);
    if (relative > tabCount - std::min(firstVisibleTab, tabCount)) return tabCount;
    return std::min(tabCount, firstVisibleTab + relative);
}
