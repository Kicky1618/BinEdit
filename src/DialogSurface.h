#pragma once

// Lightweight immediate-mode drawing facade for custom dialog windows.
// Coordinates are physical pixels because the D2D target uses 96 DPI; callers
// multiply logical measurements by Scale() to remain Per-Monitor-V2 aware.

#include "Profile.h"

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// One stop in a dialog-owned linear gradient. Opacity enables compositing the
// white and black overlays used by an HSV saturation/value plane.
struct DialogGradientStop {
    float position{};
    std::uint32_t color{};
    float opacity{1.0f};
};

// Physical-pixel rectangles used to center a modal surface. A null or destroyed
// owner falls back to the primary work area instead of reaching nullable Win32
// APIs or terminating the process during startup/error presentation.
struct DialogPlacement {
    RECT anchor{};
    RECT workArea{};
};

[[nodiscard]] DialogPlacement ResolveDialogPlacement(HWND owner) noexcept;

class D2DDialogSurface {
public:
    // Creates factories, the D3D device, flip-sequential swap chain, and D2D target.
    // Returns false without displaying UI; the owning dialog decides how to abort.
    bool Initialize(HWND window);
    // Rebinds the DXGI back buffer after a client-area size change.
    void Resize(UINT width, UINT height);
    // Recreates only DPI-dependent text formats; GPU resources remain valid.
    void DpiChanged(UINT dpi);
    // Begins an immediate-mode frame and clears it with the resolved background.
    bool Begin(const Palette& palette);
    // Ends D2D drawing and presents at vertical sync. False requests a retry after
    // a device-loss recovery path has recreated the graphics resources.
    bool End();

    // Drawing primitives intentionally accept 0xRRGGBB colors to keep Win32's
    // COLORREF byte order out of dialog layout code.
    void Fill(D2D1_RECT_F rect, std::uint32_t color);
    void Stroke(D2D1_RECT_F rect, std::uint32_t color, float width = 1.0f);
    // WinUI-style controls consistently use small corner radii. Keeping rounded
    // geometry here avoids duplicating D2D construction in each custom dialog.
    void FillRounded(D2D1_RECT_F rect, float radius, std::uint32_t color);
    void StrokeRounded(D2D1_RECT_F rect, float radius, std::uint32_t color, float width = 1.0f);
    // Creates a short-lived D2D gradient brush for one immediate-mode fill.
    // Stops are borrowed only for the duration of this call.
    void FillLinearGradient(D2D1_RECT_F rect, std::span<const DialogGradientStop> stops, bool vertical);
    void Line(D2D1_POINT_2F from, D2D1_POINT_2F to, std::uint32_t color, float width = 1.0f);
    void Ellipse(D2D1_ELLIPSE ellipse, std::uint32_t color, float width = 1.0f, bool fill = false);
    // Converts an application HICON through WIC and draws it into the active D2D
    // frame. The icon remains owned by the caller and is borrowed only here.
    bool DrawIcon(HICON icon, D2D1_RECT_F rect);
    void Text(std::wstring_view text, D2D1_RECT_F rect, std::uint32_t color, bool centered = false, bool bold = false);
    // Title and caption helpers provide Fluent typography hierarchy without
    // exposing mutable DirectWrite formats to the dialog implementations.
    void TitleText(std::wstring_view text, D2D1_RECT_F rect, std::uint32_t color, bool centered = false);
    void CaptionText(std::wstring_view text, D2D1_RECT_F rect, std::uint32_t color);
    // Returns the horizontal advance to a UTF-16 insertion point. Search input
    // uses this for caret placement and horizontal scrolling.
    float TextPosition(std::wstring_view text, std::size_t position, float maxWidth);
    // Clips long input text to the custom edit field.
    void PushClip(D2D1_RECT_F rect);
    void PopClip();

    [[nodiscard]] float Scale() const noexcept { return scale_; }
    [[nodiscard]] float Width() const noexcept { return static_cast<float>(width_); }
    [[nodiscard]] float Height() const noexcept { return static_cast<float>(height_); }

private:
    // Device-independent resources survive a lost D3D device.
    bool CreateIndependentResources();
    bool CreateFormats();
    bool CreateDeviceResources();
    bool CreateTarget();
    void DiscardDeviceResources();
    static D2D1_COLOR_F Color(std::uint32_t rgb);

    HWND window_{}; // Non-owning; the dialog destroys its HWND after this object.
    UINT width_{};
    UINT height_{};
    float scale_{1.0f};
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> centerFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> boldFormat_;
    // DirectWrite stores weight and alignment in the text format itself. Keep a
    // dedicated combined format so a primary button can be both semibold and
    // horizontally centered; choosing boldFormat_ alone would left-align it.
    Microsoft::WRL::ComPtr<IDWriteTextFormat> centerBoldFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> centerTitleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> captionFormat_;
};
