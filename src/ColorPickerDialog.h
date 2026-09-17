#pragma once

// Declares BinEdit's application-owned color picker. The implementation uses the
// shared D3D11/D2D1 dialog surface and never invokes the CHOOSECOLOR common dialog.

#include "Localization.h"
#include "Profile.h"

#include <windows.h>

#include <cstdint>
#include <optional>

// Returns the selected 0xRRGGBB color, or nullopt when the user cancels or the
// GPU dialog cannot be initialized. profile is borrowed until the modal call ends.
std::optional<std::uint32_t> ShowColorPickerDialog(
    HWND owner,
    HINSTANCE instance,
    std::uint32_t initialColor,
    UiLanguage language,
    const Profile& profile);

