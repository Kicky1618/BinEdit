// Implements the modal, fully GPU-rendered About window.
// The client area contains no HWND child controls: D3D11 owns presentation,
// while Direct2D and DirectWrite draw every visible control and label.
// All methods run on the main UI thread inside a nested modal message loop.

#include "AboutDialog.h"

#include "DialogSurface.h"
#include "resource.h"
#include "Security.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kAboutWindowClass[] = L"BinEdit.AboutDialog";
// Coalesces paint, hover, resize, and theme updates behind one GPU frame.
constexpr UINT WM_ABOUT_RENDER = WM_APP + 43;

// Standard C++ compilation macros are narrow string literals. Two-stage token
// expansion converts their actual values, rather than their macro names, into
// the UTF-16 literals used by the Win32/DirectWrite UI.
#define BINEDIT_WIDEN_BUILD_VALUE_IMPL(value) L##value
#define BINEDIT_WIDEN_BUILD_VALUE(value) BINEDIT_WIDEN_BUILD_VALUE_IMPL(value)
constexpr wchar_t kBuildDateTime[] = BINEDIT_WIDEN_BUILD_VALUE(__DATE__)
    L" " BINEDIT_WIDEN_BUILD_VALUE(__TIME__);
constexpr wchar_t kBuildTimeZone[] = L"UTC+9 (JST)";
#undef BINEDIT_WIDEN_BUILD_VALUE
#undef BINEDIT_WIDEN_BUILD_VALUE_IMPL

bool Contains(D2D1_RECT_F rect, float x, float y) { return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom; }

std::uint32_t Mix(std::uint32_t a, std::uint32_t b, unsigned bAmount) {
    const unsigned aAmount = 255 - bAmount;
    const auto channel = [=](unsigned shift) { return ((((a >> shift) & 0xff) * aAmount + ((b >> shift) & 0xff) * bAmount) / 255) << shift; };
    return channel(16) | channel(8) | channel(0);
}

struct VersionMetadata {
    std::wstring version{L"1.0.0.0"};
    std::wstring copyright{L"Copyright (c) 2026 rk0exn / n0xa All rights reserved."};
};

VersionMetadata ReadVersionMetadata(HINSTANCE instance) noexcept {
    VersionMetadata metadata;
    try {
        // Long-path capacity is intentionally heap-backed. Keeping a 64-KiB
        // path buffer in this helper would consume most of the default stack
        // budget before version-resource parsing and exception machinery run.
        std::vector<wchar_t> modulePath(32768, L'\0');
        const DWORD pathLength = GetModuleFileNameW(instance, modulePath.data(),
            static_cast<DWORD>(modulePath.size()));
        if (pathLength == 0 || pathLength >= modulePath.size()) return metadata;
        DWORD ignored{};
        const DWORD size = GetFileVersionInfoSizeW(modulePath.data(), &ignored);
        if (size == 0 || size > 1024u * 1024u) return metadata;
        std::vector<std::uint8_t> bytes(size);
        if (!GetFileVersionInfoW(modulePath.data(), 0, size, bytes.data())) return metadata;

        VS_FIXEDFILEINFO* fixed{};
        UINT fixedSize{};
        if (VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedSize) &&
            fixed && fixedSize >= sizeof(VS_FIXEDFILEINFO) && fixed->dwSignature == 0xFEEF04BD) {
            std::array<wchar_t, 48> formatted{};
            swprintf_s(formatted.data(), formatted.size(), L"%u.%u.%u.%u",
                HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
                HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
            metadata.version = formatted.data();
        }

        struct Translation { WORD language; WORD codePage; };
        Translation* translations{};
        UINT translationBytes{};
        if (VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation",
            reinterpret_cast<void**>(&translations), &translationBytes) && translations &&
            translationBytes >= sizeof(Translation)) {
            std::array<wchar_t, 96> query{};
            swprintf_s(query.data(), query.size(), L"\\StringFileInfo\\%04x%04x\\LegalCopyright",
                translations[0].language, translations[0].codePage);
            wchar_t* value{};
            UINT characters{};
            if (VerQueryValueW(bytes.data(), query.data(), reinterpret_cast<void**>(&value),
                &characters) && value && characters > 1) {
                metadata.copyright.assign(value, characters - 1);
            }
        }
    } catch (...) {
        // The immutable fallbacks match the checked-in version resource. About
        // presentation must remain available under allocation pressure.
    }
    return metadata;
}

class AboutWindow {
public:
    AboutWindow(HWND owner, HINSTANCE instance, UiLanguage language, const Profile& profile)
        : owner_(owner), instance_(instance), strings_(&GetStrings(language)), profile_(&profile), palette_(profile.ResolvePalette()) {
        const VersionMetadata metadata = ReadVersionMetadata(instance_);
        versionLine_ = std::wstring(strings_->aboutVersionLabel) + L": " + metadata.version;
        std::array<wchar_t, 128> build{};
#ifdef _DEBUG
        constexpr wchar_t configuration[] = L"Debug";
#else
        constexpr wchar_t configuration[] = L"Release";
#endif
        swprintf_s(build.data(), build.size(), L"%s: %s x64 | MSVC %u.%02u.%05u | v145",
            strings_->aboutBuildLabel, configuration, _MSC_VER / 100, _MSC_VER % 100,
            _MSC_FULL_VER % 100000);
        buildLine_ = build.data();
        builtLine_ = std::wstring(strings_->aboutBuildDateLabel) + L": " +
            kBuildDateTime + L" " + kBuildTimeZone;
        copyrightLine_ = metadata.copyright;
    }

    ~AboutWindow() {
        // LoadImage without LR_SHARED transfers icon ownership to this dialog.
        if (icon_) DestroyIcon(icon_);
    }

    void Show() {
        // Use the owner's DPI and monitor before creation to avoid an initial
        // bitmap-scaled frame or a visible position jump.
        Register();
        const UINT dpi = owner_ ? GetDpiForWindow(owner_) : 96;
        ReloadIcon(dpi);
        const float scale = dpi / 96.0f;
        RECT rect{0, 0, static_cast<LONG>(520 * scale), static_cast<LONG>(300 * scale)};
        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
        const int width = rect.right - rect.left, height = rect.bottom - rect.top;
        const DialogPlacement placement = ResolveDialogPlacement(owner_);
        const int x = std::clamp(placement.anchor.left +
            ((placement.anchor.right - placement.anchor.left) - width) / 2,
            placement.workArea.left, std::max(placement.workArea.left, placement.workArea.right - width));
        const int y = std::clamp(placement.anchor.top +
            ((placement.anchor.bottom - placement.anchor.top) - height) / 2,
            placement.workArea.top, std::max(placement.workArea.top, placement.workArea.bottom - height));
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kAboutWindowClass, strings_->aboutTitle, style,
            x, y, width, height, owner_, nullptr, instance_, this);
        if (!window_) return;
        if (!surface_.Initialize(window_)) { DestroyWindow(window_); return; }
        ready_ = true;
        RefreshTheme();
        // Explicit owner disabling provides true modality without a dialog resource
        // or native child controls.
        EnableWindow(owner_, FALSE);
        ShowWindow(window_, IsWindowVisible(owner_) ? SW_SHOW : SW_HIDE);
        SetForegroundWindow(window_); SetFocus(window_); RequestDraw();
        MSG message{};
        BOOL messageStatus = 1;
        while (running_) {
            messageStatus = GetMessageW(&message, nullptr, 0, 0);
            if (messageStatus <= 0) break;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (window_) DestroyWindow(window_);
        if (messageStatus == 0) PostQuitMessage(static_cast<int>(message.wParam));
        if (owner_ && IsWindow(owner_)) { EnableWindow(owner_, TRUE); SetActiveWindow(owner_); }
    }

private:
    void Register() const {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WindowProc; wc.hInstance = instance_; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kAboutWindowClass; wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        try {
            // The stack controller remains alive for the complete nested loop.
            auto* self = reinterpret_cast<AboutWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE) {
                self = static_cast<AboutWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
                self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
        } catch (...) {
            Security::FailFast(L"AboutWindow::WindowProc");
        }
    }

    D2D1_RECT_F ButtonRect() const {
        const float s = surface_.Scale(), w = surface_.Width(), h = surface_.Height();
        return {w - 144 * s, h - 64 * s, w - 24 * s, h - 24 * s};
    }

    void ReloadIcon(UINT dpi) {
        if (icon_) {
            DestroyIcon(icon_);
            icon_ = nullptr;
        }
        const int size = std::max(1, MulDiv(56, static_cast<int>(dpi), 96));
        icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_ICON1),
            IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    }

    // Avoid duplicate frames when multiple Win32 messages invalidate the same UI.
    void RequestDraw() { if (ready_ && !renderQueued_) renderQueued_ = PostMessageW(window_, WM_ABOUT_RENDER, 0, 0) != FALSE; }

    void RefreshTheme() {
        // Re-resolve instead of reusing the launch palette so a live high-contrast
        // switch is reflected without closing this modal window.
        if (applyingTheme_) return;
        applyingTheme_ = true;
        palette_ = profile_->ResolvePalette();
        BOOL dark = palette_.dark && !palette_.highContrast;
        constexpr DWORD immersiveDarkMode = 20;
        DwmSetWindowAttribute(window_, immersiveDarkMode, &dark, sizeof(dark));
        SetWindowTheme(window_, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        applyingTheme_ = false;
        RequestDraw();
    }

    void Draw() {
        // Every visible pixel in the client area is emitted through D2D.
        renderQueued_ = false;
        if (!surface_.Begin(palette_)) return;
        const float s = surface_.Scale(), w = surface_.Width(), h = surface_.Height();
        // A single elevated content surface and right-aligned primary action
        // mirror WinUI ContentDialog composition without introducing nested cards.
        const auto card = D2D1::RectF(20 * s, 20 * s, w - 20 * s, h - 84 * s);
        surface_.FillRounded(card, 8 * s, palette_.surface);
        surface_.StrokeRounded(card, 8 * s, palette_.grid, std::max(1.0f, s));

        // Use the same IDI_ICON1 resource as the executable and top-level windows
        // so the About surface presents the actual application identity.
        const auto mark = D2D1::RectF(40 * s, 42 * s, 96 * s, 98 * s);
        surface_.DrawIcon(icon_, mark);
        surface_.TitleText(L"BinEdit", {116 * s, 38 * s, w - 40 * s, 82 * s}, palette_.text);
        surface_.Text(strings_->aboutBody, {116 * s, 78 * s, w - 40 * s, 104 * s}, palette_.mutedText);
        surface_.Text(versionLine_, {116 * s, 104 * s, w - 40 * s, 130 * s}, palette_.mutedText);
        surface_.Text(buildLine_, {116 * s, 130 * s, w - 40 * s, 156 * s}, palette_.mutedText);
        surface_.Text(builtLine_, {116 * s, 156 * s, w - 40 * s, 182 * s}, palette_.mutedText);
        surface_.CaptionText(copyrightLine_, {40 * s, 188 * s, w - 40 * s, 210 * s}, palette_.mutedText);

        const auto button = ButtonRect();
        const std::uint32_t background = hovered_
            ? Mix(palette_.selection, palette_.selectionText, palette_.dark ? 22 : 14)
            : palette_.selection;
        surface_.FillRounded(button, 4 * s, background);
        surface_.StrokeRounded(button, 4 * s, palette_.selectionText, 2.0f * s);
        surface_.Text(L"OK", button, palette_.selectionText, true, true);
        if (!surface_.End()) RequestDraw();
    }

    void Close() const { if (window_) DestroyWindow(window_); }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(window_, &paint); EndPaint(window_, &paint); RequestDraw(); return 0; }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: if (ready_) { surface_.Resize(LOWORD(lParam), HIWORD(lParam)); RequestDraw(); } return 0;
        case WM_DPICHANGED: {
            const UINT dpi = HIWORD(wParam);
            surface_.DpiChanged(dpi); ReloadIcon(dpi);
            const RECT* rect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            RequestDraw(); return 0;
        }
        case WM_ABOUT_RENDER: Draw(); return 0;
        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED: RefreshTheme(); return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track);
            const bool hovered = Contains(ButtonRect(), static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
            if (hovered != hovered_) { hovered_ = hovered; RequestDraw(); }
            return 0;
        }
        case WM_MOUSELEAVE: hovered_ = false; RequestDraw(); return 0;
        case WM_LBUTTONDOWN:
            if (Contains(ButtonRect(), static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)))) Close();
            return 0;
        case WM_KEYDOWN: if (wParam == VK_ESCAPE || wParam == VK_RETURN || wParam == VK_SPACE) { Close(); return 0; } break;
        case WM_CLOSE: Close(); return 0;
        case WM_DESTROY: running_ = false; window_ = nullptr; return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    HWND owner_{}; HINSTANCE instance_{}; HWND window_{}; HICON icon_{};
    const UiStrings* strings_{}; const Profile* profile_{}; Palette palette_{}; D2DDialogSurface surface_;
    std::wstring versionLine_; std::wstring buildLine_; std::wstring builtLine_; std::wstring copyrightLine_;
    bool running_{true}; bool ready_{}; bool renderQueued_{}; bool hovered_{}; bool applyingTheme_{};
};
}

void ShowAboutDialog(HWND owner, HINSTANCE instance, UiLanguage language, const Profile& profile) {
    // Synchronous API intentionally matches MessageBox-style call sites.
    AboutWindow window(owner, instance, language, profile);
    window.Show();
}
