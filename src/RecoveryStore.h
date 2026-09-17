#pragma once

// Durable crash-recovery storage for unsaved document bytes. Records are written
// atomically beneath the current user's LocalAppData directory, carry a SHA-256
// integrity digest, and never modify their original document path while loading.

#include <windows.h>

#include "Document.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace RecoveryStore {
struct Record {
    std::wstring id;
    std::filesystem::path originalPath;
    std::vector<std::uint8_t> bytes;
    std::uint64_t timestamp{};
};

// Returns a CNG-generated 128-bit lowercase hexadecimal identifier. An empty
// result means the operating-system random provider was unavailable.
[[nodiscard]] std::wstring NewId();
// Writes a complete owned snapshot through a flushed random sibling and atomic
// replacement. Cancellation removes only the unfinished sibling; a previously
// complete recovery record remains available.
[[nodiscard]] bool Write(std::wstring_view id, const std::filesystem::path& originalPath,
    std::span<const std::uint8_t> bytes, std::stop_token stopToken, DWORD& error) noexcept;
// Streams a piece-table snapshot in bounded chunks, avoiding a second complete
// private byte vector for mapped or sparsely edited large documents.
[[nodiscard]] bool Write(std::wstring_view id, const std::filesystem::path& originalPath,
    const ImmutableByteSnapshot& snapshot, std::stop_token stopToken, DWORD& error) noexcept;
// Deletes one validated identifier. Invalid IDs can never escape the recovery
// directory or influence a filesystem path.
void Remove(std::wstring_view id) noexcept;
// Loads only structurally complete records whose SHA-256 digest matches. Invalid
// final records are ignored and retained for forensic/manual recovery; exact-name
// temporary files left by an interrupted atomic write are cleaned on enumeration.
[[nodiscard]] std::vector<Record> Enumerate() noexcept;
// Returns the recovery directory without creating it.
[[nodiscard]] std::filesystem::path Directory() noexcept;
}
