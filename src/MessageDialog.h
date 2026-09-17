#pragma once

// Declares BinEdit's application-owned message dialog. The implementation uses
// only USER32, GDI, and DWM APIs so error and confirmation UI remains available
// even when Direct3D or Direct2D initialization has failed.

#include "Localization.h"
#include "Profile.h"

#include <windows.h>

#include <string_view>

// Button groups are intentionally limited to the combinations used by BinEdit.
// Keeping the surface small makes the caller's destructive-action policy clear.
enum class MessageDialogButtons { Ok, YesNo, YesNoCancel, ReloadContinue };

// Icons are rendered from theme-aware, supersampled GDI paths rather than font
// glyphs or bitmap assets whose metrics/colors could conflict with DPI and
// high-contrast mode.
enum class MessageDialogIcon { Information, Warning, Error, Question };

// Strongly typed results prevent accidental comparisons with unrelated Win32
// command identifiers at save/elevation call sites.
enum class MessageDialogResult { None, Ok, Yes, No, Cancel };

// Displays a synchronous owner-modal dialog. profile and all string views are
// borrowed only until this function returns. No D3D11/D2D1 object is created.
// The window remeasures and relayouts itself for every Per-Monitor-V2 DPI change.
MessageDialogResult ShowMessageDialog(
    HWND owner,
    HINSTANCE instance,
    std::wstring_view title,
    std::wstring_view message,
    MessageDialogButtons buttons,
    MessageDialogIcon icon,
    const Profile& profile,
    UiLanguage language);
