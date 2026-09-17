// Implements the untrusted-data boundary for single-instance file-open IPC.

#include "OpenFilesProtocol.h"

#include "BinEditIdentity.h"

#include <algorithm>
#include <string>

namespace OpenFilesProtocol {
bool Parse(const COPYDATASTRUCT* transfer,
    std::vector<std::filesystem::path>& paths) noexcept {
    paths.clear();
    if (!transfer || transfer->dwData != BinEditIdentity::OpenFilesCopyDataId ||
        !transfer->lpData || transfer->cbData < sizeof(wchar_t) * 2 ||
        transfer->cbData > MaximumTransferBytes ||
        transfer->cbData % sizeof(wchar_t) != 0) return false;

    const auto* characters = static_cast<const wchar_t*>(transfer->lpData);
    const std::size_t count = transfer->cbData / sizeof(wchar_t);
    if (characters[count - 1] != L'\0' || characters[count - 2] != L'\0') return false;

    std::vector<std::filesystem::path> parsed;
    std::size_t cursor{};
    try {
        while (cursor + 1 < count && characters[cursor] != L'\0') {
            const wchar_t* terminator =
                std::find(characters + cursor, characters + count, L'\0');
            if (terminator == characters + count) return false;
            const std::size_t length =
                static_cast<std::size_t>(terminator - (characters + cursor));
            if (length == 0 || length > MaximumPathCharacters ||
                parsed.size() >= MaximumPathCount) return false;
            parsed.emplace_back(std::wstring(characters + cursor, terminator));
            cursor = static_cast<std::size_t>(terminator - characters) + 1;
        }
    } catch (...) {
        return false;
    }

    // Publish only a completely validated transaction. This prevents malformed
    // trailing data from causing a partial file-open side effect.
    if (cursor != count - 1 || parsed.empty()) return false;
    paths.swap(parsed);
    return true;
}
}
