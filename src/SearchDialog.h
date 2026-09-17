#pragma once

// Search-dialog input/output contract. The modal function returns a copy so a
// canceled window cannot partially mutate the caller's previous search state.

#include "Document.h"
#include "Localization.h"
#include "Profile.h"

#include <windows.h>

#include <string>

struct SearchDialogResult {
    // accepted is false for every close/cancel path.
    bool accepted{};
    // true selects hexadecimal parsing; false uses the selected text encoding.
    bool binary{true};
    TextEncoding encoding{TextEncoding::Ascii};
    UiLanguage language{UiLanguage::Japanese};
    std::wstring query;
};

// Runs a nested modal loop and returns only after the custom HWND is destroyed.
// profile is borrowed so live system-theme/high-contrast changes can be resolved.
SearchDialogResult ShowSearchDialog(HWND owner, HINSTANCE instance, const SearchDialogResult& initial, const Profile& profile);
