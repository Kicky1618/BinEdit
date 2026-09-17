// Implements profile loading, deterministic JSON serialization, system theme
// detection, and conversion from persisted RGB values to runtime palettes.
// The parser deliberately accepts missing keys so older profiles remain valid.

#include "Profile.h"

#include <windows.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace {
constexpr std::uintmax_t kMaximumProfileBytes = 1024u * 1024u;

std::wstring RandomProfileSuffix() {
    std::array<std::uint8_t, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring suffix(random.size() * 2, L'0');
    for (std::size_t index = 0; index < random.size(); ++index) {
        suffix[index * 2] = digits[random[index] >> 4];
        suffix[index * 2 + 1] = digits[random[index] & 0x0fu];
    }
    return suffix;
}

bool WriteProfileAtomically(const std::filesystem::path& destination,
    std::string_view contents) noexcept {
    std::filesystem::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    try {
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            const std::wstring suffix = RandomProfileSuffix();
            if (suffix.empty()) return false;
            temporary = std::filesystem::path(destination.wstring() + L"." + suffix + L".tmp");
            file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return false;
        }
        if (file == INVALID_HANDLE_VALUE) return false;
        std::size_t writtenTotal{};
        while (writtenTotal < contents.size()) {
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
                contents.size() - writtenTotal, 1024u * 1024u));
            DWORD written{};
            if (!WriteFile(file, contents.data() + writtenTotal, requested, &written, nullptr) ||
                written != requested) {
                CloseHandle(file); file = INVALID_HANDLE_VALUE;
                DeleteFileW(temporary.c_str()); return false;
            }
            writtenTotal += written;
        }
        const bool flushed = FlushFileBuffers(file) != FALSE;
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        if (!flushed) { DeleteFileW(temporary.c_str()); return false; }
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str()); return false;
        }
        return true;
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        return false;
    }
}

// Resolves the profile directory through the Known Folder API rather than
// concatenating environment variables that may be absent or redirected.
std::filesystem::path ProfileDirectory() {
#if defined(BINEDIT_EXPLOIT_TEST)
    // Exploit tests compile the production parser but redirect persistence into
    // a per-run sandbox. Production binaries never contain this override.
    std::array<wchar_t, 32768> testRoot{};
    const DWORD testLength = GetEnvironmentVariableW(
        L"BINEDIT_EXPLOIT_TEST_LOCALAPPDATA", testRoot.data(),
        static_cast<DWORD>(testRoot.size()));
    if (testLength > 0 && testLength < testRoot.size())
        return std::filesystem::path(testRoot.data()) / L"BinEdit";
#endif
    PWSTR path = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path))) {
        result = std::filesystem::path(path) / L"BinEdit";
        CoTaskMemFree(path);
    }
    return result;
}

std::optional<std::string> JsonString(const std::string& json, const std::string& key) {
    // The profile schema contains only flat strings plus a byteColors object.
    // This intentionally small reader tolerates unknown properties and avoids a
    // third-party JSON dependency in the native executable.
    const std::string needle = "\"" + key + "\"";
    auto at = json.find(needle);
    if (at == std::string::npos) return std::nullopt;
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return std::nullopt;
    at = json.find('"', at + 1);
    if (at == std::string::npos) return std::nullopt;
    auto end = json.find('"', at + 1);
    if (end == std::string::npos) return std::nullopt;
    return json.substr(at + 1, end - at - 1);
}

std::optional<std::uint32_t> ParseColor(const std::string& value) {
    // Colors are exactly #RRGGBB; partial parses and alpha values are rejected.
    if (value.size() != 7 || value[0] != '#') return std::nullopt;
    std::uint32_t parsed{};
    auto [ptr, ec] = std::from_chars(value.data() + 1, value.data() + value.size(), parsed, 16);
    if (ec != std::errc{} || ptr != value.data() + value.size()) return std::nullopt;
    return parsed;
}

std::string ColorString(std::uint32_t color) {
    std::ostringstream out;
    out << '#' << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << (color & 0xffffffu);
    return out.str();
}

bool SystemUsesDarkMode() {
    // AppsUseLightTheme is read each time a palette is resolved so a settings
    // broadcast can update an already-running system-themed instance.
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return false;
    return value == 0;
}

std::uint32_t SystemColor(int index) { return ColorRefToRgb(GetSysColor(index)); }

std::uint32_t WindowsAccentColor() {
    // UISettings exposes the accent selected by the current Windows user. This
    // is intentionally different from DWM's optional title-bar/glass tint,
    // which can diverge when accent display on window chrome is disabled.
    try {
        // Keep the WinRT object scoped to this call so it is released before
        // wWinMain balances RoInitialize with RoUninitialize. A function-static
        // wrapper would run its destructor during CRT shutdown, after the COM
        // apartment has already gone away, and can crash a normal process exit.
        const winrt::Windows::UI::ViewManagement::UISettings settings;
        const auto color = settings.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Accent);
        return (static_cast<std::uint32_t>(color.R) << 16) |
               (static_cast<std::uint32_t>(color.G) << 8) |
               static_cast<std::uint32_t>(color.B);
    } catch (...) {
        // A supported interactive Windows session normally never reaches this
        // path. Keep startup resilient if WinRT activation is unavailable.
    }

    // The DWM value is only a compatibility fallback and is not treated as the
    // authoritative user accent. Alpha describes glass blending and is dropped.
    DWORD color{};
    BOOL opaqueBlend{};
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaqueBlend))) return color & 0x00ffffffu;
    // Keep the palette usable on systems where desktop composition cannot
    // provide a color (for example, some remote or restricted sessions).
    return 0x0078D4;
}

double RelativeLuminance(std::uint32_t color) {
    const auto channel = [color](unsigned shift) {
        const double srgb = static_cast<double>((color >> shift) & 0xffu) / 255.0;
        return srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(16) + 0.7152 * channel(8) + 0.0722 * channel(0);
}

std::uint32_t ContrastingTextColor(std::uint32_t background) {
    // Pick the better of black and white using WCAG relative contrast. This is
    // needed because users can choose accents ranging from very pale to black.
    const double luminance = RelativeLuminance(background);
    const double whiteContrast = 1.05 / (luminance + 0.05);
    const double blackContrast = (luminance + 0.05) / 0.05;
    return whiteContrast >= blackContrast ? 0xFFFFFF : 0x000000;
}
}

Profile::Profile() {
    // Conservative defaults make structural bytes visible without requiring a
    // profile. High contrast suppresses these colors at render time.
    byteColors[0x00] = 0x59636F;
    byteColors[0xFF] = 0xB85C5C;
}

std::filesystem::path Profile::FilePath() const { return ProfileDirectory() / L"profile.json"; }

void Profile::Load() {
    const auto path = FilePath();
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size > kMaximumProfileBytes) return;
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    std::string json;
    try {
        json.resize(static_cast<std::size_t>(size));
    } catch (...) { return; }
    if (!json.empty()) {
        input.read(json.data(), static_cast<std::streamsize>(json.size()));
        if (!input || static_cast<std::size_t>(input.gcount()) != json.size()) return;
    }
    try {
    if (auto value = JsonString(json, "theme")) {
        if (*value == "light") theme = ThemePreference::Light;
        else if (*value == "dark") theme = ThemePreference::Dark;
        else theme = ThemePreference::System;
    }
    if (auto value = JsonString(json, "encoding")) {
        if (*value == "utf8") encoding = TextEncoding::Utf8;
        else if (*value == "utf16le") encoding = TextEncoding::Utf16Le;
        else if (*value == "shiftjis") encoding = TextEncoding::ShiftJis;
        else encoding = TextEncoding::Ascii;
    }
    if (auto value = JsonString(json, "language")) {
        if (*value == "ja") language = LanguagePreference::Japanese;
        else if (*value == "en") language = LanguagePreference::English;
        else language = LanguagePreference::System;
    }
    if (auto value = JsonString(json, "markupColor")) if (auto color = ParseColor(*value)) markupColor = *color;
    if (auto value = JsonString(json, "searchColor")) if (auto color = ParseColor(*value)) searchColor = *color;

    // Search only after the byteColors key so two-character strings elsewhere in
    // the document cannot be mistaken for a byte-color property.
    const auto colorsAt = json.find("\"byteColors\"");
    if (colorsAt != std::string::npos) {
        for (unsigned value = 0; value < 256; ++value) {
            char key[3]{};
            static constexpr char digits[] = "0123456789ABCDEF";
            key[0] = digits[value >> 4]; key[1] = digits[value & 15];
            if (auto text = JsonString(json.substr(colorsAt), key)) byteColors[value] = ParseColor(*text);
        }
    }
    } catch (...) {
        // A memory-pressure failure leaves the already-initialized defaults in
        // place and never propagates through application startup.
        return;
    }
}

void Profile::Save() const {
    const auto directory = ProfileDirectory();
    std::error_code ec;
    // Directory creation failure is reflected by the following stream open; UI
    // preferences are non-critical, so callers do not interrupt editing for it.
    std::filesystem::create_directories(directory, ec);
    if (directory.empty()) return;
    try {
    std::ostringstream output;
    const char* themeName = theme == ThemePreference::Dark ? "dark" : (theme == ThemePreference::Light ? "light" : "system");
    const char* encodingName = encoding == TextEncoding::Utf8 ? "utf8" :
                               (encoding == TextEncoding::Utf16Le ? "utf16le" :
                               (encoding == TextEncoding::ShiftJis ? "shiftjis" : "ascii"));
    const char* languageName = language == LanguagePreference::Japanese ? "ja" :
                               (language == LanguagePreference::English ? "en" : "system");
    output << "{\n  \"version\": 1,\n  \"theme\": \"" << themeName
           << "\",\n  \"encoding\": \"" << encodingName
           << "\",\n  \"language\": \"" << languageName
           << "\",\n  \"markupColor\": \"" << ColorString(markupColor)
           << "\",\n  \"searchColor\": \"" << ColorString(searchColor)
           << "\",\n  \"byteColors\": {";
    // Serialize only configured byte colors. Stable numeric order produces small,
    // reviewable diffs when users edit profile.json manually.
    bool first = true;
    static constexpr char digits[] = "0123456789ABCDEF";
    for (unsigned value = 0; value < 256; ++value) {
        if (!byteColors[value]) continue;
        output << (first ? "\n" : ",\n") << "    \"" << digits[value >> 4] << digits[value & 15]
               << "\": \"" << ColorString(*byteColors[value]) << '"';
        first = false;
    }
    if (!first) output << '\n';
    output << "  }\n}\n";
    // A complete flushed sibling replaces the old profile atomically. Abrupt
    // termination can leave only an ignored .tmp file, never truncated JSON.
    const std::string contents = output.str();
    if (contents.size() <= kMaximumProfileBytes) {
        static_cast<void>(WriteProfileAtomically(FilePath(), contents));
    }
    } catch (...) {
        // Preferences are non-critical; failed persistence never interrupts an
        // editing command or crosses the Win32 callback exception boundary.
    }
}

Palette Profile::ResolvePalette() const {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    // Accessibility colors have absolute priority over both theme preference and
    // custom color rules.
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        return {SystemColor(COLOR_WINDOW), SystemColor(COLOR_BTNFACE), SystemColor(COLOR_WINDOWTEXT),
                SystemColor(COLOR_GRAYTEXT), SystemColor(COLOR_WINDOWTEXT), SystemColor(COLOR_HIGHLIGHT),
                SystemColor(COLOR_HIGHLIGHTTEXT), SystemColor(COLOR_HIGHLIGHT), SystemColor(COLOR_WINDOWTEXT), true, false};
    }
    // Accent-derived key colors are resolved only after the high-contrast exit.
    // They therefore affect selection, primary controls, focus, and the caret in
    // normal themes without overriding the accessibility palette.
    const std::uint32_t accent = WindowsAccentColor();
    const std::uint32_t accentText = ContrastingTextColor(accent);
    const bool dark = theme == ThemePreference::Dark || (theme == ThemePreference::System && SystemUsesDarkMode());
    // These neutral values follow the WinUI 3 light/dark surface hierarchy:
    // a quiet page background, one elevated content layer, high-emphasis text,
    // secondary text, and a low-contrast control/card stroke.
    if (dark) return {0x202020, 0x2B2B2B, 0xFFFFFF, 0xC5C5C5, 0x414141, accent, accentText, searchColor, accent, false, true};
    return {0xF3F3F3, 0xFFFFFF, 0x1A1A1A, 0x616161, 0xE5E5E5, accent, accentText, searchColor, accent, false, false};
}

std::uint32_t ColorRefToRgb(COLORREF color) { return (GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color); }
COLORREF RgbToColorRef(std::uint32_t color) { return RGB((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff); }
