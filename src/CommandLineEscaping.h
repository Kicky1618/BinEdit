#pragma once

// Implements the quoting grammar consumed by CommandLineToArgvW. ShellExecute
// accepts one parameter string rather than an argv array, so every dynamic
// argument must preserve quotes and runs of backslashes explicitly.

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

// CreateProcess-style command lines are limited to 32,767 UTF-16 code units
// including the trailing NUL. ShellExecute ultimately feeds the same parser.
inline constexpr std::size_t MaximumWindowsCommandLineCharacters = 32766;

[[nodiscard]] inline std::optional<std::wstring> QuoteWindowsCommandLineArgument(
    std::wstring_view argument) noexcept {
    if (argument.find(L'\0') != std::wstring_view::npos) return std::nullopt;
    try {
        std::wstring quoted;
        // In the worst case every source character is a backslash immediately
        // before a quote or the closing delimiter and therefore doubles.
        if (argument.size() > (quoted.max_size() - 3) / 2 ||
            argument.size() > MaximumWindowsCommandLineCharacters - 2) return std::nullopt;
        quoted.reserve(std::min(argument.size() * 2 + 2,
            MaximumWindowsCommandLineCharacters));
        quoted.push_back(L'"');

        const auto appendBackslashes = [&quoted](std::size_t count) {
            if (count > MaximumWindowsCommandLineCharacters - quoted.size()) return false;
            quoted.append(count, L'\\');
            return true;
        };

        std::size_t backslashes{};
        for (const wchar_t character : argument) {
            if (character == L'\\') {
                ++backslashes;
                continue;
            }
            if (character == L'"') {
                // Backslashes before a literal quote are doubled, followed by
                // one additional backslash that escapes the quote itself.
                const std::size_t remaining = MaximumWindowsCommandLineCharacters - quoted.size();
                if (remaining < 3 || backslashes > (remaining - 3) / 2 ||
                    !appendBackslashes(backslashes * 2 + 1)) return std::nullopt;
                quoted.push_back(L'"');
                backslashes = 0;
                continue;
            }
            const std::size_t remaining = MaximumWindowsCommandLineCharacters - quoted.size();
            if (remaining < 2 || backslashes > remaining - 2 ||
                !appendBackslashes(backslashes)) return std::nullopt;
            backslashes = 0;
            quoted.push_back(character);
        }

        // A run immediately before the closing quote must also be doubled or
        // CommandLineToArgvW would treat the delimiter as a literal quote.
        const std::size_t remaining = MaximumWindowsCommandLineCharacters - quoted.size();
        if (remaining < 1 || backslashes > (remaining - 1) / 2 ||
            !appendBackslashes(backslashes * 2)) return std::nullopt;
        quoted.push_back(L'"');
        return quoted;
    } catch (...) {
        return std::nullopt;
    }
}
