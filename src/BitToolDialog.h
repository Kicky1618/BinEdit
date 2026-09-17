#pragma once

// Declares the modal D3D11/D2D1 bit-operation tool. The tool owns its input and
// operand values and performs calculations without reading or changing a
// document, selection, or editor caret.

#include "BitOperations.h"
#include "Localization.h"
#include "Profile.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

struct BitToolDialogResult {
    BitOperation operation{BitOperation::Xor};
    std::uint8_t input{};
    std::uint8_t operand{0xFF};
};

// Runs a synchronous owner-modal calculator and returns the most recent valid
// values so the next invocation can restore them.
BitToolDialogResult ShowBitToolDialog(HWND owner, HINSTANCE instance,
                                      const BitToolDialogResult& initial,
                                      UiLanguage language,
                                      const Profile& profile);
