# BinEdit Developer Guide

This file describes the engineering contracts that must remain true when BinEdit is modified. It lives beside `BinEdit.slnx` so both human contributors and coding agents see the same project-level guidance.

## Build contract

- Use Visual Studio 2026 and MSVC platform toolset `v145`.
- Keep the solution in the XML `.slnx` format; do not generate a legacy `.sln` file.
- The supported solution platform is `x64` with `Debug` and `Release` configurations.
- The application is native C++20 and must not acquire a mandatory managed or third-party runtime.
- Keep `/utf-8`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`, warning level 4, SDL checks, and conformance mode enabled.
- Add every new source/header/resource to both `BinEdit.vcxproj` and `BinEdit.vcxproj.filters`.

Build and test from PowerShell:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild BinEdit.slnx /m /p:Configuration=Debug /p:Platform=x64
& .\x64\Debug\BinEdit.Core.Tests.exe
& .\tests\AsyncFileIoUiTests.ps1 -Configuration Debug
& .\tests\EditorUiStress.ps1 -Configuration Debug
& .\tests\ScrollBarUiTests.ps1 -Configuration Debug
& .\tests\MiddleScrollUiTests.ps1 -Configuration Debug
& .\tests\ExternalChangeUiTests.ps1 -Configuration Debug
& .\tests\MemoryReleaseUiTests.ps1 -Configuration Debug
& .\tests\MemoryRegionStressUiTests.ps1 -Configuration Debug
& .\tests\SearchAsyncUiTests.ps1 -Configuration Debug
& .\tests\TabDockingUiTests.ps1 -Configuration Debug
& .\tests\TabStripScrollUiTests.ps1 -Configuration Debug
& .\tests\UntitledCloseUiTests.ps1 -Configuration Debug
& .\tests\RecoveryUiTests.ps1 -Configuration Debug
& $msbuild BinEdit.slnx /m /p:Configuration=Release /p:Platform=x64
& .\x64\Release\BinEdit.Core.Tests.exe
& .\tests\AsyncFileIoUiTests.ps1 -Configuration Release
& .\tests\EditorUiStress.ps1 -Configuration Release
& .\tests\ScrollBarUiTests.ps1 -Configuration Release
& .\tests\MiddleScrollUiTests.ps1 -Configuration Release
& .\tests\ExternalChangeUiTests.ps1 -Configuration Release
& .\tests\MemoryReleaseUiTests.ps1 -Configuration Release
& .\tests\MemoryRegionStressUiTests.ps1 -Configuration Release
& .\tests\SearchAsyncUiTests.ps1 -Configuration Release
& .\tests\TabDockingUiTests.ps1 -Configuration Release
& .\tests\TabStripScrollUiTests.ps1 -Configuration Release
& .\tests\UntitledCloseUiTests.ps1 -Configuration Release
& .\tests\RecoveryUiTests.ps1 -Configuration Release
```

## Architecture map

| Area | Files | Responsibility |
|---|---|---|
| Process and UI state | `main.cpp`, `App.h/.cpp`, `TabPolicy.h` | Named-mutex startup routing, primary/detached HWND identities, command routing, UI-independent tab-command availability, per-tab state, detach/dock ownership, file workflows, input, and shutdown policy |
| Document core | `Document.h/.cpp` | Read-only file mapping, persistent piece-table storage, sparse/atomic streaming I/O, external version monitoring, edit history, encoding, and partitioned binary/text search |
| Main rendering | `Renderer.h/.cpp` | D3D11/D2D1/DirectWrite tab strip, hex view, custom scrollbar, hit testing, and R2PR damage presentation |
| Modal rendering | `DialogSurface.h/.cpp` | Reusable D3D11/D2D1 swap-chain surface for custom dialogs |
| Color picker | `ColorPickerDialog.h/.cpp`, `ColorMath.h/.cpp` | Themed HSV/RGB selection, numeric editing, and tested color conversion |
| Stable message UI | `MessageDialog.h/.cpp` | USER32/GDI-only confirmation, warning, and error dialogs |
| Search UI | `SearchDialog.h/.cpp` | Modal custom search controls, Unicode input, clipboard, inline validation |
| Bit-operation core and UI | `BitOperations.h/.cpp`, `BitToolDialog.h/.cpp` | Tested byte calculations and themed document-independent tool |
| About UI | `AboutDialog.h/.cpp` | Modal custom About screen |
| Recovery | `RecoveryStore.h/.cpp` | Private, integrity-checked, atomic unsaved snapshots and startup enumeration |
| Process security | `Security.h/.cpp` | Exploit mitigations, callback fail-fast, and authenticated local-window IPC boundary |
| Command-line boundary | `CommandLineEscaping.h` | Allocation-safe inverse quoting for `CommandLineToArgvW` before UAC relaunch |
| Theme and profile | `Profile.h/.cpp`, `ThemeMenu.h/.cpp` | Persistent colors, system/high-contrast palette, classic menu-bar and popup-item theming |
| Localization | `Localization.h/.cpp` | Complete Japanese/English string tables and system-language resolution |
| Verification | `tests/CoreTests.cpp`, `tests/AsyncFileIoUiTests.ps1`, `tests/EditorUiStress.ps1`, `tests/ScrollBarUiTests.ps1`, `tests/MiddleScrollUiTests.ps1`, `tests/BitToolUiTests.ps1`, `tests/ExternalChangeUiTests.ps1`, `tests/MemoryReleaseUiTests.ps1`, `tests/MemoryRegionStressUiTests.ps1`, `tests/SearchAsyncUiTests.ps1`, `tests/TabDockingUiTests.ps1`, `tests/TabStripScrollUiTests.ps1`, `tests/UntitledCloseUiTests.ps1`, `tests/RecoveryUiTests.ps1` | Real-file model and tab-command-policy tests plus asynchronous editor-file I/O, byte-for-byte editor, scrollbar, middle-button auto-scroll, bit-operation tool, external-version, closed-file and detached-host memory/resource release, Explorer taskbar responsiveness, asynchronous search, main/sub-window roles, tab-strip scrolling, final-tab close, and multi-window forced-termination recovery automation |

## Non-negotiable runtime invariants

### UI thread and lifetimes

- All HWND, D3D11, D2D1, DirectWrite, menu, dialog, and interactive editor-state transitions run on the main UI thread. A file-I/O worker may exclusively borrow or mutate one `ByteDocument` only while its owning controller is in the explicit busy state.
- File Load, Save, Save As, external reload, recovery publication required by tab transfer, and elevation staging must use the owned file-I/O worker. While it runs, the UI thread pumps paint/resize/DPI/theme messages, renders only the opaque progress model, disables all document-dependent commands and input, rejects close/session-end completion, and never reads the borrowed document. Join before clearing the busy state.
- The I/O progress timer is legal only while busy and invalidates only `R2PRRegion::IoProgress` after the one initial full disabled-surface frame. It must never become an idle animation loop.
- Validated IPC and dropped paths received while busy are copied into a bounded owning queue and opened after the current worker joins. Never retain pointers into `COPYDATASTRUCT` or `HDROP` storage.
- Search coordinators and their bounded partition workers operate only on an owned byte/pattern snapshot. They return results through a generation-checked posted message; worker threads must never touch HWND, renderer, document, or tab state directly.
- Recovery workers capture document bytes, path, and record ID by value. They must never retain a controller or HWND pointer. Normal teardown requests stop and joins every `std::jthread`; forced process termination relies only on the last atomically published external record.
- Cancel and join the active search before destroying a window or moving its active document state. Editing-triggered searches are debounced, and stale completion messages must be harmless after cancellation or replacement.
- Detached HWND destruction must release its D3D/DXGI device graph immediately and defer controller deletion to a root-window message after the child window procedure returns. Closed controllers must not remain in the root ownership vector. Keep detached editors owned by the one primary HWND so taskbar thumbnail resources do not grow with detach/close cycles.
- `RenderModel` borrows its `ByteDocument` plus search/markup containers. They must stay alive and unchanged until `Renderer::Render` returns.
- Custom modal window objects are stack-owned and must not return from their `Show` method until the corresponding HWND has received `WM_DESTROY`.
- Every created GDI object, HMENU, HANDLE, HGLOBAL not transferred to Windows, and COM interface must have an explicit release path.
- WinRT wrappers that require the wWinMain apartment must be released before `RoUninitialize`; do not retain them in function-static storage whose destructor runs during later CRT shutdown.
- The main editor message loop must translate and dispatch keyboard messages directly. Do not pass the top-level editor HWND to `IsDialogMessage`; it can consume editor-owned Tab, arrow, or text input. `WM_CHAR` is the translated character authority: reject control code points, but do not resample live modifier state after a character may have waited in the posted queue.
- The root `BinEditApp` and its main HWND own process lifetime and every detached controller. A successful main-window close must confirm all documents, destroy every sub HWND first, and then destroy the main HWND; a sub window must never survive its main owner.
- Exactly one primary HWND uses `BinEdit.MainWindow`; every tab-created top-level host uses `BinEdit.DetachedTabWindow`. Sub menus must omit Theme, Language, and File/Exit, while committed process-wide settings still propagate to every live sub window. Do not reuse the primary class for a detached controller.
- The primary process owns the exact named mutex `MX_BinEdit_F80F` for its complete lifetime. Later normal launches must forward validated, double-null-terminated UTF-16 paths through `WM_COPYDATA`, then exit without creating another primary HWND.
- Keep UIPI's default integrity boundary. Before forwarding, validate that the discovered target belongs to the same session/user and is at least the launcher's integrity level. At receipt, `WM_COPYDATA` requires the symmetric sender check. Capture the HWND/PID/process-creation-time tuple and revalidate that tuple plus the token trust relationship immediately before sending or opening any path; an HWND-to-PID result observed only once is not an authority. Validate the payload identifier, byte size, alignment, final double NUL, path count, and every string boundary before opening any path.
- Mutex creation failure is fatal to startup. Never continue without `MX_BinEdit_F80F`, because that would silently violate the single-primary contract under object squatting or resource exhaustion.
- C++ exceptions must not escape `wWinMain` or a registered window procedure. Callback boundaries call `Security::FailFast` so USER32 never observes C++ unwinding and durable recovery remains available.
- Each window keeps the active tab in its hot editor fields; the corresponding `tabs_[activeTab_]` entry is an empty placeholder. Capture the hot state before switching, reordering, detaching, or docking, then restore exactly one selected state.
- A detached tab transfers its complete document, caret, selection, viewport, input mode, markup, search results, and Undo/Redo history. Never reopen the file to emulate a transfer.
- Detaching is enabled only when the source window owns at least two tabs. A newly detached host must receive active keyboard focus after all source-window refresh work completes.

### R2PR rendering

- Do not introduce an unconditional render loop. A frame is legal only after code calls `Require`, `RequireRect`, or a dialog `RequestDraw` equivalent.
- Coalesce multiple invalidations behind one posted render message.
- The main renderer must pass the same damage rectangle to the D2D clip and `IDXGISwapChain1::Present1` dirty rectangles.
- A new two-buffer flip-sequential swap chain requires two complete warm-up frames before partial rendering is safe.
- Present with sync interval one. This permits native refresh operation through 240 Hz without busy-waiting.
- Device-loss recovery must recreate resources and request another complete frame.
- Tab title/dirty-state changes may request only `R2PRRegion::Tabs`; tab count, DPI, window size, or active document layout changes require a complete frame.
- A tab strip that cannot fit every tab keeps a clamped first-visible index. Drawing, hit testing, tooltip lookup, reorder/dock gaps, active-tab visibility, and Shift+wheel horizontal movement must all use that same viewport origin.
- Tab titles use DirectWrite character trimming with the dirty indicator outside the trimmed layout. Publish a tab-tooltip candidate only after the hover delay, render it inside the D3D/D2D surface only when that title is actually trimmed, and redraw the complete scene when showing or dismissing the overlay so covered editor pixels cannot remain stale.
- `LayoutMetrics::visibleRows` counts only complete row rectangles by truncating the usable data height. Residual pixels above the status band are neither rendered nor hit-testable, and scrolling, paging, and caret visibility must use the same count. Keep an explicit nested D2D clip at the complete-row boundary so antialiased byte-color fills cannot leak into stale residual or status pixels during a full frame.
- The vertical scrollbar is client-owned D2D content. Do not restore `WS_VSCROLL`, `SCROLLINFO`, or `WM_VSCROLL`; render and hit-test the same DPI-rounded geometry, retain `size_t` row precision, and invalidate only `R2PRRegion::ScrollBar` when its visual state alone changes.
- Middle-button auto-scroll owns mouse capture and its timer only while active. Its D2D origin marker is invalidated by its tight bounds, its system auto-pan cursor must distinguish the stationary/up/down states, row movement reuses normal data/scrollbar damage, and every competing input, focus loss, resize, DPI transition, or teardown path must release capture and stop the timer.

### Themes and accessibility

- Resolve `Palette` when a frame or theme refresh is needed; system theme and high contrast can change while the application is running.
- In normal light and dark themes, `Palette::selection` and `Palette::caret` come from the current user's `UISettings.GetColorValue(UIColorType::Accent)` value. DWM colorization is only a compatibility fallback. Choose `selectionText` for contrast against that runtime color and refresh on Windows color/settings notifications.
- Windows high-contrast colors override saved theme and arbitrary byte/markup colors.
- The client visual language follows WinUI 3 conventions while remaining a native D3D11/D2D1 implementation: Segoe UI Variable hierarchy, restrained neutral layers, one primary content card, four/eight-pixel corner radii, and visible keyboard focus. Do not introduce redundant nested cards.
- Renderer breakpoints may tighten column metrics, then hide the secondary character pane as one complete unit. Never clip an arbitrary suffix of the hexadecimal grid merely to retain part of the character pane.
- The search, bit-operation, and About client areas must remain fully D3D11/D2D1/DirectWrite rendered. Do not reintroduce native edit/radio/combo/button child controls.
- Color selection is application-owned through `ColorPickerDialog.*`; do not reintroduce `ChooseColor`, `CHOOSECOLOR`, or COLORREF state at picker call sites. Keep HSV/RGB conversion UI-independent and covered by core tests.
- Current-byte color and clear-color commands are enabled only when `caret_ < document_.Size()`. The virtual EOF cell has no readable byte value, and command handlers must independently retain the same bounds guard against synthesized or stale `WM_COMMAND` delivery.
- Confirmation, warning, and error prompts use `MessageDialog.*`, which must remain independent of D3D11/D2D1 so graphics-initialization failures still have a reliable UI path.
- MessageDialog buttons mirror D2D dialog typography: 14-DIP Segoe UI Variable Text, semibold for the primary action and normal weight for secondary actions.
- MessageDialog body and button text require `TRANSPARENT` GDI background mode so the DC default color cannot overwrite themed surfaces. Tab advances button focus and Shift+Tab wraps it backward; modifier state must be cleared when focus is lost.
- MessageDialog symbols remain D3D-independent and are built from normalized, supersampled GDI paths. Preserve distinct information-circle, rounded warning-triangle, error-cross, and question-curve geometry together with system/high-contrast palette colors, the direct-draw allocation fallback, and the inset glyph box that keeps the marks visually subordinate to their 38-DIP badges.
- `MessageDialog.*` must handle both `WM_GETDPISCALEDSIZE` and `WM_DPICHANGED`: remeasure wrapped text for the destination DPI, resize to the resulting outer window dimensions, clamp to the destination work area, and synchronously rebuild button hit-test rectangles.
- Keep the Win32 menu implementation system-native. Client-area Fluent restyling must not replace the existing UAH/native popup-menu integration with custom D2D menu controls.
- UAH menu messages and ordinal `uxtheme.dll` exports are undocumented. Check every pointer/message payload and preserve the native USER32 fallback, especially for light and high-contrast modes. Popup-item theming must be reapplied and its MENU theme cache flushed whenever the resolved theme changes or the localized HMENU hierarchy is rebuilt.
- Guard `SetWindowTheme` paths against synchronous `WM_THEMECHANGED` re-entry.
- Theme, language, encoding, and persistent arbitrary-color changes must be committed once and broadcast to every live detached window so application-level settings never diverge.

### Document safety

- Treat `FILE_ATTRIBUTE_READONLY` as live save-time state, never as a property cached by Load. `Save`, existing-target `SaveAs`, full replacement, and sparse-write paths must report `ERROR_FILE_READ_ONLY`; this error must never enter the ACL-only elevated-restart path. Re-query after an `ERROR_ACCESS_DENIED` failure to narrow attribute-change races.

- A failed load must leave the existing document unchanged.
- A successful non-empty load uses a read-only Windows file mapping instead of copying the complete file into a private byte vector. The persistent piece table may share mapped and owned blocks across immutable snapshots; edits must reuse unaffected pieces and allocate only replacement bytes plus bounded metadata.
- Access to a mapped view can raise `EXCEPTION_IN_PAGE_ERROR` after device or network failure. Keep mapped copies behind the SEH read boundary and propagate range-copy failure instead of allowing a structured exception to cross C++ or worker boundaries.
- Rendering reads only visible bytes. Parallel search copies at most a bounded candidate chunk plus pattern overlap per worker, and save/recovery streaming buffers remain bounded. Do not flatten a mapped snapshot merely to satisfy a contiguous-span API.
- Closing or replacing a document must release its mapping/piece state, Undo/Redo stacks, dirty-range plan, search offsets, and markups. The final main-window tab reuses a hot controller, so `NewEmpty` and the corresponding view reset must swap with empty owning containers rather than call `clear()` and retain the closed file's allocations.
- Normal same-path fixed-size overwrites write only sorted, merged dirty byte ranges through one deny-write/deny-delete handle and flush before reporting success. A structural insert/delete over mapped storage must use bounded-buffer atomic publication: resizing a mapped stream can fail with `ERROR_USER_MAPPED_FILE`, and in-place forward copying can overwrite source bytes that later chunks still require. Finish and join an atomically published recovery record for the exact document revision before any non-atomic sparse write. If the checkpoint fails, use full atomic publication instead.
- Keep dirty-range tracking bounded without turning thousands of isolated edits into one huge in-place span. Once the range cap is reached, discard the sparse plan and require the next save to publish a full atomic snapshot.
- A same-path save must compare volume/file identity, last-write time, and size both before opening and after acquiring the locked write handle. Return a conflict instead of applying ranges to an unrecognized disk version.
- Save As, recovered snapshots, acknowledged external conflicts, and partial-write failure recovery use a complete atomic snapshot. Write and flush a CNG-named sibling before publication; use `ReplaceFileW` for an existing destination and `MoveFileExW` for a genuinely new path. Never truncate the destination on this full-snapshot path.
- Monitor each loaded path through an owned `FindFirstChangeNotification` handle and exact file stamps. Transfer that handle with `ByteDocument` during tab moves and close it exactly once. A clean changed document reloads automatically; a dirty changed document requires localized Reload/Continue verbs. Continue re-baselines the external version and forces the next full atomic save.
- If an external path disappears, retain its in-memory contents as dirty data. Notification consumption must remain sticky until reload/continue resolves it, including when a reload temporarily fails because another publisher denies sharing.
- An access-denied save finishes the latest recovery generation, stages current bytes under a CNG name, and invokes the UAC `runas` relaunch. Quote every dynamic internal argument with `QuoteWindowsCommandLineArgument`; hand-written surrounding quotes are invalid for embedded quotes and trailing backslashes. The elevated process must require the complete internal argument protocol, verify its elevated token, show the exact target for a second confirmation, hold a validated non-reparse staging handle against replacement, and save through `ByteDocument` atomic persistence.
- `Ctrl+W` closes only the active tab and must use the same save-confirmation transaction as normal window close and file replacement. Detach is disabled whenever the source window has only one tab. Closing the main window's final named document or dirty untitled document replaces it with a pristine untitled placeholder; the pristine placeholder itself cannot close. Closing a sub window's final tab destroys that sub HWND. Closing the main window checks every dirty tab in every live window before any HWND is destroyed.
- `Ctrl+Shift+S` invokes Save As; test the modifier before the ordinary `Ctrl+S` path so the shortcut cannot degrade into a normal save.
- The bit-operation tool must open with an empty document and at virtual EOF. Its input, operand, and result are dialog-owned and must never read or mutate editor state.
- A dirty document registers a localized `ShutdownBlockReason`. `WM_QUERYENDSESSION` must not display modal UI; it finishes the active recovery generation and returns `FALSE` while dirty, or `TRUE` when clean, with registration cleanup on save, close, `WM_ENDSESSION`, and destruction.
- Recovery records live only under `%LOCALAPPDATA%\BinEdit\Recovery`, use a protected current-user/SYSTEM DACL, CNG IDs, SHA-256 structural integrity, flush, and atomic replacement. Compare every digest byte without early exit, even though the current digest is an integrity checksum rather than a keyed authenticator. EFS is best effort because it is unavailable on some Windows editions and volumes. Reject reparse directories/files, malformed sizes, embedded path NULs, and digest mismatches.
- The first dirty transition checkpoints immediately; idle edits debounce for 500 ms and sustained input is capped at a two-second checkpoint interval. Switching or moving tabs and shutdown queries must finish the current generation. Saving or explicitly discarding removes the record; dismissing a recovery prompt without a decision retains it.
- Startup recovery belongs exclusively to the one main controller. Accepted records from documents that previously lived in any window are restored as main-window tabs in chronological order; recovery must not recreate the prior sub-window topology.
- Search parser errors remain language-neutral enum values; translate them only in UI code.
- The custom D2D search dialog must not render native ampersand mnemonic syntax. Mouse radio selection, Space/arrow navigation, and explicit Alt+B/Alt+T/Alt+Q handling must update the same `binary` mode state.
- Search text and binary patterns are bounded at 65,536 UTF-16 code units in both UI and core layers. Conversion to Win32 `int` lengths must occur only after this check, and allocation failure must remain a normal parser error.
- Preserve overlapping search matches and the highlight-result cap unless a replacement has equivalent bounded behavior.
- Parallel search partitions must cover every legal candidate exactly once, merge highlights in ascending order, preserve exact Find Next/wrap behavior beyond the highlight cap, and poll the shared stop token during both candidate and long-pattern loops.
- `bytes.size()` is a valid virtual EOF caret position, but it is not stored or saved. Materialize bytes only when the user types at that position.
- Commands that require the byte at the caret, including current-value coloring and selection markup, must be disabled while the caret is at virtual EOF and retain an authoritative command-handler guard.
- Insert mode is the process startup default and inserts bytes at the caret; `VK_INSERT` toggles overwrite mode, where existing-byte input replaces and EOF input appends. Multi-byte insertion and range erase operations must remain single, reversible document-history operations.
- Backspace removes one byte in the hexadecimal pane and one validated encoded character in the character pane. Malformed UTF-8, UTF-16 LE, or Shift-JIS tails must fall back conservatively rather than deleting an unverified neighbor.
- Bit operations calculate one dialog-owned input byte. Arithmetic wraps modulo 256, shifts and rotations accept counts from 0 through 7, and division rejects zero.

### Localization

- All application-owned user-visible text belongs in `UiStrings`; Japanese and English entries must be added together.
- Technical encoding names may remain shared when their standard spelling is language-independent.
- `LanguagePreference::System` resolves from the Windows user UI language. It must remain distinct from an explicitly selected language in `profile.json`.
- Runtime language changes rebuild the complete menu hierarchy and refresh all D2D text.

## Code and comment policy

- Source comments are written in English.
- Comments should explain contracts, ownership, state transitions, platform constraints, failure behavior, or non-obvious performance choices. Avoid narrating self-evident syntax.
- Public declarations should document borrowing/ownership, error reporting, and side effects.
- Keep UI strings out of comments when they are intended to be displayed; use the localization tables instead.
- Prefer narrow helper types and `Microsoft::WRL::ComPtr` for COM ownership.
- Use `std::span` for borrowed contiguous data and `std::filesystem::path` for filesystem identities.

## Security hardening contract

- Keep SDL checks, stack cookies, Spectre mitigation, CFG, EH continuation metadata, CET compatibility, DEP, ASLR, and High Entropy VA enabled in both Debug and Release project configurations.
- `Security::HardenCurrentProcess` runs before DPI, COM/WinRT, HWND, or worker initialization. Preserve safe DLL directories, heap-corruption termination, strict handle checking, extension-point disable, and remote/low-integrity image-load rejection.
- Treat same-user IPC as untrusted input even though the Windows user is the actual security boundary. Keep payload and collection limits, validate the complete transaction before side effects, and never relax integrity checks merely to forward from a non-elevated shell into an elevated editor.
- Bound data imported from files, profiles, recovery records, IPC, clipboard, and dialogs before allocation or Win32 signed-length conversion. A failed allocation must not partially modify the document.
- `taskkill /F` and `TerminateProcess` cannot execute application cleanup. Do not claim otherwise: robustness comes from process-owned worker lifetimes plus already-flushed, process-independent recovery generations. `RegisterApplicationRestart` covers eligible crash/hang cases, not an explicit forced kill.

## Change checklist

Before handing off a change:

1. Update project and filter files for any added source.
2. Build Debug and Release through `BinEdit.slnx` with `v145`.
3. Run both core-test executables.
4. Smoke-test any changed HWND flow, including clean modal close and process exit.
5. Check Japanese and English labels when UI text changes.
6. Check system, light, dark, and high-contrast palette logic when rendering changes.
7. Confirm idle UI does not continuously render.
8. Run all HWND automation scripts, including `MemoryReleaseUiTests.ps1`, `MemoryRegionStressUiTests.ps1`, and `TabStripScrollUiTests.ps1`, when editing, search, tab layout, input routing, window lifetime, memory ownership, or docking behavior changes.
9. Run `tests\RecoveryUiTests.ps1` after persistence, lifecycle, recovery, elevation, or shutdown changes.
10. Run MSVC native code analysis and inspect Release PE/load-config mitigations after security-sensitive changes.
