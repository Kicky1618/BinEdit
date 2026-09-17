#pragma once

// Process-wide exploit mitigations and authenticated local-window boundaries.
// These helpers intentionally expose no policy knobs: every supported BinEdit
// process receives the same hardened defaults before COM or UI initialization.

#include <windows.h>

#include <cstdint>

namespace Security {
// Enables safe DLL lookup, heap-corruption termination, strict HANDLE checking,
// and legacy extension-point suppression. Unsupported policies fail closed only
// for that individual mitigation so BinEdit remains compatible with Windows.
void HardenCurrentProcess() noexcept;

// Terminates immediately when a C++ exception reaches an operating-system
// callback boundary. Unwinding through USER32 is unsupported and may corrupt
// callback state; the recovery checkpoint remains available on the next start.
[[noreturn]] void FailFast(const wchar_t* boundary) noexcept;

// Captures both the HWND mapping and process creation identity so the caller can
// revalidate the same peer immediately before acting on copied data. Same-user
// desktop IPC is not an OS security boundary, but this rejects null/recycled
// HWNDs, cross-session peers, sandboxes, and lower-integrity shatter input.
struct TrustedWindowPeer {
    HWND window{};
    DWORD processId{};
    std::uint64_t processCreationTime{};
};

[[nodiscard]] bool CaptureTrustedWindowPeer(HWND window, TrustedWindowPeer& peer) noexcept;
[[nodiscard]] bool RevalidateTrustedWindowPeer(const TrustedWindowPeer& peer) noexcept;
}
