// Implements theme synchronization for the complete classic Win32 menu tree.
// The top-level bar uses UAH draw messages, while USER32 popup-menu windows use
// the process/window immersive policy borrowed from the WinNative1 sample.
// Every undocumented entry point is guarded so unsupported systems retain the
// standard light or high-contrast rendering path.

#include "ThemeMenu.h"

#include <algorithm>
#include <vector>

namespace {
// Layouts below mirror the undocumented structures carried by UAH menu messages.
// They are isolated here so normal application code never relies on their ABI.
union UahMenuItemMetrics {
    struct { DWORD cx, cy; } bar[2];
    struct { DWORD cx, cy; } popup[4];
};

struct UahMenuPopupMetrics {
    DWORD widths[4];
    DWORD updateMaxWidths : 2;
};

struct UahMenu {
    HMENU menu;
    HDC dc;
    DWORD flags;
};

struct UahMenuItem {
    int position;
    UahMenuItemMetrics metrics;
    UahMenuPopupMetrics popupMetrics;
};

struct UahDrawMenuItem {
    DRAWITEMSTRUCT draw;
    UahMenu menu;
    UahMenuItem item;
};

enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };
// Ordinal exports are undocumented and may be absent. Every call site checks for
// null and retains the normal USER32 menu path as a safe fallback.
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using RefreshImmersiveColorPolicyStateFn = void(WINAPI*)();
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
using FlushMenuThemesFn = void(WINAPI*)();

COLORREF ToColorRef(std::uint32_t rgb) { return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); }

std::uint32_t Mix(std::uint32_t a, std::uint32_t b, unsigned bAmount) {
    const unsigned aAmount = 255 - bAmount;
    const auto channel = [=](unsigned shift) {
        return ((((a >> shift) & 0xff) * aAmount + ((b >> shift) & 0xff) * bAmount) / 255) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}

void Fill(HDC dc, const RECT& rect, std::uint32_t color) {
    // UAH supplies an HDC, so this narrow bridge uses GDI and releases the brush
    // immediately after each fill to avoid per-menu-open resource leaks.
    HBRUSH brush = CreateSolidBrush(ToColorRef(color));
    if (brush) { FillRect(dc, &rect, brush); DeleteObject(brush); }
}

HFONT CreateMenuFont(HWND window) {
    // Query the DPI-specific non-client font so custom text metrics match native
    // menus on the current monitor.
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    const UINT dpi = GetDpiForWindow(window);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    return CreateFontIndirectW(&metrics.lfMenuFont);
}
}

void ThemeMenu::ApplyPreferredAppMode(HWND window, bool dark, bool highContrast) {
    const auto desired = highContrast ? PreferredAppMode::Default : (dark ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
    // LOAD_LIBRARY_SEARCH_SYSTEM32 prevents DLL search-order hijacking.
    HMODULE uxTheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxTheme) return;
    const auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxTheme, MAKEINTRESOURCEA(135)));
    const auto refresh = reinterpret_cast<RefreshImmersiveColorPolicyStateFn>(GetProcAddress(uxTheme, MAKEINTRESOURCEA(104)));
    const auto allowWindow = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(uxTheme, MAKEINTRESOURCEA(133)));
    const auto flushMenus = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxTheme, MAKEINTRESOURCEA(136)));

    // WinNative1's preferred-app-mode technique controls the native popup menu
    // window created by USER32. Apply it from BinEdit's resolved preference,
    // rather than consulting AppsUseLightTheme independently, so explicit Light
    // and Dark selections affect every drop-down item as well as the UAH bar.
    if (setMode) setMode(desired);
    if (refresh) refresh();
    if (allowWindow && window) allowWindow(window, dark && !highContrast);
    // MENU theme handles are cached by USER32/uxtheme. Flushing is essential for
    // an already-running process and for a newly rebuilt localized HMENU tree.
    if (flushMenus) flushMenus();
    FreeLibrary(uxTheme);
}

bool ThemeMenu::HandleDrawMessage(HWND window, UINT message, LPARAM lParam, const Palette& palette, LRESULT& result) {
    // High contrast owns every menu color. Light mode is already drawn correctly by USER32.
    if (palette.highContrast || !palette.dark) return false;
    // WM_UAHDRAWMENU covers the bar background; individual labels arrive through
    // WM_UAHDRAWMENUITEM below.
    if (message == DrawMenuMessage) {
        auto* menu = reinterpret_cast<UahMenu*>(lParam);
        if (!menu || !menu->dc) return false;
        MENUBARINFO info{sizeof(info)};
        if (!GetMenuBarInfo(window, OBJID_MENU, 0, &info)) return false;
        RECT windowRect{}; GetWindowRect(window, &windowRect);
        OffsetRect(&info.rcBar, -windowRect.left, -windowRect.top);
        Fill(menu->dc, info.rcBar, palette.surface);
        result = 0;
        return true;
    }
    if (message != DrawMenuItemMessage) return false;

    auto* item = reinterpret_cast<UahDrawMenuItem*>(lParam);
    if (!item || !item->menu.dc || !item->menu.menu) return false;
    MENUITEMINFOW info{sizeof(info)};
    info.fMask = MIIM_STRING;
    GetMenuItemInfoW(item->menu.menu, item->item.position, TRUE, &info);
    std::vector<wchar_t> text(static_cast<std::size_t>(info.cch) + 2, L'\0');
    info.dwTypeData = text.data();
    info.cch = static_cast<UINT>(text.size());
    if (!GetMenuItemInfoW(item->menu.menu, item->item.position, TRUE, &info)) return false;

    // Derive background and foreground from standard ODS state flags so mouse and
    // keyboard activation remain visually equivalent.
    const UINT state = item->draw.itemState;
    std::uint32_t background = palette.surface;
    if (state & ODS_SELECTED) background = Mix(palette.surface, palette.selection, 150);
    else if (state & ODS_HOTLIGHT) background = Mix(palette.surface, palette.selection, 90);
    Fill(item->menu.dc, item->draw.rcItem, background);

    const int oldMode = SetBkMode(item->menu.dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(item->menu.dc, ToColorRef((state & (ODS_GRAYED | ODS_DISABLED)) ? palette.mutedText : palette.text));
    HFONT font = CreateMenuFont(window);
    HGDIOBJ oldFont = font ? SelectObject(item->menu.dc, font) : nullptr;
    UINT flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
    if (state & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
    DrawTextW(item->menu.dc, text.data(), static_cast<int>(info.cch), &item->draw.rcItem, flags);
    if (oldFont) SelectObject(item->menu.dc, oldFont);
    if (font) DeleteObject(font);
    SetTextColor(item->menu.dc, oldColor);
    SetBkMode(item->menu.dc, oldMode);
    result = 0;
    return true;
}

void ThemeMenu::DrawMenuBorder(HWND window, const Palette& palette) {
    if (palette.highContrast || !palette.dark) return;
    RECT client{}; GetClientRect(window, &client);
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
    RECT windowRect{}; GetWindowRect(window, &windowRect);
    OffsetRect(&client, -windowRect.left, -windowRect.top);
    // The seam is in non-client coordinates, requiring GetWindowDC rather than a
    // normal BeginPaint client HDC.
    HDC dc = GetWindowDC(window);
    if (!dc) return;
    HPEN pen = CreatePen(PS_SOLID, 1, ToColorRef(palette.grid));
    HGDIOBJ oldPen = pen ? SelectObject(dc, pen) : nullptr;
    MoveToEx(dc, client.left, client.top - 1, nullptr);
    LineTo(dc, client.right, client.top - 1);
    if (oldPen) SelectObject(dc, oldPen);
    if (pen) DeleteObject(pen);
    ReleaseDC(window, dc);
}
