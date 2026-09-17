#pragma once

// Defines the UI-independent availability rules for tab commands. Keeping this
// policy free of HWND and document ownership makes boundary cases directly
// testable while BinEditApp remains responsible for the actual transactions.

#include <cstddef>

namespace TabPolicy {
struct State {
    std::size_t tabCount{};
    std::size_t activeTab{};
    bool primaryWindow{};
    bool activeDocumentHasPath{};
    bool activeDocumentDirty{};
};

[[nodiscard]] constexpr bool CanClose(const State& state, std::size_t requestedTab) noexcept {
    if (requestedTab >= state.tabCount) return false;
    if (!state.primaryWindow || state.tabCount > 1) return true;

    // The main HWND remains the stable process anchor. Closing its final named
    // document, or a dirty untitled document, replaces that session with a clean
    // untitled placeholder. A pristine placeholder itself has nothing to close.
    return requestedTab == state.activeTab &&
        (state.activeDocumentHasPath || state.activeDocumentDirty);
}

[[nodiscard]] constexpr bool CanDetach(const State& state) noexcept {
    // Moving the only tab into another new window cannot add useful layout and
    // would leave an empty host. This applies equally to main and sub windows.
    return state.tabCount > 1;
}
}
