#pragma once

// Declares the UI-thread application coordinator. BinEditApp intentionally
// centralizes mutable editor state so Win32 callbacks can remain thin and all
// state transitions can request the correct R2PR damage region.

#include "Document.h"
#include "BinEditIdentity.h"
#include "BitToolDialog.h"
#include "Profile.h"
#include "RecoveryStore.h"
#include "Renderer.h"
#include "SearchDialog.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

class BinEditApp {
public:
    // instance is borrowed from wWinMain and remains valid for the process lifetime.
    explicit BinEditApp(HINSTANCE instance);
    // Stops and joins search work before any state captured by its worker can be
    // destroyed. HWND teardown normally performs the same cancellation earlier.
    ~BinEditApp();
    // Creates the UI, opens every command-line file as a tab, and owns the shared
    // message loop for this window and all subsequently detached windows.
    int Run(int showCommand, std::span<const std::filesystem::path> initialFiles);

private:
    enum class ExternalChangeResolution { None, Reloaded, Continue, Failed };
    // USER32 supplies three auto-pan cursors: a stationary origin plus the two
    // vertical directions. Keeping the logical state explicit prevents a busy
    // mouse stream from repeatedly installing the same shared cursor handle.
    enum class AutoScrollCursorState { None, Stationary, Up, Down };

    // Complete editor state owned by an inactive tab. The active tab remains in
    // the existing hot fields below so high-frequency editing does not add an
    // indirection to every input and render operation.
    struct EditorTabState {
        ByteDocument document;
        std::size_t caret{};
        std::size_t selectionAnchor{};
        std::size_t selectionActive{};
        std::size_t firstRow{};
        bool hexPane{true};
        bool lowNibble{};
        bool insertMode{true};
        std::vector<MarkupRange> markups;
        std::optional<SearchPattern> searchPattern;
        std::vector<std::size_t> searchOffsets;
        bool searchResultsStale{};
        std::wstring recoveryId;
        ULONGLONG recoveryCheckpointTick{};
        std::uint64_t recoveryDurableRevision{};
    };

    // Written only by the recovery jthread and consumed only after join. Shared
    // ownership lets the worker report completion without retaining BinEditApp.
    struct RecoveryWriteCompletion {
        std::uint64_t revision{};
        bool succeeded{};
    };

    // Result transferred from the coordinator thread to the UI thread. generation
    // prevents an older search from applying after an edit, tab switch, or query.
    struct SearchCompletion {
        std::uint64_t generation{};
        ParallelSearchResult result;
        bool selectNext{};
        bool notifyNotFound{};
    };

    // Static trampoline stores/retrieves this through GWLP_USERDATA.
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // Single message router for commands, input, rendering, theme, and lifetime events.
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    // detached selects the secondary HWND class; only Run may create the one
    // primary-class window for this process.
    bool CreateEditorWindow(int showCommand, bool detached);
    HMENU CreateMainMenu() const;
    void UpdateMenuChecks();
    void RebuildMenu();
    void UpdateTitle();
    // Registers a localized Restart Manager reason while unsaved bytes exist.
    void UpdateShutdownBlockReason();
    void ClearShutdownBlockReason();
    // Recovery checkpoints are copied on the UI thread, persisted on an owned
    // jthread, and atomically replaced. No background routine borrows editor state.
    void RestoreRecoverySessions();
    void ScheduleRecoveryCheckpoint();
    bool StartRecoveryCheckpoint();
    bool EnsureRecoveryCheckpoint();
    void JoinRecoveryWriter(bool requestStop);
    void StopRecoveryWriter();
    void ClearRecoveryCheckpoint();
    // Clamps and invalidates the application-owned D2D scrollbar model; no
    // non-client SCROLLINFO state is used.
    void UpdateScrollBar();
    void ScrollToFirstRow(std::size_t row);
    void SetScrollBarHovered(bool hovered);
    // Arms the application-owned tooltip delay for a tab label. Native tooltip
    // HWNDs are intentionally not used because the overlay is D3D11/D2D1 themed.
    void UpdateTabHover(std::optional<std::size_t> index);
    // Keeps the active tab inside the horizontally scrollable strip viewport and
    // clamps stale offsets after resize, DPI, close, dock, or reorder changes.
    void EnsureActiveTabVisible();
    void ScrollTabStrip(int tabDelta);
    void StartAutoScroll(POINT anchor);
    void StopAutoScroll();
    void TickAutoScroll();
    [[nodiscard]] AutoScrollCursorState CurrentAutoScrollCursorState() const noexcept;
    void UpdateAutoScrollCursor(bool force = false);
    // Require damage first, then coalesce all callers behind one posted render message.
    void ScheduleRender(R2PRRegion region, std::optional<std::size_t> byteOffset = std::nullopt);
    void ScheduleRenderRect(RECT rect);
    RenderModel MakeRenderModel() const;
    // Runs one editor-file operation on an owned worker while the UI thread
    // continues to dispatch paint, resize, DPI, and progress-timer messages.
    // All document-dependent interaction remains gated until the worker joins.
    bool RunIoOperation(const wchar_t* progressText,
        std::function<bool(DWORD&)> operation, DWORD& error);
    void SetIoBusy(bool busy, const wchar_t* progressText = nullptr);
    bool LoadDocumentWithProgress(const std::filesystem::path& path,
        ByteDocument& destination, DWORD& error);
    void QueueOpenPaths(std::vector<std::filesystem::path>&& paths);
    void ProcessPendingOpenPaths();

    [[nodiscard]] EditorTabState CaptureActiveTab();
    void RestoreActiveTab(EditorTabState&& state);
    void RefreshAfterTabChange();
    void CreateNewTab();
    bool OpenPathInNewTab(const std::filesystem::path& path);
    void AddTab(EditorTabState&& state, std::size_t index);
    [[nodiscard]] EditorTabState ExtractActiveTab();
    void SwitchTab(std::size_t index);
    bool CloseTab(std::size_t index);
    void ReorderActiveTab(std::size_t index);
    [[nodiscard]] bool CanCloseTab(std::size_t index) const noexcept;
    [[nodiscard]] bool CanDetachTab() const noexcept;
    bool ConfirmAllTabs();
    [[nodiscard]] bool HasDirtyTabs() const;
    bool ConfirmAllWindows();
    void DestroyAllWindows();
    // Releases controller objects whose detached HWND has completed WM_DESTROY.
    // Collection is deferred to the root message queue because deleting a
    // controller from inside its own window procedure would invalidate `this`
    // before the active HandleMessage frame returns.
    void CollectClosedDetachedHosts();
    BinEditApp* CreateDetachedHost(POINT screenPoint);
    void DetachActiveTab(POINT screenPoint);
    void DockActiveTabTo(BinEditApp& target, std::size_t index);
    [[nodiscard]] static BinEditApp* WindowAtPoint(POINT screenPoint);

    // Returns false only when closing/opening must be canceled.
    bool ConfirmDiscard();
    // Clears all view-only state after replacing the current byte sequence.
    void ResetDocumentSession();
    void OpenDialog();
    bool OpenPath(const std::filesystem::path& path);
    bool Save(bool saveAs);
    bool SaveDialog();
    // Resolves a detected on-disk version change on the UI thread. Clean files
    // reload automatically; dirty files present explicit Reload/Continue verbs.
    void CheckExternalChanges();
    ExternalChangeResolution ResolveActiveExternalChange(ExternalFileState state, DWORD queryError);
    ExternalChangeResolution QueryAndResolveActiveExternalChange(bool forceQuery);
    void ShowFileError(const wchar_t* operation, DWORD error) const;
    // Stages the current bytes and launches this executable through the UAC runas verb.
    bool RestartElevatedForSave(const std::filesystem::path& target);

    void ShowFind();
    void FindNext(bool fromNewSearch = false);
    void ApplySearchSelection(std::size_t offset);
    void StartAsyncSearch(bool selectNext, bool fromNewSearch, bool notifyNotFound);
    void QueueSearchRefresh();
    void CancelAsyncSearch();
    void HandleSearchCompletion();
    // Opens the GPU-rendered tool and applies one byte-wise transformation to the
    // current selection, or to the current byte when no range is selected.
    void ShowBitTool();
    void MoveCaret(std::ptrdiff_t delta, bool extend);
    // Applies clamping, selection semantics, pane focus, scrolling, and damage.
    void SetCaret(std::size_t offset, bool extend, bool hexPane);
    void EnsureCaretVisible();
    void EditHexDigit(int digit);
    void EditCharacter(wchar_t ch);
    void Backspace();
    void DeleteForward();
    // Rebases byte-addressed decorations after a size-changing deletion.
    void RebaseViewAfterErase(std::size_t offset, std::size_t count);
    // Keeps annotations attached to their original bytes after an insertion.
    void RebaseViewAfterInsert(std::size_t offset, std::size_t count);
    void Undo();
    void Redo();
    void ToggleMarkup();
    void ChooseMarkupColor();
    void ChooseByteColor();
    std::optional<std::uint32_t> PickColor(std::uint32_t initial) const;
    // Persists this window's preference change and broadcasts the resulting
    // profile to every detached host owned by the same process.
    void CommitProfileChange(bool localizedMenusChanged = false);
    // Applies both client palette policy and non-client immersive-dark attributes.
    void ApplyTheme();
    void ApplyLanguage(LanguagePreference language);
    const wchar_t* SearchErrorMessage(SearchPatternError error) const;

    HINSTANCE instance_{}; // Non-owning process module handle.
    BinEditApp* root_{};
    HWND window_{};
    bool primaryWindow_{true};
    ByteDocument document_;
    Profile profile_;
    Renderer renderer_;
    std::size_t caret_{};
    std::size_t selectionAnchor_{};
    std::size_t selectionActive_{};
    std::size_t firstRow_{};
    bool hexPane_{true};
    bool lowNibble_{};
    // Insert is the startup default; VK_INSERT toggles overwrite mode at runtime.
    bool insertMode_{true};
    bool dragging_{};
    bool tabDragging_{};
    POINT tabDragStart_{};
    std::optional<std::size_t> tabHoverCandidate_;
    std::optional<std::size_t> tabTooltipTab_;
    bool tabMouseTracked_{};
    std::size_t firstVisibleTab_{};
    int tabWheelDeltaRemainder_{};
    bool scrollBarDragging_{};
    bool scrollBarHovered_{};
    bool scrollBarMouseTracked_{};
    bool scrollBarModelValid_{};
    float scrollBarDragOffset_{};
    std::size_t scrollBarModelRows_{};
    std::size_t scrollBarModelVisible_{};
    std::size_t scrollBarModelFirst_{};
    int wheelDeltaRemainder_{};
    bool autoScrollActive_{};
    AutoScrollCursorState autoScrollCursorState_{AutoScrollCursorState::None};
    POINT autoScrollAnchor_{};
    POINT autoScrollPointer_{};
    ULONGLONG autoScrollLastTick_{};
    long double autoScrollRowAccumulator_{};
    // Prevents high-frequency mouse/key input from flooding the queue with frames.
    bool renderMessageQueued_{};
    // The UI thread owns these presentation fields. The document itself may be
    // borrowed by the file-I/O worker only while ioBusy_ is true.
    bool ioBusy_{};
    bool ioTitleRefreshPending_{};
    float ioProgressPhase_{};
    std::wstring ioProgressText_;
    std::deque<std::filesystem::path> pendingOpenPaths_;
    bool closingForElevation_{};
    bool applyingTheme_{};
    bool resolvingExternalChange_{};
    bool shutdownBlockRegistered_{};
    UiLanguage resolvedLanguage_{UiLanguage::Japanese};
    // Markups belong to the current document session and are cleared on file load.
    std::vector<MarkupRange> markups_;
    SearchDialogResult searchDialog_;
    // Retain the previous operation and operand between dialog invocations.
    BitToolDialogResult bitToolDialog_;
    std::optional<SearchPattern> searchPattern_;
    std::vector<std::size_t> searchOffsets_;
    bool searchResultsStale_{};
    bool searchInProgress_{};
    std::atomic_uint64_t searchGeneration_{};
    std::mutex searchCompletionMutex_;
    std::optional<SearchCompletion> searchCompletion_;
    std::wstring recoveryId_;
    ULONGLONG recoveryCheckpointTick_{};
    std::uint64_t recoveryDurableRevision_{};
    std::shared_ptr<RecoveryWriteCompletion> recoveryCompletion_;
    // The active slot is an empty placeholder; inactive slots own their complete
    // EditorTabState and are moved into the hot fields when selected.
    std::vector<EditorTabState> tabs_;
    std::size_t activeTab_{};
    // The root controller and its main HWND own process lifetime. Detached
    // controllers remain children of this object and are destroyed before the
    // main HWND completes its close transaction.
    std::vector<std::unique_ptr<BinEditApp>> detachedWindows_;
    // Declared last so its destructor joins before state referenced by the worker
    // begins member destruction, including on partial window-creation failures.
    std::jthread searchWorker_;
    // Captures every argument by value; destruction cannot leave a thread with a
    // pointer into this controller even if HWND creation or teardown is partial.
    std::jthread recoveryWorker_;
};
