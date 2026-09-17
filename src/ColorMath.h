#pragma once

// Defines locale- and UI-independent color conversion used by the custom color
// picker. RGB values use BinEdit's platform-neutral 0xRRGGBB representation.

#include <cstdint>

struct HsvColor {
    // Hue is normalized to [0, 360); saturation and value are clamped to [0, 1].
    double hue{};
    double saturation{};
    double value{};
};

// Converts between the persisted RGB representation and the picker's HSV model.
// Conversion rounds to the nearest 8-bit channel for stable RGB -> HSV -> RGB
// round trips.
[[nodiscard]] HsvColor RgbToHsv(std::uint32_t rgb) noexcept;
[[nodiscard]] std::uint32_t HsvToRgb(HsvColor hsv) noexcept;

