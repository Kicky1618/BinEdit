#pragma once

// Narrow interface around themed non-client menu rendering. Keeping UAH types
// private to ThemeMenu.cpp prevents undocumented structures from leaking into
// the rest of the application.

#include "Profile.h"

#include <windows.h>

namespace ThemeMenu {
constexpr UINT DrawMenuMessage = 0x0091;      // WM_UAHDRAWMENU
constexpr UINT DrawMenuItemMessage = 0x0092;  // WM_UAHDRAWMENUITEM

// Synchronizes native popup-menu items with the resolved BinEdit palette. The
// implementation selects the process policy, opts this HWND into or out of dark
// mode, and flushes cached MENU theme handles so the next popup uses the new
// setting immediately. Missing undocumented exports degrade to native drawing.
void ApplyPreferredAppMode(HWND window, bool dark, bool highContrast);
// Returns true only when a dark UAH message was recognized and fully handled.
bool HandleDrawMessage(HWND window, UINT message, LPARAM lParam, const Palette& palette, LRESULT& result);
// Repairs the one-pixel non-client seam below a custom dark menu bar.
void DrawMenuBorder(HWND window, const Palette& palette);
}
