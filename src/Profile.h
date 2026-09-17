#pragma once

// User preferences and resolved theme colors.
// Persisted colors use the platform-independent 0xRRGGBB representation;
// conversion to COLORREF or D2D color values happens only at API boundaries.

#include <windows.h>

#include "Document.h"
#include "Localization.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>

// Persisted selection. System is resolved on demand, so an open application can
// react immediately when Windows changes between light and dark modes.
enum class ThemePreference { System, Light, Dark };

// Fully resolved colors used by every renderer. Each integer is 0xRRGGBB.
// highContrast also acts as a policy flag that disables user byte colors where
// those colors could violate the operating-system accessibility palette.
struct Palette {
    std::uint32_t background{};
    std::uint32_t surface{};
    std::uint32_t text{};
    std::uint32_t mutedText{};
    std::uint32_t grid{};
    std::uint32_t selection{};
    std::uint32_t selectionText{};
    std::uint32_t search{};
    std::uint32_t caret{};
    bool highContrast{};
    bool dark{};
};

class Profile {
public:
    // Seeds accessible defaults before optional JSON values are applied.
    Profile();
    // Missing, unknown, or malformed properties retain their current defaults.
    void Load();
    // Writes deterministic, human-editable JSON under LocalAppData.
    void Save() const;
    // Combines saved preference, Windows theme/accent state, and high-contrast
    // colors. The accent is never applied while high contrast is active.
    [[nodiscard]] Palette ResolvePalette() const;
    // Returns %LOCALAPPDATA%/BinEdit/profile.json without creating the file.
    [[nodiscard]] std::filesystem::path FilePath() const;

    ThemePreference theme{ThemePreference::System};
    TextEncoding encoding{TextEncoding::Ascii};
    LanguagePreference language{LanguagePreference::System};
    std::uint32_t markupColor{0x5B8DEF};
    std::uint32_t searchColor{0xD69E2E};
    // Optional color for each possible byte value. An array gives constant-time
    // renderer lookup and serializes only entries explicitly configured.
    std::array<std::optional<std::uint32_t>, 256> byteColors{};
};

// Explicit boundary conversions between 0xRRGGBB and Win32's 0x00BBGGRR.
std::uint32_t ColorRefToRgb(COLORREF color);
COLORREF RgbToColorRef(std::uint32_t color);
