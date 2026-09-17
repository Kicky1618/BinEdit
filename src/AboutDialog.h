#pragma once

// Public entry point for the themed About window. The implementation disables
// the owner for modal behavior and restores it before returning.

#include "Localization.h"
#include "Profile.h"

#include <windows.h>

// Displays a keyboard-accessible D3D modal window. profile is borrowed during the
// call to resolve live theme/high-contrast changes and is never modified.
void ShowAboutDialog(HWND owner, HINSTANCE instance, UiLanguage language, const Profile& profile);
