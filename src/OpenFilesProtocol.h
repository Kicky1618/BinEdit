#pragma once

// Parser for the bounded WM_COPYDATA payload used by single-instance routing.
// Sender authentication remains the caller's responsibility; this layer only
// validates the complete byte representation before returning any paths.

#include <windows.h>

#include <filesystem>
#include <vector>

namespace OpenFilesProtocol {
inline constexpr DWORD MaximumTransferBytes = 8u * 1024u * 1024u;
inline constexpr std::size_t MaximumPathCount = 256;
inline constexpr std::size_t MaximumPathCharacters = 32766;

[[nodiscard]] bool Parse(const COPYDATASTRUCT* transfer,
    std::vector<std::filesystem::path>& paths) noexcept;
}
