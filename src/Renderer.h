#pragma once

// Rendering contracts for the main editor surface.
// RenderModel is an immutable snapshot assembled by BinEditApp for one frame;
// Renderer never owns or mutates document, search, markup, or profile data.

#include "Document.h"
#include "Profile.h"

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

constexpr UINT WM_APP_R2PR_RENDER = WM_APP + 41;

// Inclusive byte interval painted with a user-selected background color.
struct MarkupRange {
    std::size_t first{};
    std::size_t last{};
    std::uint32_t color{};
};

// One application-owned tab-strip item. Titles are copied into the render model
// because inactive document sessions live outside the active editor fields.
struct RenderTab {
    std::wstring title;
    bool dirty{};
};

struct TabHit {
    std::size_t index{};
    bool closeButton{};
};

// Geometry shared by rendering and Win32 pointer routing. Keeping the mapping
// in Renderer guarantees that hit testing and pixels use identical DPI-rounded
// bounds, including for documents whose row count exceeds 32-bit scroll ranges.
struct ScrollBarGeometry {
    float left{};
    float top{};
    float right{};
    float bottom{};
    float thumbTop{};
    float thumbBottom{};
    bool scrollable{};
};

enum class ScrollBarPart { None, Thumb, TrackBefore, TrackAfter };

struct RenderModel {
    // The document is borrowed only for the synchronous Render call. ByteAt
    // traverses mapped/edited pieces without flattening a large file.
    const ByteDocument* document{};
    // Viewport and interaction state captured on the UI thread.
    std::size_t firstRow{};
    std::size_t caret{};
    std::size_t selectionAnchor{};
    std::size_t selectionActive{};
    bool hexPane{true};
    bool insertMode{true};
    bool dirty{};
    bool searchInProgress{};
    bool scrollBarHovered{};
    bool scrollBarDragging{};
    bool autoScrollActive{};
    // File I/O replaces the interactive editor with one opaque disabled surface.
    // The indeterminate phase advances only from the application timer.
    bool ioBusy{};
    float ioProgressPhase{};
    std::wstring ioProgressText;
    float autoScrollAnchorX{};
    float autoScrollAnchorY{};
    TextEncoding encoding{TextEncoding::Ascii};
    UiLanguage language{UiLanguage::Japanese};
    std::wstring fileName;
    // Sorted match starts permit logarithmic lookup for each visible byte.
    std::span<const std::size_t> searchOffsets;
    std::size_t searchLength{};
    std::span<const MarkupRange> markups;
    std::vector<RenderTab> tabs;
    std::size_t activeTab{};
    // Index of the leftmost tab represented in the horizontal strip viewport.
    // The application owns this state so wheel input and tab mutations remain
    // deterministic; Renderer only maps it to DPI-rounded geometry.
    std::size_t firstVisibleTab{};
    // Set only after the application-owned hover delay expires. Renderer still
    // verifies that the corresponding title is actually trimmed before showing
    // the in-surface D2D tooltip.
    std::optional<std::size_t> tabTooltip;
    const Profile* profile{};
};

struct LayoutMetrics {
    // Dimensions are in D2D device-independent units configured to equal physical
    // pixels. They are recalculated after resize or DPI change.
    float width{};
    float height{};
    float tabStripHeight{};
    float headerHeight{};
    float statusHeight{};
    float rowHeight{};
    float addressX{};
    float hexX{};
    float hexCellWidth{};
    float textX{};
    float textCellWidth{};
    // The custom D2D scrollbar occupies this right-side client-area gutter.
    // Editor column layout never extends into it.
    float scrollBarLeft{};
    float scrollBarRight{};
    float scrollBarTrackTop{};
    float scrollBarTrackBottom{};
    // Narrow windows keep the complete hexadecimal grid usable and hide the
    // secondary character pane instead of clipping it at an arbitrary column.
    bool textPaneVisible{true};
    // Counts only rows whose complete height fits between the header and status
    // bands. Any remaining vertical pixels stay blank and non-interactive.
    std::size_t visibleRows{};
    static constexpr std::size_t bytesPerRow = 16;
};

enum class R2PRRegion { Full, Tabs, Header, Data, Status, ScrollBar, ByteCell, IoProgress };

// Require-To-Partial-Re-Render: damage is accumulated until the next presentation.
// A missing requirement means no BeginDraw and no Present call at all.
class R2PRDamage {
public:
    // Unions a new physical-pixel rectangle into the pending damage envelope.
    void Require(RECT rect);
    // Promotes all accumulated damage to a complete-surface redraw.
    void RequireFull();
    [[nodiscard]] bool Pending() const noexcept { return full_ || dirty_.has_value(); }
    // Clips and clears pending damage. nullopt means drawing and Present must be skipped.
    [[nodiscard]] std::optional<RECT> Consume(RECT bounds);
private:
    bool full_{};
    std::optional<RECT> dirty_;
};

class Renderer {
public:
    // Binds this renderer to one HWND for its entire lifetime.
    bool Initialize(HWND window);
    void Resize(UINT width, UINT height);
    void DpiChanged(UINT dpi);
    void DiscardDeviceResources();
    // Converts a semantic invalidation into a physical damage rectangle.
    void Require(R2PRRegion region, std::optional<std::size_t> byteOffset = std::nullopt, std::size_t firstRow = 0);
    void RequireRect(RECT rect);
    [[nodiscard]] bool HasPendingFrame() const noexcept { return damage_.Pending(); }
    // Consumes one pending R2PR frame. A call with no damage is intentionally free.
    bool Render(const RenderModel& model);
    [[nodiscard]] const LayoutMetrics& Metrics() const noexcept { return metrics_; }
    [[nodiscard]] std::optional<std::pair<std::size_t, bool>> HitTest(float x, float y, std::size_t firstRow, std::size_t dataSize) const;
    [[nodiscard]] std::optional<TabHit> HitTestTab(float x, float y,
                                                   std::size_t tabCount,
                                                   std::size_t firstVisibleTab) const;
    [[nodiscard]] bool IsInTabStrip(float y) const noexcept { return y >= 0 && y < metrics_.tabStripHeight; }
    [[nodiscard]] std::size_t TabDropIndex(float x, std::size_t tabCount,
                                           std::size_t firstVisibleTab) const;
    // Largest valid leftmost index for the current client width. A zero result
    // means every tab fits and Shift+wheel has no horizontal work to perform.
    [[nodiscard]] std::size_t MaximumFirstVisibleTab(std::size_t tabCount) const noexcept;
    [[nodiscard]] ScrollBarGeometry GetScrollBarGeometry(std::size_t firstRow, std::size_t totalRows) const noexcept;
    [[nodiscard]] ScrollBarPart HitTestScrollBar(float x, float y, std::size_t firstRow,
                                                 std::size_t totalRows) const noexcept;
    // Converts a dragged thumb top back into a document row without the 32-bit
    // position limit imposed by SCROLLINFO.
    [[nodiscard]] std::size_t ScrollRowFromThumb(float pointerY, float grabOffset,
                                                 std::size_t totalRows) const noexcept;
    [[nodiscard]] bool IsInDataViewport(float x, float y) const noexcept;
    [[nodiscard]] RECT AutoScrollMarkerBounds(float x, float y) const noexcept;

private:
    // Factory/text resources do not depend on the DXGI device.
    bool CreateDeviceIndependentResources();
    bool CreateTextFormats();
    bool CreateDeviceResources();
    bool CreateTargetBitmap();
    void UpdateMetrics();
    [[nodiscard]] float TabWidth(std::size_t tabCount) const;
    [[nodiscard]] D2D1_RECT_F TabBounds(std::size_t index, std::size_t tabCount,
                                        std::size_t firstVisibleTab) const;
    [[nodiscard]] bool CreateTabTitleLayout(const std::wstring& title, float width,
                                            float height,
                                            Microsoft::WRL::ComPtr<IDWriteTextLayout>& layout) const;
    [[nodiscard]] static bool IsTextLayoutTrimmed(IDWriteTextLayout* layout) noexcept;
    void DrawScene(const RenderModel& model, const Palette& palette, const RECT& damage);
    void DrawIoProgress(const RenderModel& model, const Palette& palette, const RECT& damage);
    void DrawTabTooltip(const RenderModel& model, const Palette& palette);
    void DrawScrollBar(const RenderModel& model, const Palette& palette);
    void DrawAutoScrollMarker(const RenderModel& model, const Palette& palette);
    void DrawByteCell(const RenderModel& model, const Palette& palette, std::size_t offset, float y, bool hexPane);
    // Paints the non-persistent insertion cell at bytes.size().
    void DrawEndOfFileCaret(const RenderModel& model, const Palette& palette, float y, bool hexPane);
    // Resolves overlapping visual layers in priority order: selection, search,
    // markup, per-byte coloring, then the editor background.
    std::uint32_t ByteBackground(const RenderModel& model, const Palette& palette, std::size_t offset) const;
    // Decodes the character-pane cell while preserving one visual cell per byte.
    wchar_t ByteGlyph(const RenderModel& model, std::size_t offset) const;
    static D2D1_COLOR_F Color(std::uint32_t rgb, float alpha = 1.0f);

    HWND window_{};
    UINT pixelWidth_{};
    UINT pixelHeight_{};
    float dpiScale_{1.0f};
    LayoutMetrics metrics_;
    R2PRDamage damage_;

    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2dContext_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> monoFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> uiFormat_;
    // Column headers use WinUI's semibold hierarchy while status text remains
    // regular weight. Separate formats avoid state changes inside R2PR frames.
    Microsoft::WRL::ComPtr<IDWriteTextFormat> uiStrongFormat_;
    // Tab labels use DirectWrite's native ellipsis inline object. The tooltip
    // format permits character wrapping so even a maximum-length file name can
    // be displayed without leaving the client area.
    Microsoft::WRL::ComPtr<IDWriteInlineObject> tabEllipsis_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tooltipFormat_;
    // A two-buffer flip chain needs both buffers initialized before partial draws.
    unsigned fullRedrawsRemaining_{};
};
