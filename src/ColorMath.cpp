// Implements deterministic HSV/RGB conversion for the custom color picker.

#include "ColorMath.h"

#include <algorithm>
#include <cmath>

namespace {
double NormalizeHue(double hue) {
    hue = std::fmod(hue, 360.0);
    return hue < 0.0 ? hue + 360.0 : hue;
}

std::uint8_t Channel(double value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}
}

HsvColor RgbToHsv(std::uint32_t rgb) noexcept {
    const double red = ((rgb >> 16) & 0xFFu) / 255.0;
    const double green = ((rgb >> 8) & 0xFFu) / 255.0;
    const double blue = (rgb & 0xFFu) / 255.0;
    const double maximum = std::max({red, green, blue});
    const double minimum = std::min({red, green, blue});
    const double delta = maximum - minimum;

    HsvColor hsv;
    hsv.value = maximum;
    hsv.saturation = maximum == 0.0 ? 0.0 : delta / maximum;
    if (delta == 0.0) {
        hsv.hue = 0.0;
    } else if (maximum == red) {
        hsv.hue = 60.0 * std::fmod((green - blue) / delta, 6.0);
    } else if (maximum == green) {
        hsv.hue = 60.0 * (((blue - red) / delta) + 2.0);
    } else {
        hsv.hue = 60.0 * (((red - green) / delta) + 4.0);
    }
    hsv.hue = NormalizeHue(hsv.hue);
    return hsv;
}

std::uint32_t HsvToRgb(HsvColor hsv) noexcept {
    hsv.hue = NormalizeHue(hsv.hue);
    hsv.saturation = std::clamp(hsv.saturation, 0.0, 1.0);
    hsv.value = std::clamp(hsv.value, 0.0, 1.0);
    const double chroma = hsv.value * hsv.saturation;
    const double sector = hsv.hue / 60.0;
    const double intermediate = chroma * (1.0 - std::abs(std::fmod(sector, 2.0) - 1.0));
    const double match = hsv.value - chroma;

    double red{};
    double green{};
    double blue{};
    if (sector < 1.0) { red = chroma; green = intermediate; }
    else if (sector < 2.0) { red = intermediate; green = chroma; }
    else if (sector < 3.0) { green = chroma; blue = intermediate; }
    else if (sector < 4.0) { green = intermediate; blue = chroma; }
    else if (sector < 5.0) { red = intermediate; blue = chroma; }
    else { red = chroma; blue = intermediate; }

    return (static_cast<std::uint32_t>(Channel(red + match)) << 16) |
           (static_cast<std::uint32_t>(Channel(green + match)) << 8) |
           static_cast<std::uint32_t>(Channel(blue + match));
}

