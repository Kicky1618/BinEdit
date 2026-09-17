// Implements a small, fully application-owned GDI message dialog.
//
// Message boxes are often needed precisely when graphics initialization or a
// file operation has failed. This implementation therefore avoids D3D11, D2D1,
// DirectWrite, and custom child controls. A single HWND, synchronous modal loop,
// double-buffered GDI paint, and explicit keyboard navigation form the complete
// dependency surface.

#include "MessageDialog.h"

#include "resource.h"
#include "Security.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace {
constexpr wchar_t kMessageDialogClass[] = L"BinEdit.MessageDialog.Gdi";
// All layout constants below are expressed at Windows' 96-DPI (100%) baseline.
// Actual monitor DPI is applied exactly once through Scale.
constexpr UINT kLogicalDpi = 96;
constexpr int kLogicalWidth = 560;
constexpr int kMinimumLogicalHeight = 228;
constexpr int kMaximumLogicalHeight = 420;
constexpr int kIconSupersample = 4;
// Keep the semantic glyph comfortably inside its colored badge. At the old
// full-box scale, the exclamation mark, cross, and question mark dominated the
// 38-DIP icon and appeared larger than the adjacent 15-DIP body typography.
constexpr int kIconSymbolInset = 120;
constexpr DWORD kDialogStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME;

// Linear interpolation is sufficient for UI hover fills because all palette
// values are already display-space sRGB colors.
std::uint32_t Blend(std::uint32_t foreground, std::uint32_t background, unsigned foregroundWeight) {
    const auto channel = [foreground, background, foregroundWeight](unsigned shift) {
        const unsigned a = (foreground >> shift) & 0xffu;
        const unsigned b = (background >> shift) & 0xffu;
        return (a * foregroundWeight + b * (255u - foregroundWeight) + 127u) / 255u;
    };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

int Scale(int logical, UINT dpi) {
    return MulDiv(logical, static_cast<int>(dpi), static_cast<int>(kLogicalDpi));
}

HFONT CreateUiFont(UINT dpi, int logicalSize, int weight) {
    return CreateFontW(-Scale(logicalSize, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
}

void FillSolid(HDC dc, const RECT& rect, std::uint32_t color) {
    HBRUSH brush = CreateSolidBrush(RgbToColorRef(color));
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void FillRounded(HDC dc, const RECT& rect, int radius, std::uint32_t color) {
    HBRUSH brush = CreateSolidBrush(RgbToColorRef(color));
    HPEN pen = CreatePen(PS_NULL, 0, RgbToColorRef(color));
    const HGDIOBJ oldBrush = SelectObject(dc, brush);
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void StrokeRounded(HDC dc, const RECT& rect, int radius, int width, std::uint32_t color) {
    HPEN pen = CreatePen(PS_SOLID, width, RgbToColorRef(color));
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// Maps one thousandth of a local icon box to a device coordinate. Normalized
// geometry keeps every symbol optically consistent at arbitrary monitor DPIs.
int IconCoordinate(int origin, int extent, int normalized) {
    return origin + MulDiv(extent, normalized, 1000);
}

RECT IconRectangle(const RECT& bounds, int left, int top, int right, int bottom) {
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    return {IconCoordinate(bounds.left, width, left), IconCoordinate(bounds.top, height, top),
            IconCoordinate(bounds.left, width, right), IconCoordinate(bounds.top, height, bottom)};
}

void FillEllipse(HDC dc, const RECT& bounds, std::uint32_t color) {
    HBRUSH brush = CreateSolidBrush(RgbToColorRef(color));
    if (!brush) return;
    const HGDIOBJ oldBrush = SelectObject(dc, brush);
    const HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

HPEN CreateRoundedPen(int width, std::uint32_t color) {
    LOGBRUSH brush{BS_SOLID, RgbToColorRef(color), 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
        static_cast<DWORD>(std::max(1, width)), &brush, 0, nullptr);
    // The geometric style is supported by normal display and memory DCs. Keep a
    // cosmetic fallback for low-resource or unusual printer-compatible targets.
    if (!pen) pen = CreatePen(PS_SOLID, std::max(1, width), RgbToColorRef(color));
    return pen;
}

void StrokeLine(HDC dc, POINT from, POINT to, int width, std::uint32_t color) {
    HPEN pen = CreateRoundedPen(width, color);
    if (!pen) return;
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, from.x, from.y, nullptr);
    LineTo(dc, to.x, to.y);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void FillWarningTriangle(HDC dc, const RECT& bounds, std::uint32_t color) {
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const auto point = [&](int x, int y) {
        return POINT{IconCoordinate(bounds.left, width, x), IconCoordinate(bounds.top, height, y)};
    };

    HBRUSH brush = CreateSolidBrush(RgbToColorRef(color));
    if (!brush) return;
    const HGDIOBJ oldBrush = SelectObject(dc, brush);
    const HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));

    // Cubic corners remove the sharp, uneven joins produced by a three-point
    // Polygon call, especially after fractional Per-Monitor-V2 scaling.
    if (BeginPath(dc)) {
        const POINT top = point(500, 45);
        MoveToEx(dc, top.x, top.y, nullptr);
        std::array<POINT, 3> curve{point(522, 45), point(541, 58), point(555, 84)};
        PolyBezierTo(dc, curve.data(), static_cast<DWORD>(curve.size()));
        const POINT lowerRight = point(948, 823);
        LineTo(dc, lowerRight.x, lowerRight.y);
        curve = {point(962, 850), point(961, 876), point(946, 899)};
        PolyBezierTo(dc, curve.data(), static_cast<DWORD>(curve.size()));
        curve = {point(932, 923), point(907, 935), point(878, 935)};
        PolyBezierTo(dc, curve.data(), static_cast<DWORD>(curve.size()));
        const POINT lowerLeft = point(122, 935);
        LineTo(dc, lowerLeft.x, lowerLeft.y);
        curve = {point(93, 935), point(68, 923), point(54, 899)};
        PolyBezierTo(dc, curve.data(), static_cast<DWORD>(curve.size()));
        curve = {point(39, 876), point(38, 850), point(52, 823)};
        PolyBezierTo(dc, curve.data(), static_cast<DWORD>(curve.size()));
        const POINT upperLeft = point(445, 84);
        LineTo(dc, upperLeft.x, upperLeft.y);
        curve = {point(459, 58), point(478, 45), point(500, 45)};
        PolyBezierTo(dc, curve.data(), static_cast<DWORD>(curve.size()));
        CloseFigure(dc);
        if (EndPath(dc)) FillPath(dc);
    }

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

void DrawMessageIconGeometry(HDC dc, const RECT& bounds, MessageDialogIcon icon,
    std::uint32_t fillColor, std::uint32_t symbolColor) {
    const RECT symbolBounds = IconRectangle(bounds, kIconSymbolInset, kIconSymbolInset,
        1000 - kIconSymbolInset, 1000 - kIconSymbolInset);
    const int width = symbolBounds.right - symbolBounds.left;
    const int height = symbolBounds.bottom - symbolBounds.top;
    const auto point = [&](int x, int y) {
        return POINT{IconCoordinate(symbolBounds.left, width, x),
                     IconCoordinate(symbolBounds.top, height, y)};
    };
    const int symbolStroke = std::max(1, MulDiv(std::min(width, height), 82, 1000));

    if (icon == MessageDialogIcon::Warning)
        FillWarningTriangle(dc, bounds, fillColor);
    else
        FillEllipse(dc, IconRectangle(bounds, 25, 25, 975, 975), fillColor);

    switch (icon) {
    case MessageDialogIcon::Information:
        FillEllipse(dc, IconRectangle(symbolBounds, 435, 190, 565, 320), symbolColor);
        StrokeLine(dc, point(500, 430), point(500, 780), symbolStroke, symbolColor);
        break;
    case MessageDialogIcon::Warning:
        StrokeLine(dc, point(500, 350), point(500, 650), symbolStroke, symbolColor);
        FillEllipse(dc, IconRectangle(symbolBounds, 435, 735, 565, 865), symbolColor);
        break;
    case MessageDialogIcon::Error:
        StrokeLine(dc, point(335, 335), point(665, 665), symbolStroke, symbolColor);
        StrokeLine(dc, point(665, 335), point(335, 665), symbolStroke, symbolColor);
        break;
    case MessageDialogIcon::Question: {
        HPEN pen = CreateRoundedPen(symbolStroke, symbolColor);
        if (pen) {
            const HGDIOBJ oldPen = SelectObject(dc, pen);
            const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            if (BeginPath(dc)) {
                const POINT start = point(335, 370);
                MoveToEx(dc, start.x, start.y, nullptr);
                std::array<POINT, 3> curve{point(350, 205), point(650, 185), point(690, 370)};
                PolyBezierTo(dc, curve.data(), static_cast<DWORD>(curve.size()));
                curve = {point(715, 505), point(520, 545), point(510, 665)};
                PolyBezierTo(dc, curve.data(), static_cast<DWORD>(curve.size()));
                if (EndPath(dc)) StrokePath(dc);
            }
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        FillEllipse(dc, IconRectangle(symbolBounds, 445, 755, 575, 885), symbolColor);
        break;
    }
    }
}

void DrawMessageIcon(HDC dc, const RECT& bounds, MessageDialogIcon icon,
    std::uint32_t surfaceColor, std::uint32_t fillColor, std::uint32_t symbolColor) {
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return;

    HDC layer = CreateCompatibleDC(dc);
    HBITMAP bitmap = layer ? CreateCompatibleBitmap(dc, width * kIconSupersample,
        height * kIconSupersample) : nullptr;
    if (!layer || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (layer) DeleteDC(layer);
        DrawMessageIconGeometry(dc, bounds, icon, fillColor, symbolColor);
        return;
    }

    const HGDIOBJ oldBitmap = SelectObject(layer, bitmap);
    const RECT highResolution{0, 0, width * kIconSupersample, height * kIconSupersample};
    FillSolid(layer, highResolution, surfaceColor);
    DrawMessageIconGeometry(layer, highResolution, icon, fillColor, symbolColor);

    // GDI has no general path antialiasing mode. Drawing at four times the target
    // resolution and filtering once produces stable edges without depending on
    // Direct2D, GDI+, WIC, or an external graphics runtime.
    const int previousMode = SetStretchBltMode(dc, HALFTONE);
    POINT previousOrigin{};
    const BOOL changedOrigin = SetBrushOrgEx(dc, 0, 0, &previousOrigin);
    StretchBlt(dc, bounds.left, bounds.top, width, height, layer, 0, 0,
        highResolution.right, highResolution.bottom, SRCCOPY);
    if (changedOrigin) SetBrushOrgEx(dc, previousOrigin.x, previousOrigin.y, nullptr);
    if (previousMode) SetStretchBltMode(dc, previousMode);

    SelectObject(layer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(layer);
}

class GdiMessageDialog {
public:
    GdiMessageDialog(HWND owner, HINSTANCE instance, std::wstring_view title,
        std::wstring_view message, MessageDialogButtons buttons, MessageDialogIcon icon,
        const Profile& profile, UiLanguage language)
        : owner_(owner), instance_(instance ? instance : GetModuleHandleW(nullptr)),
          title_(title), message_(message), buttons_(buttons), icon_(icon),
          profile_(profile), language_(language) {}

    MessageDialogResult Show() {
        RegisterWindowClass();
        dpi_ = owner_ && IsWindow(owner_) ? GetDpiForWindow(owner_) : GetDpiForSystem();
        const SIZE windowSize = MeasureWindowSize(dpi_);
        const int width = windowSize.cx;
        const int height = windowSize.cy;
        const RECT anchor = OwnerOrWorkArea();
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
        // Center relative to the owner, then clamp the complete dialog to the
        // selected monitor's work area. This remains correct when the owner spans
        // monitors with different scale factors or is smaller than the dialog.
        const LONG centeredX = anchor.left + ((anchor.right - anchor.left) - width) / 2;
        const LONG centeredY = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
        const LONG x = std::clamp(centeredX, monitor.rcWork.left,
            std::max(monitor.rcWork.left, monitor.rcWork.right - width));
        const LONG y = std::clamp(centeredY, monitor.rcWork.top,
            std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
        ownerWasEnabled_ = owner_ && IsWindow(owner_) && IsWindowEnabled(owner_);
        window_ = CreateWindowExW(kDialogExStyle, kMessageDialogClass, title_.c_str(),
            kDialogStyle, x, y, width, height, owner_, nullptr,
            instance_, this);
        if (!window_) return SafeFailureResult();

        if (ownerWasEnabled_) EnableWindow(owner_, FALSE);
        ApplyWindowTheme();
        ShowWindow(window_, SW_SHOWNORMAL);
        UpdateWindow(window_);
        SetForegroundWindow(window_);
        SetFocus(window_);

        MSG message{};
        bool repostQuit = false;
        int quitCode = 0;
        while (window_) {
            const BOOL status = GetMessageW(&message, nullptr, 0, 0);
            if (status <= 0) {
                if (status == 0) { repostQuit = true; quitCode = static_cast<int>(message.wParam); }
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        // A GetMessage failure or process quit can end the loop without a button
        // result. Close any remaining HWND before restoring the owner.
        if (window_) DestroyWindow(window_);

        if (ownerWasEnabled_ && owner_ && IsWindow(owner_)) {
            EnableWindow(owner_, TRUE);
            SetActiveWindow(owner_);
            SetFocus(owner_);
        }
        if (repostQuit) PostQuitMessage(quitCode);
        return result_ == MessageDialogResult::None ? SafeFailureResult() : result_;
    }

private:
    struct ButtonState {
        RECT bounds{};
        MessageDialogResult result{MessageDialogResult::None};
        const wchar_t* label{};
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        try {
            auto* dialog = reinterpret_cast<GdiMessageDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE) {
                const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                dialog = static_cast<GdiMessageDialog*>(create->lpCreateParams);
                dialog->window_ = window;
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            }
            return dialog ? dialog->HandleMessage(message, wParam, lParam)
                          : DefWindowProcW(window, message, wParam, lParam);
        } catch (...) {
            Security::FailFast(L"GdiMessageDialog::WindowProc");
        }
    }

    void RegisterWindowClass() const {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_ICON1));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = kMessageDialogClass;
        RegisterClassExW(&wc);
    }

    int MeasureLogicalHeight(UINT targetDpi) const {
        // Measure with a font created for the destination monitor, not the current
        // window DPI. Per-Monitor-V2 can therefore choose a new wrapped height
        // before Windows commits the monitor transition.
        HWND dcOwner = owner_ && IsWindow(owner_) ? owner_ : nullptr;
        HDC dc = GetDC(dcOwner);
        if (!dc && dcOwner) {
            dcOwner = nullptr;
            dc = GetDC(nullptr);
        }
        if (!dc) return 280;
        HFONT font = CreateUiFont(targetDpi, 15, FW_NORMAL);
        const HGDIOBJ oldFont = SelectObject(dc, font);
        RECT measure{0, 0, Scale(420, targetDpi), 0};
        DrawTextW(dc, message_.c_str(), static_cast<int>(message_.size()), &measure,
            DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        ReleaseDC(dcOwner, dc);
        const int messageLogicalHeight = MulDiv(measure.bottom - measure.top,
            static_cast<int>(kLogicalDpi), static_cast<int>(targetDpi));
        return std::clamp(messageLogicalHeight + 142, kMinimumLogicalHeight, kMaximumLogicalHeight);
    }

    SIZE MeasureWindowSize(UINT targetDpi) const {
        // The requested dimensions are client pixels. Include the actual modal
        // frame style at the target DPI so the resulting client area retains its
        // logical width and dynamically measured text height.
        const int logicalHeight = MeasureLogicalHeight(targetDpi);
        RECT bounds{0, 0, Scale(kLogicalWidth, targetDpi), Scale(logicalHeight, targetDpi)};
        AdjustWindowRectExForDpi(&bounds, kDialogStyle, FALSE, kDialogExStyle, targetDpi);
        return {bounds.right - bounds.left, bounds.bottom - bounds.top};
    }

    RECT ClampToMonitorWorkArea(RECT bounds) const {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &monitor)) return bounds;
        const LONG width = bounds.right - bounds.left;
        const LONG height = bounds.bottom - bounds.top;
        bounds.left = std::clamp(bounds.left, monitor.rcWork.left,
            std::max(monitor.rcWork.left, monitor.rcWork.right - width));
        bounds.top = std::clamp(bounds.top, monitor.rcWork.top,
            std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
        bounds.right = bounds.left + width;
        bounds.bottom = bounds.top + height;
        return bounds;
    }

    RECT OwnerOrWorkArea() const {
        RECT anchor{};
        if (owner_ && IsWindow(owner_) && GetWindowRect(owner_, &anchor)) return anchor;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
        return anchor;
    }

    void ApplyWindowTheme() {
        // SetWindowTheme can synchronously send WM_THEMECHANGED. The guard keeps
        // the non-D3D fallback dialog from recursively reapplying its own theme.
        if (applyingTheme_) return;
        applyingTheme_ = true;
        const Palette palette = profile_.ResolvePalette();
        BOOL dark = palette.dark && !palette.highContrast;
        constexpr DWORD immersiveDarkMode = 20;
        DwmSetWindowAttribute(window_, immersiveDarkMode, &dark, sizeof(dark));
        SetWindowTheme(window_, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        applyingTheme_ = false;
    }

    void BuildButtonLayout(const RECT& client) {
        const auto& strings = GetStrings(language_);
        buttonCount_ = buttons_ == MessageDialogButtons::Ok ? 1u :
                       (buttons_ == MessageDialogButtons::YesNoCancel ? 3u : 2u);
        if (buttons_ == MessageDialogButtons::Ok) {
            buttonStates_[0].result = MessageDialogResult::Ok;
            buttonStates_[0].label = strings.dialogOk;
        } else {
            buttonStates_[0].result = MessageDialogResult::Yes;
            buttonStates_[0].label = buttons_ == MessageDialogButtons::ReloadContinue ?
                strings.dialogReload : strings.dialogYes;
            buttonStates_[1].result = MessageDialogResult::No;
            buttonStates_[1].label = buttons_ == MessageDialogButtons::ReloadContinue ?
                strings.dialogContinue : strings.dialogNo;
            if (buttonCount_ == 3) {
                buttonStates_[2].result = MessageDialogResult::Cancel;
                buttonStates_[2].label = strings.dialogCancel;
            }
        }

        const int width = Scale(108, dpi_);
        const int height = Scale(34, dpi_);
        const int gap = Scale(8, dpi_);
        const int right = client.right - Scale(20, dpi_);
        const int top = client.bottom - Scale(54, dpi_);
        const int total = static_cast<int>(buttonCount_) * width +
                          static_cast<int>(buttonCount_ - 1) * gap;
        int x = right - total;
        for (std::size_t index = 0; index < buttonCount_; ++index) {
            buttonStates_[index].bounds = {x, top, x + width, top + height};
            x += width + gap;
        }
    }

    void RelayoutButtons() {
        // Hit-test rectangles must change synchronously with the HWND. Waiting for
        // WM_PAINT would leave a short interval where scaled buttons draw in one
        // location but mouse input still targets the previous monitor's layout.
        if (!window_) return;
        RECT client{};
        if (GetClientRect(window_, &client)) BuildButtonLayout(client);
    }

    int HitTestButton(POINT point) const {
        for (std::size_t index = 0; index < buttonCount_; ++index) {
            if (PtInRect(&buttonStates_[index].bounds, point)) return static_cast<int>(index);
        }
        return -1;
    }

    void ActivateButton(std::size_t index) {
        if (index < buttonCount_) Finish(buttonStates_[index].result);
    }

    void Finish(MessageDialogResult result) {
        result_ = result;
        if (window_) DestroyWindow(window_);
    }

    MessageDialogResult CancelResult() const {
        if (buttons_ == MessageDialogButtons::YesNoCancel) return MessageDialogResult::Cancel;
        if (buttons_ == MessageDialogButtons::YesNo ||
            buttons_ == MessageDialogButtons::ReloadContinue) return MessageDialogResult::No;
        return MessageDialogResult::Ok;
    }

    MessageDialogResult SafeFailureResult() const {
        // Confirmation failures must never silently approve a destructive action.
        return buttons_ == MessageDialogButtons::Ok ? MessageDialogResult::Ok : CancelResult();
    }

    void Draw(HDC dc, const RECT& client) {
        const Palette palette = profile_.ResolvePalette();
        const int radius = Scale(8, dpi_);
        // DrawText defaults to OPAQUE on a newly created compatible DC. Keep all
        // body and button text transparent so its bounding rectangle never
        // replaces the themed surface with the DC's default white background.
        const int previousBackgroundMode = SetBkMode(dc, TRANSPARENT);
        FillSolid(dc, client, palette.surface);

        RECT actions{0, client.bottom - Scale(72, dpi_), client.right, client.bottom};
        FillSolid(dc, actions, palette.background);
        RECT separator{0, actions.top, client.right, actions.top + std::max(1, Scale(1, dpi_))};
        FillSolid(dc, separator, palette.grid);

        const int iconSize = Scale(38, dpi_);
        RECT iconRect{Scale(24, dpi_), Scale(28, dpi_), Scale(24, dpi_) + iconSize,
                      Scale(28, dpi_) + iconSize};
        DrawMessageIcon(dc, iconRect, icon_, palette.surface,
            palette.selection, palette.selectionText);

        HFONT bodyFont = CreateUiFont(dpi_, 15, FW_NORMAL);
        const HGDIOBJ oldBodyFont = SelectObject(dc, bodyFont);
        SetTextColor(dc, RgbToColorRef(palette.text));
        RECT messageRect{Scale(82, dpi_), Scale(27, dpi_), client.right - Scale(24, dpi_), actions.top - Scale(18, dpi_)};
        DrawTextW(dc, message_.c_str(), static_cast<int>(message_.size()), &messageRect,
            DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(dc, oldBodyFont);
        DeleteObject(bodyFont);

        BuildButtonLayout(client);
        // Match D2DDialogSurface exactly: 14-DIP Segoe UI Variable Text uses
        // semibold for the primary action and normal weight for secondary ones.
        HFONT primaryButtonFont = CreateUiFont(dpi_, 14, FW_SEMIBOLD);
        HFONT secondaryButtonFont = CreateUiFont(dpi_, 14, FW_NORMAL);
        const HGDIOBJ oldButtonFont = SelectObject(dc, primaryButtonFont);
        for (std::size_t index = 0; index < buttonCount_; ++index) {
            const auto& button = buttonStates_[index];
            const bool primary = index == 0;
            SelectObject(dc, primary ? primaryButtonFont : secondaryButtonFont);
            std::uint32_t fill = primary ? palette.selection : palette.surface;
            if (static_cast<int>(index) == hotButton_ || static_cast<int>(index) == pressedButton_) {
                fill = primary ? Blend(palette.text, fill, 26) : Blend(palette.selection, fill, 25);
            }
            FillRounded(dc, button.bounds, radius, fill);
            if (!primary) StrokeRounded(dc, button.bounds, radius, std::max(1, Scale(1, dpi_)), palette.grid);
            if (index == focusedButton_) {
                RECT focus = button.bounds;
                InflateRect(&focus, -Scale(2, dpi_), -Scale(2, dpi_));
                StrokeRounded(dc, focus, std::max(2, radius - Scale(2, dpi_)),
                    std::max(1, Scale(1, dpi_)), primary ? palette.selectionText : palette.selection);
            }
            SetTextColor(dc, RgbToColorRef(primary ? palette.selectionText : palette.text));
            RECT labelRect = button.bounds;
            DrawTextW(dc, button.label, -1, &labelRect,
                DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }
        SelectObject(dc, oldButtonFont);
        DeleteObject(secondaryButtonFont);
        DeleteObject(primaryButtonFont);
        if (previousBackgroundMode) SetBkMode(dc, previousBackgroundMode);
    }

    void Paint() {
        PAINTSTRUCT paint{};
        HDC target = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        HDC buffer = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(1L, client.right), std::max(1L, client.bottom));
        const HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
        Draw(buffer, client);
        BitBlt(target, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(window_, &paint);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            RelayoutButtons();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            if (wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT) {
                shiftDown_ = true;
                return 0;
            }
            if (wParam == VK_TAB) {
                const bool reverse = shiftDown_ || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                focusedButton_ = reverse ?
                    (focusedButton_ + buttonCount_ - 1) % buttonCount_ :
                    (focusedButton_ + 1) % buttonCount_;
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_RIGHT || wParam == VK_DOWN) {
                focusedButton_ = (focusedButton_ + 1) % buttonCount_;
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_LEFT || wParam == VK_UP) {
                focusedButton_ = (focusedButton_ + buttonCount_ - 1) % buttonCount_;
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_RETURN || wParam == VK_SPACE) {
                ActivateButton(focusedButton_);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                Finish(CancelResult());
                return 0;
            }
            break;
        case WM_KEYUP:
            if (wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT) {
                shiftDown_ = false;
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            // A modifier key can be released after another window takes focus,
            // in which case this HWND receives no matching WM_KEYUP.
            shiftDown_ = false;
            return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
            TrackMouseEvent(&tracking);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int hit = HitTestButton(point);
            if (hit != hotButton_) {
                hotButton_ = hit;
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hotButton_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            pressedButton_ = HitTestButton(point);
            if (pressedButton_ >= 0) {
                focusedButton_ = static_cast<std::size_t>(pressedButton_);
                SetCapture(window_);
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const int pressed = pressedButton_;
            pressedButton_ = -1;
            if (GetCapture() == window_) ReleaseCapture();
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pressed >= 0 && HitTestButton(point) == pressed) ActivateButton(static_cast<std::size_t>(pressed));
            else InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_GETDPISCALEDSIZE: {
            // Tell Per-Monitor-V2 the exact outer size produced by remeasuring the
            // wrapped body at the destination DPI. Returning TRUE makes this size
            // the basis of the subsequent WM_DPICHANGED suggested rectangle.
            auto* scaledSize = reinterpret_cast<SIZE*>(lParam);
            if (!scaledSize) return FALSE;
            *scaledSize = MeasureWindowSize(static_cast<UINT>(wParam));
            return TRUE;
        }
        case WM_DPICHANGED: {
            const UINT newDpi = HIWORD(wParam) ? HIWORD(wParam) : LOWORD(wParam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (!suggested || newDpi == 0) return 0;
            dpi_ = newDpi;

            // Recompute instead of trusting an automatically scaled legacy size:
            // text wrapping and modal-frame metrics may differ at the new monitor.
            const SIZE desired = MeasureWindowSize(dpi_);
            RECT target{suggested->left, suggested->top,
                        suggested->left + desired.cx, suggested->top + desired.cy};
            target = ClampToMonitorWorkArea(target);
            if (GetCapture() == window_) ReleaseCapture();
            pressedButton_ = -1;
            hotButton_ = -1;
            SetWindowPos(window_, nullptr, target.left, target.top,
                target.right - target.left, target.bottom - target.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            RelayoutButtons();
            // Fonts are created from dpi_ during each paint, so invalidating the
            // client recreates body, icon, and button typography at the new scale.
            RedrawWindow(window_, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplyWindowTheme();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            Finish(CancelResult());
            return 0;
        case WM_DESTROY:
            window_ = nullptr;
            // A same-thread posted key naturally wakes GetMessage, but automation,
            // accessibility, and owner teardown can destroy the HWND through a
            // synchronous path. Wake the loop so it observes the null handle.
            PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    HWND owner_{};
    HINSTANCE instance_{};
    HWND window_{};
    std::wstring title_;
    std::wstring message_;
    MessageDialogButtons buttons_{};
    MessageDialogIcon icon_{};
    const Profile& profile_;
    UiLanguage language_{};
    UINT dpi_{kLogicalDpi};
    bool ownerWasEnabled_{};
    bool applyingTheme_{};
    bool shiftDown_{};
    MessageDialogResult result_{MessageDialogResult::None};
    std::array<ButtonState, 3> buttonStates_{};
    std::size_t buttonCount_{1};
    std::size_t focusedButton_{};
    int hotButton_{-1};
    int pressedButton_{-1};
};
}

MessageDialogResult ShowMessageDialog(HWND owner, HINSTANCE instance,
    std::wstring_view title, std::wstring_view message, MessageDialogButtons buttons,
    MessageDialogIcon icon, const Profile& profile, UiLanguage language) {
    GdiMessageDialog dialog(owner, instance, title, message, buttons, icon, profile, language);
    return dialog.Show();
}
