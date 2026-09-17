// Implements the main Win32 application controller.
//
// This translation unit owns the top-level message dispatch, command routing,
// document/caret/selection state, persistence workflows, and render scheduling.
// Rendering itself is delegated to Renderer; the controller only declares
// invalid regions through the R2PR contract and never paints with GDI.

#include "App.h"
#include "AboutDialog.h"
#include "ColorPickerDialog.h"
#include "CommandLineEscaping.h"
#include "MessageDialog.h"
#include "OpenFilesProtocol.h"
#include "resource.h"
#include "Security.h"
#include "TabPolicy.h"
#include "ThemeMenu.h"

#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <limits>
#include <string>

namespace {
std::vector<BinEditApp*> gEditorWindows;
constexpr UINT WM_APP_SEARCH_COMPLETE = WM_APP + 42;
constexpr UINT WM_APP_IO_COMPLETE = WM_APP + 43;
constexpr UINT WM_APP_PROCESS_PENDING_OPEN = WM_APP + 44;
constexpr UINT WM_APP_COLLECT_DETACHED = WM_APP + 45;
constexpr UINT_PTR kSearchRefreshTimer = 0xB1E5;
constexpr UINT kSearchRefreshDelayMilliseconds = 150;
constexpr UINT_PTR kRecoveryTimer = 0xB1E6;
constexpr UINT kRecoveryIdleMilliseconds = 500;
constexpr ULONGLONG kRecoveryMaximumIntervalMilliseconds = 2000;
constexpr UINT_PTR kExternalChangeTimer = 0xB1E7;
constexpr UINT kExternalChangeIntervalMilliseconds = 750;
constexpr UINT_PTR kAutoScrollTimer = 0xB1E8;
constexpr UINT kAutoScrollIntervalMilliseconds = 8;
constexpr UINT_PTR kTabTooltipTimer = 0xB1E9;
constexpr UINT kTabTooltipDelayMilliseconds = 500;
constexpr UINT_PTR kIoProgressTimer = 0xB1EA;
constexpr UINT kIoProgressIntervalMilliseconds = 50;
// These stable USER32 resource identifiers are the system auto-pan cursor set.
// Loading them as shared system cursors honors the user's Windows cursor scheme
// without adding fixed-resolution cursor bitmaps to the application resources.
constexpr WORD kPanMiddleCursorResource = 32654;
constexpr WORD kPanNorthCursorResource = 32655;
constexpr WORD kPanSouthCursorResource = 32657;

// Menus are built at runtime because every label can change when the user
// switches languages without restarting the process.
void AppendMenuItem(HMENU menu, UINT id, const wchar_t* text) { AppendMenuW(menu, MF_STRING, id, text); }

// Include the row containing the virtual EOF caret. For a partial final row this
// is already the last data row; an exact row boundary requires one extra row.
std::size_t RowCount(std::size_t bytes) { return bytes / LayoutMetrics::bytesPerRow + 1; }

// Converts only ASCII hexadecimal input. Locale-sensitive digit conversion is
// deliberately avoided because the editor writes exact nibbles.
int HexValue(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}

bool IsUtf8Continuation(std::uint8_t byte) { return (byte & 0xC0u) == 0x80u; }

bool IsShiftJisLead(std::uint8_t byte) {
    return (byte >= 0x81u && byte <= 0x9Fu) || (byte >= 0xE0u && byte <= 0xFCu);
}

bool IsShiftJisTrail(std::uint8_t byte) {
    return (byte >= 0x40u && byte <= 0x7Eu) || (byte >= 0x80u && byte <= 0xFCu);
}

// Finds the byte boundary of the character immediately preceding end. Invalid
// or partial encoded data deliberately falls back to one byte so Backspace never
// consumes an unverified neighboring character.
std::size_t PreviousCharacterStart(const ByteDocument& document,
    std::size_t end, TextEncoding encoding) {
    end = std::min(end, document.Size());
    if (end == 0) return 0;
    if (encoding == TextEncoding::Ascii) return end - 1;

    if (encoding == TextEncoding::Utf16Le) {
        if ((end & 1u) != 0 || end < 2) return end - 1;
        std::size_t start = end - 2;
        const std::uint16_t unit = static_cast<std::uint16_t>(document.ByteAt(start) |
            (document.ByteAt(start + 1) << 8));
        if (unit >= 0xDC00u && unit <= 0xDFFFu && start >= 2) {
            const std::uint16_t previous = static_cast<std::uint16_t>(document.ByteAt(start - 2) |
                (document.ByteAt(start - 1) << 8));
            if (previous >= 0xD800u && previous <= 0xDBFFu) start -= 2;
        }
        return start;
    }

    if (encoding == TextEncoding::Utf8) {
        std::size_t start = end - 1;
        while (start > 0 && end - start < 4 &&
            IsUtf8Continuation(document.ByteAt(start))) --start;
        const std::uint8_t lead = document.ByteAt(start);
        const std::size_t expected = lead < 0x80u ? 1u :
            ((lead & 0xE0u) == 0xC0u ? 2u : ((lead & 0xF0u) == 0xE0u ? 3u :
            ((lead & 0xF8u) == 0xF0u ? 4u : 0u)));
        bool valid = expected != 0 && start + expected == end;
        for (std::size_t at = start + 1; valid && at < end; ++at)
            valid = IsUtf8Continuation(document.ByteAt(at));
        return valid ? start : end - 1;
    }

    // Shift-JIS trail bytes overlap valid single-byte values, so scan forward
    // from a known boundary instead of guessing backward from the final byte.
    std::size_t at = 0;
    std::size_t previous = end - 1;
    while (at < end) {
        previous = at;
        const std::size_t length = IsShiftJisLead(document.ByteAt(at)) && at + 1 < end &&
            IsShiftJisTrail(document.ByteAt(at + 1)) ? 2u : 1u;
        if (at + length >= end) return at;
        at += length;
    }
    return previous;
}
}

BinEditApp::BinEditApp(HINSTANCE instance) : instance_(instance), root_(this) {}

BinEditApp::~BinEditApp() {
    CancelAsyncSearch();
    StopRecoveryWriter();
}

// Application startup order matters: preferences must be resolved before menu
// creation, while the renderer must exist before the first R2PR request.
int BinEditApp::Run(int showCommand, std::span<const std::filesystem::path> initialFiles) {
    profile_.Load();
    resolvedLanguage_ = ResolveLanguage(profile_.language);
    searchDialog_.encoding = profile_.encoding;
    searchDialog_.language = resolvedLanguage_;
    document_.NewEmpty();
    tabs_.emplace_back();
    if (!CreateEditorWindow(showCommand, false)) return 1;
    if (!initialFiles.empty()) {
        OpenPath(initialFiles.front());
        for (const auto& path : initialFiles.subspan(1)) OpenPathInNewTab(path);
    }
    RestoreRecoverySessions();
    ScheduleRender(R2PRRegion::Full);

    MSG message{};
    for (;;) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status == 0) break;
        if (status == -1) return 1;
        // The main editor is a normal top-level window, not a dialog manager.
        // Sending its keyboard stream through IsDialogMessage can consume Tab,
        // arrows, and character translation before the editor sees them.
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

bool BinEditApp::CreateEditorWindow(int showCommand, bool detached) {
    // No class background brush is registered. Returning nonzero for
    // WM_ERASEBKGND and presenting through DXGI prevents GDI/D2D flicker.
    if (!detached && std::ranges::any_of(gEditorWindows,
        [](const BinEditApp* app) { return app && app->window_ && app->primaryWindow_; })) return false;
    primaryWindow_ = !detached;
    const wchar_t* className = detached ? BinEditIdentity::DetachedWindowClass : BinEditIdentity::MainWindowClass;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), (LPCWSTR)IDI_ICON1);
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = className;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    RECT desired{0, 0, 1040, 720};
    AdjustWindowRectExForDpi(&desired, WS_OVERLAPPEDWINDOW, TRUE, 0, 96);
    // A detached editor is still a normal movable top-level window, but making
    // the one primary HWND its Win32 owner keeps transient tab hosts out of the
    // taskbar's thumbnail registry. Besides matching the main/sub lifetime model,
    // this avoids accumulating Explorer-owned taskbar resources during repeated
    // detach/close cycles. No WS_EX_TOOLWINDOW style is used, so the normal frame,
    // activation behavior, DPI handling, and independent editor surface remain.
    const HWND owner = detached && root_ && root_ != this ? root_->window_ : nullptr;
    window_ = CreateWindowExW(0, className, L"BinEdit", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, desired.right - desired.left, desired.bottom - desired.top,
        owner, CreateMainMenu(), instance_, this);
    if (!window_) return false;
    // Keep UIPI's default integrity boundary. The receiver additionally checks
    // session, user SID, and token integrity before parsing any WM_COPYDATA bytes.
    DragAcceptFiles(window_, TRUE);
    if (!renderer_.Initialize(window_)) {
        ShowMessageDialog(window_, instance_, L"BinEdit", GetStrings(resolvedLanguage_).initializeFailed,
            MessageDialogButtons::Ok, MessageDialogIcon::Error, profile_, resolvedLanguage_);
        DestroyWindow(window_);
        return false;
    }
    gEditorWindows.push_back(this);
    ApplyTheme();
    UpdateTitle();
    UpdateScrollBar();
    // The timer only consumes already-signaled directory notifications in the
    // common path; it does not reread or restat large files every 750 ms.
    SetTimer(window_, kExternalChangeTimer, kExternalChangeIntervalMilliseconds, nullptr);
    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    // Explicit focus makes the client ready for immediate hexadecimal typing,
    // including when startup was initiated from a shell file association.
    SetFocus(window_);
    return true;
}

HMENU BinEditApp::CreateMainMenu() const {
    const auto& strings = GetStrings(resolvedLanguage_);
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuItem(file, IDM_FILE_NEW, strings.fileNew);
    AppendMenuItem(file, IDM_FILE_OPEN, strings.fileOpen);
    AppendMenuItem(file, IDM_FILE_SAVE, strings.fileSave);
    AppendMenuItem(file, IDM_FILE_SAVE_AS, strings.fileSaveAs);
    AppendMenuItem(file, IDM_FILE_CLOSE, strings.fileClose);
    AppendMenuItem(file, IDM_FILE_DETACH_TAB, strings.fileDetachTab);
    if (primaryWindow_) {
        // Process-level Exit belongs only to the main host. A sub window can
        // still be closed through its title bar or Alt+F4 without implying that
        // the complete application should terminate.
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuItem(file, IDM_FILE_EXIT, strings.fileExit);
    }
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), strings.menuFile);

    HMENU edit = CreatePopupMenu();
    AppendMenuItem(edit, IDM_EDIT_UNDO, strings.editUndo);
    AppendMenuItem(edit, IDM_EDIT_REDO, strings.editRedo);
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuItem(edit, IDM_EDIT_FIND, strings.editFind);
    AppendMenuItem(edit, IDM_EDIT_FIND_NEXT, strings.editFindNext);
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuItem(edit, IDM_EDIT_MARK, strings.editMark);
    AppendMenuItem(edit, IDM_EDIT_CLEAR_MARKS, strings.editClearMarks);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), strings.menuEdit);

    // Tool commands retain ordinary Win32 menu behavior while their client-area
    // interaction is rendered by the same themed D3D11/D2D1 dialog system.
    HMENU tools = CreatePopupMenu();
    AppendMenuItem(tools, IDM_TOOLS_BIT_OPERATION, strings.toolsBitOperation);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), strings.menuTools);

    // Encoding names are technical identifiers and intentionally identical in
    // Japanese and English UI modes.
    HMENU encoding = CreatePopupMenu();
    AppendMenuItem(encoding, IDM_ENCODING_ASCII, L"ASCII");
    AppendMenuItem(encoding, IDM_ENCODING_UTF8, L"UTF-8");
    AppendMenuItem(encoding, IDM_ENCODING_UTF16LE, L"UTF-16 LE");
    AppendMenuItem(encoding, IDM_ENCODING_SHIFTJIS, L"Shift-JIS");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(encoding), strings.menuEncoding);

    HMENU color = CreatePopupMenu();
    AppendMenuItem(color, IDM_COLOR_MARK, strings.colorMark);
    AppendMenuItem(color, IDM_COLOR_BYTE, strings.colorByte);
    AppendMenuItem(color, IDM_COLOR_CLEAR_BYTE, strings.colorClearByte);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(color), strings.menuColor);

    if (primaryWindow_) {
        // Theme and language are process-wide preferences. Only the main window
        // exposes their commands; committed changes continue to update every
        // sub window so visual state cannot diverge.
        HMENU theme = CreatePopupMenu();
        AppendMenuItem(theme, IDM_THEME_SYSTEM, strings.themeSystem);
        AppendMenuItem(theme, IDM_THEME_LIGHT, strings.themeLight);
        AppendMenuItem(theme, IDM_THEME_DARK, strings.themeDark);
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), strings.menuTheme);

        HMENU language = CreatePopupMenu();
        AppendMenuItem(language, IDM_LANGUAGE_SYSTEM, strings.languageSystem);
        AppendMenuItem(language, IDM_LANGUAGE_JAPANESE, strings.languageJapanese);
        AppendMenuItem(language, IDM_LANGUAGE_ENGLISH, strings.languageEnglish);
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(language), strings.menuLanguage);
    }

    HMENU help = CreatePopupMenu();
    AppendMenuItem(help, IDM_HELP_ABOUT, strings.helpAbout);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), strings.menuHelp);
    return bar;
}

void BinEditApp::UpdateMenuChecks() {
    // Menu state is refreshed lazily before a popup opens as well as after each
    // command, keeping Undo/Redo and Find Next availability authoritative.
    HMENU menu = GetMenu(window_);
    if (!menu) return;
    if (ioBusy_) {
        const int count = GetMenuItemCount(menu);
        for (int position = 0; position < count; ++position) {
            EnableMenuItem(menu, static_cast<UINT>(position),
                MF_BYPOSITION | MF_GRAYED);
        }
        DrawMenuBar(window_);
        return;
    }
    if (primaryWindow_) {
        CheckMenuRadioItem(menu, IDM_THEME_SYSTEM, IDM_THEME_DARK,
            profile_.theme == ThemePreference::Dark ? IDM_THEME_DARK : (profile_.theme == ThemePreference::Light ? IDM_THEME_LIGHT : IDM_THEME_SYSTEM), MF_BYCOMMAND);
        CheckMenuRadioItem(menu, IDM_LANGUAGE_SYSTEM, IDM_LANGUAGE_ENGLISH,
            profile_.language == LanguagePreference::Japanese ? IDM_LANGUAGE_JAPANESE :
            (profile_.language == LanguagePreference::English ? IDM_LANGUAGE_ENGLISH : IDM_LANGUAGE_SYSTEM), MF_BYCOMMAND);
    }
    CheckMenuRadioItem(menu, IDM_ENCODING_ASCII, IDM_ENCODING_SHIFTJIS,
        profile_.encoding == TextEncoding::Utf8 ? IDM_ENCODING_UTF8 : (profile_.encoding == TextEncoding::Utf16Le ? IDM_ENCODING_UTF16LE :
        (profile_.encoding == TextEncoding::ShiftJis ? IDM_ENCODING_SHIFTJIS : IDM_ENCODING_ASCII)), MF_BYCOMMAND);
    EnableMenuItem(menu, IDM_EDIT_UNDO, MF_BYCOMMAND | (document_.CanUndo() ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, IDM_EDIT_REDO, MF_BYCOMMAND | (document_.CanRedo() ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, IDM_EDIT_FIND_NEXT, MF_BYCOMMAND | (searchPattern_ ? MF_ENABLED : MF_GRAYED));
    // Virtual EOF is an insertion position rather than a stored byte range.
    // Keep the command state consistent with ToggleMarkup's authoritative guard.
    EnableMenuItem(menu, IDM_EDIT_MARK, MF_BYCOMMAND |
        (caret_ < document_.Size() ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, IDM_FILE_CLOSE, MF_BYCOMMAND |
        (CanCloseTab(activeTab_) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, IDM_FILE_DETACH_TAB, MF_BYCOMMAND |
        (CanDetachTab() ? MF_ENABLED : MF_GRAYED));
    // The virtual EOF cell is an insertion position, not a byte value. Both
    // value-color commands must therefore remain unavailable until the caret
    // addresses storage owned by the document.
    const UINT byteColorState = caret_ < document_.Size() ? MF_ENABLED : MF_GRAYED;
    EnableMenuItem(menu, IDM_COLOR_BYTE, MF_BYCOMMAND | byteColorState);
    EnableMenuItem(menu, IDM_COLOR_CLEAR_BYTE, MF_BYCOMMAND | byteColorState);
    // The calculator is intentionally independent from the document and caret.
    EnableMenuItem(menu, IDM_TOOLS_BIT_OPERATION, MF_BYCOMMAND | MF_ENABLED);
}

void BinEditApp::RebuildMenu() {
    // SetMenu transfers the new hierarchy to the HWND. The previous hierarchy
    // must then be destroyed explicitly to avoid leaking HMENU objects.
    HMENU previous = GetMenu(window_);
    HMENU replacement = CreateMainMenu();
    SetMenu(window_, replacement);
    if (previous) DestroyMenu(previous);
    DrawMenuBar(window_);
    UpdateMenuChecks();
}

void BinEditApp::UpdateTitle() {
    std::wstring title = document_.Path().empty() ? GetStrings(resolvedLanguage_).untitled : document_.Path().filename().wstring();
    if (document_.Dirty()) title += L" *";
    title += L" — BinEdit";
    SetWindowTextW(window_, title.c_str());
    if (document_.Dirty()) ScheduleRecoveryCheckpoint();
    else ClearRecoveryCheckpoint();
    UpdateMenuChecks();
    UpdateShutdownBlockReason();
    ScheduleRender(R2PRRegion::Tabs);
}

void BinEditApp::UpdateShutdownBlockReason() {
    if (!window_) return;
    if (!HasDirtyTabs()) {
        ClearShutdownBlockReason();
        return;
    }
    // Registering as soon as the document becomes dirty gives Windows a useful,
    // localized reason even if logoff reaches WM_QUERYENDSESSION immediately.
    if (ShutdownBlockReasonCreate(window_, GetStrings(resolvedLanguage_).shutdownBlockReason)) {
        shutdownBlockRegistered_ = true;
    }
}

void BinEditApp::ClearShutdownBlockReason() {
    if (!shutdownBlockRegistered_ || !window_) return;
    ShutdownBlockReasonDestroy(window_);
    shutdownBlockRegistered_ = false;
}

void BinEditApp::RestoreRecoverySessions() {
    for (auto& record : RecoveryStore::Enumerate()) {
        std::wstring prompt = GetStrings(resolvedLanguage_).recoveryPrompt;
        prompt += record.originalPath.empty() ? GetStrings(resolvedLanguage_).untitled :
            record.originalPath.wstring();
        const MessageDialogResult answer = ShowMessageDialog(window_, instance_,
            GetStrings(resolvedLanguage_).recoveryTitle, prompt, MessageDialogButtons::YesNo,
            MessageDialogIcon::Warning, profile_, resolvedLanguage_);
        if (answer == MessageDialogResult::No) {
            RecoveryStore::Remove(record.id);
            continue;
        }
        if (answer != MessageDialogResult::Yes) continue;

        EditorTabState state;
        state.document.RestoreSnapshot(std::move(record.bytes), record.originalPath);
        state.recoveryId = std::move(record.id);
        state.recoveryCheckpointTick = GetTickCount64();
        // Enumeration validated this exact on-disk record, so the restored
        // revision is already durable. Tab activation must not republish it
        // merely to move the recovered document into another hot slot.
        state.recoveryDurableRevision = state.document.Revision();
        const bool pristine = document_.Path().empty() && document_.Empty() && !document_.Dirty() &&
            !document_.CanUndo() && !document_.CanRedo();
        if (pristine) {
            ResetDocumentSession();
            RestoreActiveTab(std::move(state));
            RefreshAfterTabChange();
        } else {
            AddTab(std::move(state), activeTab_ + 1);
        }
    }
}

void BinEditApp::ScheduleRecoveryCheckpoint() {
    if (!window_ || !document_.Dirty()) return;
    bool firstCheckpoint = false;
    if (recoveryId_.empty()) {
        try { recoveryId_ = RecoveryStore::NewId(); } catch (...) { return; }
        if (recoveryId_.empty()) return;
        firstCheckpoint = true;
    }
    if (recoveryDurableRevision_ == document_.Revision()) return;
    const ULONGLONG now = GetTickCount64();
    if (firstCheckpoint || now - recoveryCheckpointTick_ >= kRecoveryMaximumIntervalMilliseconds) {
        StartRecoveryCheckpoint();
        return;
    }
    // Continuous input is bounded by the maximum interval above; ordinary bursts
    // collapse to one snapshot after the user pauses, avoiding write amplification.
    KillTimer(window_, kRecoveryTimer);
    if (!SetTimer(window_, kRecoveryTimer, kRecoveryIdleMilliseconds, nullptr)) {
        StartRecoveryCheckpoint();
    }
}

bool BinEditApp::StartRecoveryCheckpoint() {
    if (window_) KillTimer(window_, kRecoveryTimer);
    if (!document_.Dirty() || recoveryId_.empty() || document_.Revision() == 0) return false;
    StopRecoveryWriter();
    const std::uint64_t revision = document_.Revision();
    if (recoveryDurableRevision_ == revision) return true;
    ImmutableByteSnapshot snapshot;
    std::filesystem::path originalPath;
    std::wstring id;
    try {
        snapshot = document_.CreateSnapshot();
        originalPath = document_.Path();
        id = recoveryId_;
    } catch (...) { return false; }
    recoveryCheckpointTick_ = GetTickCount64();
    std::shared_ptr<RecoveryWriteCompletion> completion;
    try {
        completion = std::make_shared<RecoveryWriteCompletion>();
        completion->revision = revision;
    } catch (...) { return false; }
    recoveryCompletion_ = completion;
    try {
        recoveryWorker_ = std::jthread([snapshot = std::move(snapshot),
            originalPath = std::move(originalPath), id = std::move(id),
            completion = std::move(completion)](std::stop_token stopToken) {
            SetThreadDescription(GetCurrentThread(), L"BinEdit recovery writer");
            DWORD ignored{};
            completion->succeeded = RecoveryStore::Write(
                id, originalPath, snapshot, stopToken, ignored);
        });
    } catch (...) {
        recoveryCompletion_.reset();
        return false;
    }
    return true;
}

bool BinEditApp::EnsureRecoveryCheckpoint() {
    if (!document_.Dirty()) return true;
    if (recoveryId_.empty()) {
        try { recoveryId_ = RecoveryStore::NewId(); } catch (...) { return false; }
        if (recoveryId_.empty()) return false;
    }
    if (window_) KillTimer(window_, kRecoveryTimer);
    const std::uint64_t revision = document_.Revision();
    if (recoveryDurableRevision_ == revision) return true;
    bool checkpointSucceeded = false;
    DWORD error{};
    static_cast<void>(RunIoOperation(GetStrings(resolvedLanguage_).statusSavingFile,
        [this, revision, &checkpointSucceeded](DWORD& workerError) {
            // Joining on this worker keeps a previous recovery flush from
            // stalling the UI thread before the progress surface appears.
            JoinRecoveryWriter(true);
            if (recoveryDurableRevision_ == revision) {
                checkpointSucceeded = true;
                workerError = ERROR_SUCCESS;
                return true;
            }
            checkpointSucceeded = RecoveryStore::Write(recoveryId_, document_.Path(),
                document_.CreateSnapshot(), {}, workerError);
            return checkpointSucceeded;
        }, error));
    if (checkpointSucceeded) {
        recoveryDurableRevision_ = revision;
        recoveryCheckpointTick_ = GetTickCount64();
    }
    return checkpointSucceeded;
}

void BinEditApp::JoinRecoveryWriter(bool requestStop) {
    if (!recoveryWorker_.joinable()) {
        recoveryCompletion_.reset();
        return;
    }
    if (requestStop) recoveryWorker_.request_stop();
    recoveryWorker_.join();
    if (recoveryCompletion_ && recoveryCompletion_->succeeded) {
        recoveryDurableRevision_ = recoveryCompletion_->revision;
    }
    recoveryCompletion_.reset();
}

void BinEditApp::StopRecoveryWriter() {
    if (window_) KillTimer(window_, kRecoveryTimer);
    JoinRecoveryWriter(true);
}

void BinEditApp::ClearRecoveryCheckpoint() {
    if (window_) KillTimer(window_, kRecoveryTimer);
    StopRecoveryWriter();
    if (!recoveryId_.empty()) RecoveryStore::Remove(recoveryId_);
    recoveryId_.clear();
    recoveryCheckpointTick_ = 0;
    recoveryDurableRevision_ = 0;
    recoveryCompletion_.reset();
}

void BinEditApp::UpdateScrollBar() {
    const std::size_t rows = RowCount(document_.Size());
    const std::size_t visible = renderer_.Metrics().visibleRows;
    const std::size_t maximumFirst = rows > visible ? rows - visible : 0;
    firstRow_ = std::min(firstRow_, maximumFirst);
    const bool changed = !scrollBarModelValid_ || scrollBarModelRows_ != rows ||
        scrollBarModelVisible_ != visible || scrollBarModelFirst_ != firstRow_;
    scrollBarModelRows_ = rows;
    scrollBarModelVisible_ = visible;
    scrollBarModelFirst_ = firstRow_;
    scrollBarModelValid_ = true;
    if (changed) ScheduleRender(R2PRRegion::ScrollBar);
}

void BinEditApp::ScrollToFirstRow(std::size_t row) {
    const std::size_t rows = RowCount(document_.Size());
    const std::size_t visible = renderer_.Metrics().visibleRows;
    const std::size_t maximumFirst = rows > visible ? rows - visible : 0;
    const std::size_t clamped = std::min(row, maximumFirst);
    if (clamped == firstRow_) return;
    firstRow_ = clamped;
    UpdateScrollBar();
    ScheduleRender(R2PRRegion::Data);
}

void BinEditApp::SetScrollBarHovered(bool hovered) {
    if (scrollBarHovered_ == hovered) return;
    scrollBarHovered_ = hovered;
    ScheduleRender(R2PRRegion::ScrollBar);
}

void BinEditApp::UpdateTabHover(std::optional<std::size_t> index) {
    if (index && *index >= tabs_.size()) index.reset();
    if (tabHoverCandidate_ == index) return;

    if (window_) KillTimer(window_, kTabTooltipTimer);
    const bool tooltipWasVisible = tabTooltipTab_.has_value();
    tabTooltipTab_.reset();
    tabHoverCandidate_ = index;
    if (tabHoverCandidate_ && window_ && !tabDragging_) {
        // Delay matches conventional desktop tooltips while keeping hover motion
        // render-free. WM_TIMER merely publishes the candidate into RenderModel.
        SetTimer(window_, kTabTooltipTimer, kTabTooltipDelayMilliseconds, nullptr);
    }
    if (tooltipWasVisible) ScheduleRender(R2PRRegion::Full);
}

void BinEditApp::EnsureActiveTabVisible() {
    if (tabs_.empty()) {
        firstVisibleTab_ = 0;
        return;
    }
    const std::size_t maximumFirst = renderer_.MaximumFirstVisibleTab(tabs_.size());
    firstVisibleTab_ = std::min(firstVisibleTab_, maximumFirst);
    if (activeTab_ < firstVisibleTab_) {
        firstVisibleTab_ = activeTab_;
        return;
    }
    // maximumFirst is derived from the number of complete tabs that fit. Moving
    // the viewport just far enough keeps keyboard and programmatic tab changes
    // visible without unnecessarily snapping all the way to the right edge.
    const std::size_t visibleCapacity = tabs_.size() - maximumFirst;
    if (activeTab_ - firstVisibleTab_ >= visibleCapacity) {
        firstVisibleTab_ = activeTab_ - visibleCapacity + 1;
    }
}

void BinEditApp::ScrollTabStrip(int tabDelta) {
    const std::size_t maximumFirst = renderer_.MaximumFirstVisibleTab(tabs_.size());
    const std::size_t previous = firstVisibleTab_;
    firstVisibleTab_ = std::min(firstVisibleTab_, maximumFirst);
    if (tabDelta < 0) {
        const std::size_t amount = static_cast<std::size_t>(-static_cast<long long>(tabDelta));
        firstVisibleTab_ -= std::min(firstVisibleTab_, amount);
    } else if (tabDelta > 0) {
        const std::size_t amount = static_cast<std::size_t>(tabDelta);
        firstVisibleTab_ += std::min(maximumFirst - firstVisibleTab_, amount);
    }
    if (previous != firstVisibleTab_) {
        UpdateTabHover(std::nullopt);
        ScheduleRender(R2PRRegion::Tabs);
    }
}

void BinEditApp::StartAutoScroll(POINT anchor) {
    if (autoScrollActive_ || dragging_ || tabDragging_ || scrollBarDragging_) return;
    const std::size_t rows = RowCount(document_.Size());
    if (rows <= renderer_.Metrics().visibleRows ||
        !renderer_.IsInDataViewport(static_cast<float>(anchor.x), static_cast<float>(anchor.y))) return;
    if (!SetTimer(window_, kAutoScrollTimer, kAutoScrollIntervalMilliseconds, nullptr)) return;

    autoScrollActive_ = true;
    autoScrollCursorState_ = AutoScrollCursorState::None;
    autoScrollAnchor_ = autoScrollPointer_ = anchor;
    autoScrollLastTick_ = GetTickCount64();
    autoScrollRowAccumulator_ = 0.0L;
    SetCapture(window_);
    UpdateAutoScrollCursor();
    ScheduleRenderRect(renderer_.AutoScrollMarkerBounds(
        static_cast<float>(anchor.x), static_cast<float>(anchor.y)));
}

void BinEditApp::StopAutoScroll() {
    if (!autoScrollActive_) return;
    const RECT marker = renderer_.AutoScrollMarkerBounds(
        static_cast<float>(autoScrollAnchor_.x), static_cast<float>(autoScrollAnchor_.y));
    autoScrollActive_ = false;
    autoScrollCursorState_ = AutoScrollCursorState::None;
    autoScrollRowAccumulator_ = 0.0L;
    KillTimer(window_, kAutoScrollTimer);
    if (GetCapture() == window_) ReleaseCapture();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (window_) ScheduleRenderRect(marker);
}

BinEditApp::AutoScrollCursorState BinEditApp::CurrentAutoScrollCursorState() const noexcept {
    if (!autoScrollActive_) return AutoScrollCursorState::None;
    const float delta = static_cast<float>(autoScrollPointer_.y - autoScrollAnchor_.y);
    const float deadZone = std::max(8.0f, renderer_.Metrics().rowHeight * 0.5f);
    if (delta < -deadZone) return AutoScrollCursorState::Up;
    if (delta > deadZone) return AutoScrollCursorState::Down;
    return AutoScrollCursorState::Stationary;
}

void BinEditApp::UpdateAutoScrollCursor(bool force) {
    const AutoScrollCursorState next = CurrentAutoScrollCursorState();
    if (next == AutoScrollCursorState::None || (!force && next == autoScrollCursorState_)) return;

    WORD resource = kPanMiddleCursorResource;
    if (next == AutoScrollCursorState::Up) resource = kPanNorthCursorResource;
    else if (next == AutoScrollCursorState::Down) resource = kPanSouthCursorResource;

    // LoadCursor returns a shared handle that must never be destroyed. Older or
    // restricted shells may omit the auto-pan resources, so retain a usable
    // directional cursor instead of allowing a null cursor to hide the pointer.
    HCURSOR cursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(resource));
    if (!cursor) cursor = LoadCursorW(nullptr,
        next == AutoScrollCursorState::Stationary ? IDC_SIZEALL : IDC_SIZENS);
    if (cursor) SetCursor(cursor);
    autoScrollCursorState_ = next;
}

void BinEditApp::TickAutoScroll() {
    if (!autoScrollActive_) return;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsedMilliseconds = std::min<ULONGLONG>(now - autoScrollLastTick_, 250);
    autoScrollLastTick_ = now;
    if (elapsedMilliseconds == 0) return;

    const AutoScrollCursorState cursorState = CurrentAutoScrollCursorState();
    const float delta = static_cast<float>(autoScrollPointer_.y - autoScrollAnchor_.y);
    const float deadZone = std::max(8.0f, renderer_.Metrics().rowHeight * 0.5f);
    const float excess = std::abs(delta) - deadZone;
    if (cursorState == AutoScrollCursorState::Stationary || excess <= 0.0f) {
        autoScrollRowAccumulator_ = 0.0L;
        return;
    }

    // Quadratic acceleration makes nearby movement controllable while allowing
    // a pointer near or outside the window edge to traverse very large files.
    const long double rowUnits = excess / std::max(1.0f, renderer_.Metrics().rowHeight);
    const long double rowsPerSecond = std::min<long double>(12000.0L,
        18.0L + rowUnits * rowUnits * 90.0L);
    const long double direction = cursorState == AutoScrollCursorState::Up ? -1.0L : 1.0L;
    autoScrollRowAccumulator_ += direction * rowsPerSecond *
        static_cast<long double>(elapsedMilliseconds) / 1000.0L;
    const auto wholeRows = static_cast<std::ptrdiff_t>(std::trunc(autoScrollRowAccumulator_));
    if (wholeRows == 0) return;
    autoScrollRowAccumulator_ -= static_cast<long double>(wholeRows);

    const std::size_t previous = firstRow_;
    if (wholeRows < 0) {
        const std::size_t amount = static_cast<std::size_t>(-wholeRows);
        ScrollToFirstRow(firstRow_ - std::min(firstRow_, amount));
    } else {
        const std::size_t amount = static_cast<std::size_t>(wholeRows);
        ScrollToFirstRow(firstRow_ + amount);
    }
    if (firstRow_ == previous) autoScrollRowAccumulator_ = 0.0L;
}

void BinEditApp::ScheduleRender(R2PRRegion region, std::optional<std::size_t> byteOffset) {
    // Post instead of SendMessage: consecutive input messages can merge their
    // damage before the queued frame is consumed, which is essential at 240 Hz.
    renderer_.Require(region, byteOffset, firstRow_);
    if (!renderMessageQueued_ && renderer_.HasPendingFrame() && window_) {
        renderMessageQueued_ = PostMessageW(window_, WM_APP_R2PR_RENDER, 0, 0) != FALSE;
    }
}

void BinEditApp::ScheduleRenderRect(RECT rect) {
    renderer_.RequireRect(rect);
    if (!renderMessageQueued_ && renderer_.HasPendingFrame()) renderMessageQueued_ = PostMessageW(window_, WM_APP_R2PR_RENDER, 0, 0) != FALSE;
}

void BinEditApp::SetIoBusy(bool busy, const wchar_t* progressText) {
    ioBusy_ = busy;
    if (busy) {
        CancelAsyncSearch();
        StopAutoScroll();
        UpdateTabHover(std::nullopt);
        ioProgressPhase_ = 0.0f;
        ioProgressText_ = progressText ? progressText : L"";
        ClearShutdownBlockReason();
        if (window_ && !ioProgressText_.empty() &&
            ShutdownBlockReasonCreate(window_, ioProgressText_.c_str())) {
            shutdownBlockRegistered_ = true;
        }
        SetTimer(window_, kIoProgressTimer, kIoProgressIntervalMilliseconds, nullptr);
    } else {
        KillTimer(window_, kIoProgressTimer);
        ioProgressPhase_ = 0.0f;
        ioProgressText_.clear();
        ClearShutdownBlockReason();
    }

    HMENU menu = GetMenu(window_);
    if (menu) {
        const int count = GetMenuItemCount(menu);
        for (int position = 0; position < count; ++position) {
            EnableMenuItem(menu, static_cast<UINT>(position), MF_BYPOSITION |
                (busy ? MF_GRAYED : MF_ENABLED));
        }
        DrawMenuBar(window_);
    }
    if (!busy) {
        if (ioTitleRefreshPending_) {
            ioTitleRefreshPending_ = false;
            UpdateTitle();
        } else {
            UpdateMenuChecks();
            UpdateShutdownBlockReason();
        }
    }
    ScheduleRender(R2PRRegion::Full);
}

bool BinEditApp::RunIoOperation(const wchar_t* progressText,
                                std::function<bool(DWORD&)> operation,
                                DWORD& error) {
    if (ioBusy_ || !window_ || !operation) {
        error = ERROR_BUSY;
        return false;
    }

    HANDLE completionEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completionEvent) {
        error = GetLastError();
        return false;
    }

    bool succeeded = false;
    DWORD workerError = ERROR_SUCCESS;
    SetIoBusy(true, progressText);
    std::jthread worker;
    try {
        worker = std::jthread([operation = std::move(operation), &succeeded,
            &workerError, completionEvent, wakeWindow = window_](std::stop_token) mutable {
            SetThreadDescription(GetCurrentThread(), L"BinEdit file I/O");
            try {
                succeeded = operation(workerError);
            } catch (...) {
                succeeded = false;
                workerError = ERROR_NOT_ENOUGH_MEMORY;
            }
            SetEvent(completionEvent);
            // The event is authoritative. This message only wakes or annotates
            // UI automation that observes the ordinary window-message stream.
            PostMessageW(wakeWindow, WM_APP_IO_COMPLETE, 0, 0);
        });
    } catch (...) {
        CloseHandle(completionEvent);
        SetIoBusy(false);
        error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }

    bool repostQuit = false;
    WPARAM quitCode = 0;
    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjectsEx(1, &completionEvent, INFINITE,
            QS_ALLINPUT, MWMO_INPUTAVAILABLE | MWMO_ALERTABLE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) break;
        if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_IO_COMPLETION) continue;

        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                repostQuit = true;
                quitCode = message.wParam;
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    worker.join();
    CloseHandle(completionEvent);
    error = workerError;
    SetIoBusy(false);
    if (!pendingOpenPaths_.empty()) {
        PostMessageW(window_, WM_APP_PROCESS_PENDING_OPEN, 0, 0);
    }
    if (repostQuit) PostQuitMessage(static_cast<int>(quitCode));
    return succeeded;
}

bool BinEditApp::LoadDocumentWithProgress(const std::filesystem::path& path,
                                          ByteDocument& destination, DWORD& error) {
    std::filesystem::path source;
    try { source = path; }
    catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    return RunIoOperation(GetStrings(resolvedLanguage_).statusLoadingFile,
        [&destination, source = std::move(source)](DWORD& workerError) {
            return destination.Load(source, workerError);
        }, error);
}

void BinEditApp::QueueOpenPaths(std::vector<std::filesystem::path>&& paths) {
    try {
        for (auto& path : paths) {
            if (pendingOpenPaths_.size() >= 256) break;
            pendingOpenPaths_.push_back(std::move(path));
        }
    } catch (...) {
        // A memory-pressure failure leaves already queued, fully owned paths
        // intact. No partially constructed filesystem path is ever processed.
    }
    if (!ioBusy_ && !pendingOpenPaths_.empty() && window_) {
        PostMessageW(window_, WM_APP_PROCESS_PENDING_OPEN, 0, 0);
    }
}

void BinEditApp::ProcessPendingOpenPaths() {
    if (ioBusy_) return;
    while (!pendingOpenPaths_.empty() && window_) {
        std::filesystem::path path = std::move(pendingOpenPaths_.front());
        pendingOpenPaths_.pop_front();
        const bool pristine = document_.Path().empty() && document_.Empty() &&
            !document_.Dirty() && !document_.CanUndo() && !document_.CanRedo();
        if (pristine) OpenPath(path);
        else OpenPathInNewTab(path);
    }
}

RenderModel BinEditApp::MakeRenderModel() const {
    // Spans borrow vectors owned by this controller. Render executes immediately
    // on the same UI thread, so no synchronization or deep copy is required.
    RenderModel model;
    model.profile = &profile_;
    model.language = resolvedLanguage_;
    if (ioBusy_) {
        // The worker may be mutating or replacing ByteDocument storage. The
        // progress surface deliberately contains no span or title borrowed from
        // that document, eliminating UI/worker data races during rendering.
        model.ioBusy = true;
        model.ioProgressPhase = ioProgressPhase_;
        model.ioProgressText = ioProgressText_;
        return model;
    }
    model.document = &document_; model.firstRow = firstRow_; model.caret = caret_;
    model.selectionAnchor = selectionAnchor_; model.selectionActive = selectionActive_;
    // A responsive layout may hide the character pane. Never report an invisible
    // input target to the renderer or keyboard path.
    model.hexPane = hexPane_ || !renderer_.Metrics().textPaneVisible;
    model.insertMode = insertMode_;
    model.searchInProgress = searchInProgress_;
    model.scrollBarHovered = scrollBarHovered_;
    model.scrollBarDragging = scrollBarDragging_;
    model.autoScrollActive = autoScrollActive_;
    model.autoScrollAnchorX = static_cast<float>(autoScrollAnchor_.x);
    model.autoScrollAnchorY = static_cast<float>(autoScrollAnchor_.y);
    model.dirty = document_.Dirty(); model.encoding = profile_.encoding;
    model.fileName = document_.Path().empty() ? GetStrings(resolvedLanguage_).untitled : document_.Path().filename().wstring();
    model.searchOffsets = std::span<const std::size_t>(searchOffsets_); model.searchLength = searchPattern_ ? searchPattern_->bytes.size() : 0;
    model.markups = std::span<const MarkupRange>(markups_);
    model.activeTab = activeTab_;
    model.firstVisibleTab = firstVisibleTab_;
    model.tabTooltip = tabTooltipTab_;
    model.tabs.reserve(tabs_.size());
    const auto& strings = GetStrings(resolvedLanguage_);
    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        const ByteDocument& tabDocument = index == activeTab_ ? document_ : tabs_[index].document;
        model.tabs.push_back({tabDocument.Path().empty() ? strings.untitled : tabDocument.Path().filename().wstring(),
            tabDocument.Dirty()});
    }
    return model;
}

BinEditApp::EditorTabState BinEditApp::CaptureActiveTab() {
    // Search workers are window-owned, whereas search patterns/results are
    // document-owned. Cancel before moving the document state to another slot or
    // controller so a completion can never target the wrong active tab.
    CancelAsyncSearch();
    if (document_.Dirty() && !recoveryId_.empty()) {
        static_cast<void>(EnsureRecoveryCheckpoint());
    }
    EditorTabState state;
    state.document = std::move(document_);
    state.caret = caret_;
    state.selectionAnchor = selectionAnchor_;
    state.selectionActive = selectionActive_;
    state.firstRow = firstRow_;
    state.hexPane = hexPane_;
    state.lowNibble = lowNibble_;
    state.insertMode = insertMode_;
    state.markups = std::move(markups_);
    state.searchPattern = std::move(searchPattern_);
    state.searchOffsets = std::move(searchOffsets_);
    state.searchResultsStale = searchResultsStale_;
    state.recoveryId = std::move(recoveryId_);
    state.recoveryCheckpointTick = recoveryCheckpointTick_;
    state.recoveryDurableRevision = recoveryDurableRevision_;
    recoveryCheckpointTick_ = 0;
    recoveryDurableRevision_ = 0;
    return state;
}

void BinEditApp::RestoreActiveTab(EditorTabState&& state) {
    document_ = std::move(state.document);
    caret_ = state.caret;
    selectionAnchor_ = state.selectionAnchor;
    selectionActive_ = state.selectionActive;
    firstRow_ = state.firstRow;
    hexPane_ = state.hexPane;
    lowNibble_ = state.lowNibble;
    insertMode_ = state.insertMode;
    markups_ = std::move(state.markups);
    searchPattern_ = std::move(state.searchPattern);
    searchOffsets_ = std::move(state.searchOffsets);
    searchResultsStale_ = state.searchResultsStale;
    searchInProgress_ = false;
    recoveryId_ = std::move(state.recoveryId);
    recoveryCheckpointTick_ = state.recoveryCheckpointTick;
    recoveryDurableRevision_ = state.recoveryDurableRevision;
    dragging_ = false;
}

void BinEditApp::RefreshAfterTabChange() {
    UpdateTabHover(std::nullopt);
    caret_ = std::min(caret_, document_.Size());
    selectionAnchor_ = std::min(selectionAnchor_, document_.Size());
    selectionActive_ = std::min(selectionActive_, document_.Size());
    EnsureActiveTabVisible();
    UpdateTitle();
    UpdateScrollBar();
    ScheduleRender(R2PRRegion::Full);
    if (searchResultsStale_ && searchPattern_) QueueSearchRefresh();
    if (window_) SetFocus(window_);
}

void BinEditApp::SwitchTab(std::size_t index) {
    if (index >= tabs_.size() || index == activeTab_) return;
    tabs_[activeTab_] = CaptureActiveTab();
    EditorTabState next = std::move(tabs_[index]);
    activeTab_ = index;
    RestoreActiveTab(std::move(next));
    RefreshAfterTabChange();
}

void BinEditApp::CreateNewTab() {
    tabs_[activeTab_] = CaptureActiveTab();
    activeTab_ = tabs_.size();
    tabs_.emplace_back();
    RestoreActiveTab(EditorTabState{});
    RefreshAfterTabChange();
}

void BinEditApp::AddTab(EditorTabState&& state, std::size_t index) {
    index = std::min(index, tabs_.size());
    if (!tabs_.empty()) tabs_[activeTab_] = CaptureActiveTab();
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(index), std::move(state));
    activeTab_ = index;
    EditorTabState selected = std::move(tabs_[activeTab_]);
    RestoreActiveTab(std::move(selected));
    RefreshAfterTabChange();
}

BinEditApp::EditorTabState BinEditApp::ExtractActiveTab() {
    EditorTabState extracted = CaptureActiveTab();
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(activeTab_));
    if (!tabs_.empty()) {
        activeTab_ = std::min(activeTab_, tabs_.size() - 1);
        EditorTabState selected = std::move(tabs_[activeTab_]);
        RestoreActiveTab(std::move(selected));
    } else {
        activeTab_ = 0;
    }
    return extracted;
}

void BinEditApp::ReorderActiveTab(std::size_t index) {
    if (tabs_.size() < 2) return;
    // TabDropIndex returns a gap in the original sequence, including the gap
    // after the final tab. Removing an item left of that gap shifts the eventual
    // insertion position one place to the left.
    index = std::min(index, tabs_.size());
    if (index > activeTab_) --index;
    if (index == activeTab_) return;
    tabs_[activeTab_] = CaptureActiveTab();
    EditorTabState moved = std::move(tabs_[activeTab_]);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(activeTab_));
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
    activeTab_ = index;
    EditorTabState selected = std::move(tabs_[activeTab_]);
    RestoreActiveTab(std::move(selected));
    RefreshAfterTabChange();
}

bool BinEditApp::CloseTab(std::size_t index) {
    if (index >= tabs_.size()) return false;
    if (!CanCloseTab(index)) return false;
    SwitchTab(index);
    if (!ConfirmDiscard()) return false;
    if (tabs_.size() == 1) {
        // The primary HWND is the permanent application anchor, so closing its
        // final real document returns it to a pristine Untitled session. A sub
        // window exists only to host detached tabs; closing its final tab closes
        // that host instead of manufacturing a second application anchor.
        if (!primaryWindow_) {
            DestroyWindow(window_);
            return true;
        }
        document_.NewEmpty();
        ResetDocumentSession();
    } else {
        static_cast<void>(ExtractActiveTab());
    }
    RefreshAfterTabChange();
    return true;
}

bool BinEditApp::CanCloseTab(std::size_t index) const noexcept {
    return TabPolicy::CanClose({tabs_.size(), activeTab_, primaryWindow_,
        !document_.Path().empty(), document_.Dirty()}, index);
}

bool BinEditApp::CanDetachTab() const noexcept {
    return TabPolicy::CanDetach({tabs_.size(), activeTab_, primaryWindow_,
        !document_.Path().empty(), document_.Dirty()});
}

bool BinEditApp::HasDirtyTabs() const {
    // A document owned by the file worker must not be inspected from the UI
    // thread. Treat the host as non-closable until the operation completes.
    if (ioBusy_) return true;
    if (document_.Dirty()) return true;
    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        if (index != activeTab_ && tabs_[index].document.Dirty()) return true;
    }
    return false;
}

bool BinEditApp::ConfirmAllTabs() {
    const std::size_t count = tabs_.size();
    for (std::size_t index = 0; index < count; ++index) {
        SwitchTab(index);
        if (!ConfirmDiscard()) return false;
    }
    return true;
}

bool BinEditApp::ConfirmAllWindows() {
    // Copy the registry because a successful Save may enter the elevation path
    // and synchronously destroy one or more HWNDs while confirmation is active.
    const std::vector<BinEditApp*> windows = gEditorWindows;
    for (BinEditApp* app : windows) {
        if (!app || !app->window_) continue;
        if (app->ioBusy_) return false;
        if (!app->ConfirmAllTabs()) return false;
    }
    return true;
}

void BinEditApp::DestroyAllWindows() {
    // Destroy sub windows first so their worker threads and shutdown-block
    // registrations are released while the main HWND still anchors the process.
    const std::vector<BinEditApp*> windows = gEditorWindows;
    for (BinEditApp* app : windows) {
        if (app && app != this && app->window_) DestroyWindow(app->window_);
    }
    if (window_) DestroyWindow(window_);
}

void BinEditApp::CollectClosedDetachedHosts() {
    // This method runs only from the primary controller after the detached
    // window procedure has returned from WM_DESTROY. Erasing here promptly
    // releases the closed document, undo history, D3D/DXGI resources, and all
    // other per-window allocations without deleting a live window procedure.
    if (!primaryWindow_) return;
    std::erase_if(detachedWindows_, [](const std::unique_ptr<BinEditApp>& child) {
        return child && child->window_ == nullptr;
    });
}

BinEditApp* BinEditApp::CreateDetachedHost(POINT screenPoint) {
    // Also collect after a failed PostMessage or a root message backlog before
    // adding another potentially resource-intensive detached renderer.
    root_->CollectClosedDetachedHosts();
    auto child = std::make_unique<BinEditApp>(instance_);
    child->root_ = root_;
    child->profile_ = profile_;
    child->resolvedLanguage_ = resolvedLanguage_;
    child->searchDialog_.encoding = profile_.encoding;
    child->searchDialog_.language = resolvedLanguage_;
    child->document_.NewEmpty();
    child->tabs_.emplace_back();
    if (!child->CreateEditorWindow(SW_SHOWNORMAL, true)) return nullptr;

    RECT sourceRect{};
    GetWindowRect(window_, &sourceRect);
    const int width = std::max(720L, sourceRect.right - sourceRect.left);
    const int height = std::max(420L, sourceRect.bottom - sourceRect.top);
    SetWindowPos(child->window_, nullptr, screenPoint.x - 80, screenPoint.y - 20,
        width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    BinEditApp* result = child.get();
    root_->detachedWindows_.push_back(std::move(child));
    return result;
}

void BinEditApp::DetachActiveTab(POINT screenPoint) {
    if (!CanDetachTab()) return;
    BinEditApp* child = CreateDetachedHost(screenPoint);
    if (!child) return;
    EditorTabState state = ExtractActiveTab();
    child->RestoreActiveTab(std::move(state));
    child->RefreshAfterTabChange();
    if (tabs_.empty()) DestroyWindow(window_);
    else RefreshAfterTabChange();
    // Source refresh calls SetFocus for keyboard continuity. Activate the new
    // host only after that refresh so focus cannot jump back to the parent.
    SetActiveWindow(child->window_);
    SetForegroundWindow(child->window_);
    SetFocus(child->window_);
}

void BinEditApp::DockActiveTabTo(BinEditApp& target, std::size_t index) {
    if (&target == this) {
        ReorderActiveTab(index);
        return;
    }
    EditorTabState state = ExtractActiveTab();
    target.AddTab(std::move(state), index);
    if (tabs_.empty()) DestroyWindow(window_);
    else RefreshAfterTabChange();
    SetActiveWindow(target.window_);
    SetForegroundWindow(target.window_);
    SetFocus(target.window_);
}

BinEditApp* BinEditApp::WindowAtPoint(POINT screenPoint) {
    HWND hit = WindowFromPoint(screenPoint);
    if (hit) hit = GetAncestor(hit, GA_ROOT);
    auto found = std::find_if(gEditorWindows.begin(), gEditorWindows.end(),
        [hit](const BinEditApp* app) { return app && app->window_ == hit; });
    if (found != gEditorWindows.end()) return *found;
    // Hidden automation windows and partially occluded hosts may not be returned
    // by WindowFromPoint. The process registry provides a deterministic fallback.
    found = std::find_if(gEditorWindows.begin(), gEditorWindows.end(),
        [screenPoint](const BinEditApp* app) {
            RECT rect{};
            return app && app->window_ && GetWindowRect(app->window_, &rect) && PtInRect(&rect, screenPoint);
        });
    return found == gEditorWindows.end() ? nullptr : *found;
}

bool BinEditApp::ConfirmDiscard() {
    if (!document_.Dirty()) return true;
    const MessageDialogResult answer = ShowMessageDialog(window_, instance_, L"BinEdit",
        GetStrings(resolvedLanguage_).saveChanges, MessageDialogButtons::YesNoCancel,
        MessageDialogIcon::Question, profile_, resolvedLanguage_);
    if (answer == MessageDialogResult::Cancel || answer == MessageDialogResult::None) return false;
    if (answer == MessageDialogResult::Yes) {
        const bool saved = Save(false);
        // An elevated handoff destroys this process's HWND. Returning false keeps
        // Open/Close callers from continuing a second workflow on that dead HWND.
        return saved && !closingForElevation_;
    }
    // An explicit No is an intentional discard, not an abnormal termination.
    // Marking the in-memory document clean prevents a following tab switch from
    // immediately recreating the recovery record that was just removed.
    ClearRecoveryCheckpoint();
    document_.MarkClean();
    return true;
}

void BinEditApp::ResetDocumentSession() {
    CancelAsyncSearch();
    caret_ = selectionAnchor_ = selectionActive_ = firstRow_ = 0;
    hexPane_ = true;
    lowNibble_ = false;
    insertMode_ = true;
    dragging_ = false;
    // This path runs when a file is closed or replaced. Release capacities, not
    // just elements, so a large markup/search session cannot remain charged to
    // the new Untitled document.
    std::vector<MarkupRange>{}.swap(markups_);
    std::vector<std::size_t>{}.swap(searchOffsets_);
    searchPattern_.reset();
    searchResultsStale_ = false;
}

void BinEditApp::OpenDialog() {
    // Long-path capacity belongs on the heap so a file dialog cannot consume
    // most of the UI thread's reserved stack.
    std::vector<wchar_t> path(32768, L'\0');
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_; dialog.lpstrFile = path.data(); dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrFilter = resolvedLanguage_ == UiLanguage::Japanese ? L"すべてのファイル\0*.*\0\0" : L"All files\0*.*\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) return;
    // Reuse a pristine placeholder; otherwise opening creates a peer tab and
    // leaves every existing document and its history untouched.
    const bool pristine = document_.Path().empty() && document_.Empty() && !document_.Dirty() &&
        !document_.CanUndo() && !document_.CanRedo();
    if (pristine) OpenPath(path.data());
    else OpenPathInNewTab(path.data());
}

bool BinEditApp::OpenPath(const std::filesystem::path& path) {
    ByteDocument loaded;
    DWORD error{};
    if (!LoadDocumentWithProgress(path, loaded, error)) {
        ShowFileError(GetStrings(resolvedLanguage_).openFailed, error);
        return false;
    }
    document_ = std::move(loaded);
    // View-specific annotations and search highlights are not transferable to a
    // different byte sequence, so a successful load resets the entire session.
    ResetDocumentSession();
    UpdateTitle(); UpdateScrollBar(); ScheduleRender(R2PRRegion::Full);
    return true;
}

bool BinEditApp::OpenPathInNewTab(const std::filesystem::path& path) {
    EditorTabState state;
    DWORD error{};
    if (!LoadDocumentWithProgress(path, state.document, error)) {
        ShowFileError(GetStrings(resolvedLanguage_).openFailed, error);
        return false;
    }
    AddTab(std::move(state), activeTab_ + 1);
    return true;
}

BinEditApp::ExternalChangeResolution BinEditApp::ResolveActiveExternalChange(
    ExternalFileState state, DWORD queryError) {
    if (state == ExternalFileState::Unchanged) return ExternalChangeResolution::None;
    if (state == ExternalFileState::Unavailable) {
        // An editor may briefly hold a deny-share handle while publishing. The
        // sticky model state retries after the next notification/timer cycle.
        static_cast<void>(queryError);
        return ExternalChangeResolution::Failed;
    }

    const auto& strings = GetStrings(resolvedLanguage_);
    if (state == ExternalFileState::Missing) {
        ShowMessageDialog(window_, instance_, L"BinEdit", strings.externalFileMissing,
            MessageDialogButtons::Ok, MessageDialogIcon::Warning, profile_, resolvedLanguage_);
        DWORD error{};
        if (!document_.ContinueAfterExternalChange(error)) {
            ShowFileError(strings.openFailed, error);
            return ExternalChangeResolution::Failed;
        }
        UpdateTitle();
        ScheduleRender(R2PRRegion::Status);
        return ExternalChangeResolution::Continue;
    }

    const bool hadUnsavedChanges = document_.Dirty();
    if (hadUnsavedChanges) {
        std::wstring prompt = strings.externalChangePrompt;
        try { prompt += L"\n\n" + document_.Path().wstring(); }
        catch (...) { prompt = strings.externalChangePrompt; }
        const MessageDialogResult answer = ShowMessageDialog(window_, instance_, L"BinEdit", prompt,
            MessageDialogButtons::ReloadContinue, MessageDialogIcon::Warning,
            profile_, resolvedLanguage_);
        if (answer != MessageDialogResult::Yes) {
            DWORD error{};
            if (!document_.ContinueAfterExternalChange(error)) {
                ShowFileError(strings.openFailed, error);
                return ExternalChangeResolution::Failed;
            }
            UpdateTitle();
            ScheduleRender(R2PRRegion::Status);
            return ExternalChangeResolution::Continue;
        }
    }

    std::filesystem::path reloadPath;
    try { reloadPath = document_.Path(); }
    catch (...) { return ExternalChangeResolution::Failed; }
    ByteDocument loaded;
    DWORD error{};
    if (!LoadDocumentWithProgress(reloadPath, loaded, error)) {
        // Dirty reload was an explicit action, so report its failure. Automatic
        // clean reloads retry silently after transient publisher locks clear.
        if (hadUnsavedChanges) ShowFileError(strings.openFailed, error);
        return ExternalChangeResolution::Failed;
    }
    document_ = std::move(loaded);
    ClearRecoveryCheckpoint();
    ResetDocumentSession();
    UpdateTitle();
    UpdateScrollBar();
    ScheduleRender(R2PRRegion::Full);
    return ExternalChangeResolution::Reloaded;
}

BinEditApp::ExternalChangeResolution BinEditApp::QueryAndResolveActiveExternalChange(
    bool forceQuery) {
    if (resolvingExternalChange_ || document_.Path().empty()) return ExternalChangeResolution::None;
    resolvingExternalChange_ = true;
    struct ResetFlag {
        bool& value;
        ~ResetFlag() { value = false; }
    } reset{resolvingExternalChange_};
    DWORD error{};
    const ExternalFileState state = document_.CheckExternalChange(error, forceQuery);
    if (forceQuery && state == ExternalFileState::Unavailable) {
        ShowFileError(GetStrings(resolvedLanguage_).saveFailed, error);
        return ExternalChangeResolution::Failed;
    }
    return ResolveActiveExternalChange(state, error);
}

void BinEditApp::CheckExternalChanges() {
    static_cast<void>(QueryAndResolveActiveExternalChange(false));

    // Inactive clean tabs can reload without modal UI. Dirty and missing tabs
    // retain a sticky model notification and are resolved when selected, which
    // avoids switching the user's context merely because a background tab changed.
    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        if (index == activeTab_) continue;
        EditorTabState& state = tabs_[index];
        DWORD error{};
        const ExternalFileState external = state.document.CheckExternalChange(error);
        if (external != ExternalFileState::Changed || state.document.Dirty()) continue;
        std::filesystem::path reloadPath;
        try { reloadPath = state.document.Path(); }
        catch (...) { continue; }
        ByteDocument loaded;
        if (!LoadDocumentWithProgress(reloadPath, loaded, error)) continue;
        state.document = std::move(loaded);
        state.caret = state.selectionAnchor = state.selectionActive = state.firstRow = 0;
        state.hexPane = true;
        state.lowNibble = false;
        state.insertMode = true;
        std::vector<MarkupRange>{}.swap(state.markups);
        state.searchPattern.reset();
        std::vector<std::size_t>{}.swap(state.searchOffsets);
        state.searchResultsStale = false;
    }
}

bool BinEditApp::SaveDialog() {
    std::vector<wchar_t> path(32768, L'\0');
    if (!document_.Path().empty()) wcsncpy_s(path.data(), path.size(), document_.Path().c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_; dialog.lpstrFile = path.data(); dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrFilter = resolvedLanguage_ == UiLanguage::Japanese ? L"すべてのファイル\0*.*\0\0" : L"All files\0*.*\0\0";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_EXPLORER;
    if (!GetSaveFileNameW(&dialog)) return false;
    std::filesystem::path destination;
    try { destination = path.data(); }
    catch (...) {
        ShowFileError(GetStrings(resolvedLanguage_).saveFailed, ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    DWORD error{};
    if (window_) KillTimer(window_, kRecoveryTimer);
    if (!RunIoOperation(GetStrings(resolvedLanguage_).statusSavingFile,
        [this, destination](DWORD& workerError) {
            JoinRecoveryWriter(true);
            return document_.SaveAs(destination, workerError);
        }, error)) {
        // Only a live ACL access failure may request elevation. ByteDocument maps
        // FILE_ATTRIBUTE_READONLY to ERROR_FILE_READ_ONLY at save time instead.
        if (error == ERROR_ACCESS_DENIED) {
            const MessageDialogResult answer = ShowMessageDialog(window_, instance_, L"BinEdit",
                GetStrings(resolvedLanguage_).elevationPrompt, MessageDialogButtons::YesNo,
                MessageDialogIcon::Warning, profile_, resolvedLanguage_);
            if (answer == MessageDialogResult::Yes && RestartElevatedForSave(destination)) return true;
            if (answer == MessageDialogResult::Yes) return false;
        }
        ShowFileError(GetStrings(resolvedLanguage_).saveFailed, error); return false;
    }
    UpdateTitle(); ScheduleRender(R2PRRegion::Status); return true;
}

bool BinEditApp::Save(bool saveAs) {
    if (saveAs || document_.Path().empty()) return SaveDialog();
    // Sparse target writes are preceded by a durable snapshot of the exact
    // revision being saved. If recovery storage is unavailable, preserve the
    // crash-safety guarantee by publishing a full atomic sibling instead.
    if (window_) KillTimer(window_, kRecoveryTimer);
    if (document_.Dirty() && recoveryId_.empty()) {
        try { recoveryId_ = RecoveryStore::NewId(); }
        catch (...) { recoveryId_.clear(); }
    }
    DWORD error{};
    bool saved = false;
    // A very active external writer can race more than once. Bound the number of
    // confirmation/retry cycles so a hostile process cannot trap the UI thread.
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const std::uint64_t revision = document_.Revision();
        bool checkpointSucceeded = !document_.Dirty();
        if (!RunIoOperation(GetStrings(resolvedLanguage_).statusSavingFile,
            [this, revision, &checkpointSucceeded](DWORD& workerError) {
                JoinRecoveryWriter(true);
                const bool alreadyDurable = recoveryDurableRevision_ == revision;
                if (!alreadyDurable && document_.Dirty() && !recoveryId_.empty()) {
                    DWORD checkpointError{};
                    checkpointSucceeded = RecoveryStore::Write(recoveryId_, document_.Path(),
                        document_.CreateSnapshot(), {}, checkpointError);
                } else if (alreadyDurable) {
                    checkpointSucceeded = true;
                }
                return checkpointSucceeded ? document_.Save(workerError) :
                    document_.SaveAtomically(workerError);
            }, error)) {
            if (checkpointSucceeded) {
                recoveryDurableRevision_ = revision;
                recoveryCheckpointTick_ = GetTickCount64();
            }
        } else {
            if (checkpointSucceeded) {
                recoveryDurableRevision_ = revision;
                recoveryCheckpointTick_ = GetTickCount64();
            }
            saved = true;
            break;
        }
        if (error != ERROR_FILE_INVALID) break;
        const ExternalChangeResolution resolution = QueryAndResolveActiveExternalChange(true);
        if (resolution == ExternalChangeResolution::Reloaded) return true;
        if (resolution == ExternalChangeResolution::Failed) return false;
        if (resolution != ExternalChangeResolution::Continue) break;
    }
    if (!saved) {
        // Read-only file attributes are an explicit user policy and are never a
        // reason to restart elevated; only an ACL denial reaches this branch.
        if (error == ERROR_ACCESS_DENIED) {
            const MessageDialogResult answer = ShowMessageDialog(window_, instance_, L"BinEdit",
                GetStrings(resolvedLanguage_).elevationPrompt, MessageDialogButtons::YesNo,
                MessageDialogIcon::Warning, profile_, resolvedLanguage_);
            if (answer == MessageDialogResult::Yes && RestartElevatedForSave(document_.Path())) return true;
            if (answer == MessageDialogResult::Yes) return false;
        }
        ShowFileError(GetStrings(resolvedLanguage_).saveFailed, error); return false;
    }
    UpdateTitle(); ScheduleRender(R2PRRegion::Status); return true;
}

void BinEditApp::ShowFileError(const wchar_t* operation, DWORD error) const {
    // Request the selected UI language explicitly. If Windows lacks that message
    // resource, the numeric fallback remains localized by BinEdit itself.
    wchar_t* systemMessage = nullptr;
    const DWORD language = resolvedLanguage_ == UiLanguage::Japanese ? MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT) :
                                                                      MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, language, reinterpret_cast<LPWSTR>(&systemMessage), 0, nullptr);
    std::wstring text = operation;
    text += resolvedLanguage_ == UiLanguage::Japanese ? L"。\n" : L".\n";
    if (systemMessage) { text += systemMessage; LocalFree(systemMessage); }
    else text += (resolvedLanguage_ == UiLanguage::Japanese ? L"エラーコード: " : L"Error code: ") + std::to_wstring(error);
    ShowMessageDialog(window_, instance_, L"BinEdit", text, MessageDialogButtons::Ok,
        MessageDialogIcon::Error, profile_, resolvedLanguage_);
}

bool BinEditApp::RestartElevatedForSave(const std::filesystem::path& target) {
    // Never elevate the current process in place. A snapshot is written first so
    // the new process receives every unsaved edit without shared memory or IPC.
    if (target.empty()) return false;
    // Persist the newest generation before the original process exits. The
    // elevated helper removes it only after the protected destination commits.
    if (window_) KillTimer(window_, kRecoveryTimer);
    if (document_.Dirty() && recoveryId_.empty()) {
        try { recoveryId_ = RecoveryStore::NewId(); }
        catch (...) { recoveryId_.clear(); }
    }
    std::array<wchar_t, MAX_PATH> tempDirectory{};
    if (!GetTempPathW(static_cast<DWORD>(tempDirectory.size()), tempDirectory.data())) return false;
    std::wstring stagingId;
    try { stagingId = RecoveryStore::NewId(); } catch (...) { return false; }
    if (stagingId.empty()) return false;
    const std::filesystem::path tempFile = std::filesystem::path(tempDirectory.data()) /
        (L"BNE" + stagingId + L".tmp");
    DWORD error{};
    const std::uint64_t revision = document_.Revision();
    bool checkpointSucceeded = !document_.Dirty();
    if (!RunIoOperation(GetStrings(resolvedLanguage_).statusSavingFile,
        [this, tempFile, revision, &checkpointSucceeded](DWORD& workerError) {
            JoinRecoveryWriter(true);
            if (document_.Dirty() && !recoveryId_.empty() &&
                recoveryDurableRevision_ != revision) {
                DWORD checkpointError{};
                checkpointSucceeded = RecoveryStore::Write(recoveryId_, document_.Path(),
                    document_.CreateSnapshot(), {}, checkpointError);
            } else {
                checkpointSucceeded = true;
            }
            return document_.WriteCopy(tempFile, workerError);
        }, error)) {
        DeleteFileW(tempFile.c_str()); ShowFileError(GetStrings(resolvedLanguage_).stageFailed, error); return false;
    }
    if (checkpointSucceeded) {
        recoveryDurableRevision_ = revision;
        recoveryCheckpointTick_ = GetTickCount64();
    }

    std::vector<wchar_t> executable(32768, L'\0');
    if (!GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()))) {
        DeleteFileW(tempFile.c_str()); return false;
    }
    const auto quotedTemp = QuoteWindowsCommandLineArgument(tempFile.native());
    const auto quotedTarget = QuoteWindowsCommandLineArgument(target.native());
    const auto quotedRecoveryId = recoveryId_.empty() ? std::optional<std::wstring>{} :
        QuoteWindowsCommandLineArgument(recoveryId_);
    if (!quotedTemp || !quotedTarget || (!recoveryId_.empty() && !quotedRecoveryId)) {
        DeleteFileW(tempFile.c_str());
        return false;
    }
    std::wstring parameters;
    try {
        parameters = L"--elevated --recover ";
        parameters += *quotedTemp;
        parameters += L" --target ";
        parameters += *quotedTarget;
        if (quotedRecoveryId) {
            parameters += L" --recovery-id ";
            parameters += *quotedRecoveryId;
        }
        if (parameters.size() > MaximumWindowsCommandLineCharacters) {
            DeleteFileW(tempFile.c_str());
            return false;
        }
    } catch (...) {
        DeleteFileW(tempFile.c_str());
        return false;
    }
    // The runas verb is the supported shell entry point for a UAC consent prompt.
    // Each value uses the same escaping grammar as the elevated receiver's
    // CommandLineToArgvW parser, including trailing and quote-adjacent slashes.
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI; execute.hwnd = window_; execute.lpVerb = L"runas";
    execute.lpFile = executable.data(); execute.lpParameters = parameters.c_str(); execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) { DeleteFileW(tempFile.c_str()); return false; }
    if (execute.hProcess) CloseHandle(execute.hProcess);
    closingForElevation_ = true;
    if (primaryWindow_) {
        // The elevated peer must be able to acquire the process mutex. Preserve
        // current recovery generations for every other dirty host, then close the
        // complete main-owned window set without opening a second confirmation.
        for (BinEditApp* app : gEditorWindows) {
            if (app && app->window_ && app->document_.Dirty() && !app->recoveryId_.empty())
                static_cast<void>(app->EnsureRecoveryCheckpoint());
        }
        DestroyAllWindows();
    } else if (window_) {
        DestroyWindow(window_);
    }
    return true;
}

void BinEditApp::ShowFind() {
    searchDialog_.encoding = profile_.encoding;
    searchDialog_.language = resolvedLanguage_;
    auto result = ShowSearchDialog(window_, instance_, searchDialog_, profile_);
    if (!result.accepted) return;
    searchDialog_ = result;
    SearchPatternError error{};
    auto pattern = result.binary ? ParseBinaryPattern(result.query, error) : EncodeTextPattern(result.query, result.encoding, error);
    if (!pattern) {
        ShowMessageDialog(window_, instance_, GetStrings(resolvedLanguage_).searchTitle,
            SearchErrorMessage(error), MessageDialogButtons::Ok, MessageDialogIcon::Warning,
            profile_, resolvedLanguage_);
        return;
    }
    if (!result.binary) {
        profile_.encoding = result.encoding;
        CommitProfileChange();
    }
    searchPattern_ = std::move(pattern);
    searchOffsets_.clear();
    searchResultsStale_ = true;
    ScheduleRender(R2PRRegion::Data);
    StartAsyncSearch(true, true, true);
}

void BinEditApp::FindNext(bool fromNewSearch) {
    if (!searchPattern_) { ShowFind(); return; }
    StartAsyncSearch(true, fromNewSearch, true);
}

void BinEditApp::ApplySearchSelection(std::size_t offset) {
    caret_ = selectionAnchor_ = offset;
    selectionActive_ = std::min(document_.Size() - 1, offset + searchPattern_->bytes.size() - 1);
    lowNibble_ = false; EnsureCaretVisible();
    ScheduleRender(R2PRRegion::Data); ScheduleRender(R2PRRegion::Status);
}

void BinEditApp::StartAsyncSearch(bool selectNext, bool fromNewSearch, bool notifyNotFound) {
    if (!searchPattern_) return;
    CancelAsyncSearch();

    SearchPattern pattern;
    ImmutableByteSnapshot snapshot;
    try {
        pattern = *searchPattern_;
        snapshot = document_.CreateSnapshot();
    } catch (...) {
        searchResultsStale_ = false;
        ShowMessageDialog(window_, instance_, GetStrings(resolvedLanguage_).searchTitle,
            GetStrings(resolvedLanguage_).searchFailed, MessageDialogButtons::Ok,
            MessageDialogIcon::Error, profile_, resolvedLanguage_);
        return;
    }

    const std::size_t start = fromNewSearch ? caret_ :
        (caret_ < document_.Size() ? caret_ + 1 : 0);
    const std::uint64_t generation = searchGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    const HWND completionWindow = window_;
    searchInProgress_ = true;
    ScheduleRender(R2PRRegion::Status);
    try {
        searchWorker_ = std::jthread([this, completionWindow, generation, start, selectNext,
            notifyNotFound, snapshot = std::move(snapshot), pattern = std::move(pattern)](std::stop_token stopToken) mutable {
            SetThreadDescription(GetCurrentThread(), L"BinEdit search coordinator");
            SearchCompletion completion;
            completion.generation = generation;
            completion.selectNext = selectNext;
            completion.notifyNotFound = notifyNotFound;
            try {
                completion.result = FindPatternsParallel(snapshot, pattern, start, 10000, stopToken);
            } catch (...) {
                completion.result.failed = true;
            }
            if (stopToken.stop_requested()) return;
            {
                std::scoped_lock lock(searchCompletionMutex_);
                if (generation != searchGeneration_.load(std::memory_order_relaxed)) return;
                searchCompletion_ = std::move(completion);
            }
            PostMessageW(completionWindow, WM_APP_SEARCH_COMPLETE, 0, 0);
        });
    } catch (...) {
        searchInProgress_ = false;
        searchResultsStale_ = false;
        ScheduleRender(R2PRRegion::Status);
        ShowMessageDialog(window_, instance_, GetStrings(resolvedLanguage_).searchTitle,
            GetStrings(resolvedLanguage_).searchFailed, MessageDialogButtons::Ok,
            MessageDialogIcon::Error, profile_, resolvedLanguage_);
    }
}

void BinEditApp::QueueSearchRefresh() {
    if (!searchPattern_) return;
    CancelAsyncSearch();
    searchOffsets_.clear();
    searchResultsStale_ = true;
    ScheduleRender(R2PRRegion::Data);
    ScheduleRender(R2PRRegion::Status);
    // Rapid typing restarts one timer instead of copying and searching the file
    // for every nibble. Failure to create a timer falls back to immediate work.
    if (!SetTimer(window_, kSearchRefreshTimer, kSearchRefreshDelayMilliseconds, nullptr)) {
        StartAsyncSearch(false, false, false);
    }
}

void BinEditApp::CancelAsyncSearch() {
    if (window_) KillTimer(window_, kSearchRefreshTimer);
    searchGeneration_.fetch_add(1, std::memory_order_relaxed);
    if (searchWorker_.joinable()) {
        searchWorker_.request_stop();
        searchWorker_.join();
    }
    {
        std::scoped_lock lock(searchCompletionMutex_);
        searchCompletion_.reset();
    }
    searchInProgress_ = false;
}

void BinEditApp::HandleSearchCompletion() {
    std::optional<SearchCompletion> completion;
    {
        std::scoped_lock lock(searchCompletionMutex_);
        if (!searchCompletion_ || searchCompletion_->generation !=
            searchGeneration_.load(std::memory_order_relaxed)) return;
        completion = std::move(searchCompletion_);
        searchCompletion_.reset();
    }
    if (searchWorker_.joinable()) searchWorker_.join();
    searchInProgress_ = false;
    ScheduleRender(R2PRRegion::Status);
    if (!completion) {
        searchResultsStale_ = false;
        return;
    }
    if (completion->result.failed) {
        searchResultsStale_ = false;
        ShowMessageDialog(window_, instance_, GetStrings(resolvedLanguage_).searchTitle,
            GetStrings(resolvedLanguage_).searchFailed, MessageDialogButtons::Ok,
            MessageDialogIcon::Error, profile_, resolvedLanguage_);
        return;
    }
    if (completion->result.canceled) return;

    searchOffsets_ = std::move(completion->result.offsets);
    searchResultsStale_ = false;
    ScheduleRender(R2PRRegion::Data);
    if (completion->selectNext) {
        if (completion->result.next) ApplySearchSelection(*completion->result.next);
        else if (completion->notifyNotFound) {
            ShowMessageDialog(window_, instance_, GetStrings(resolvedLanguage_).searchTitle,
                GetStrings(resolvedLanguage_).searchNotFound, MessageDialogButtons::Ok,
                MessageDialogIcon::Information, profile_, resolvedLanguage_);
        }
    }
}

void BinEditApp::ShowBitTool() {
    // The tool is a pure calculator: invocation and results never depend on or
    // mutate the active document, selection, history, search state, or caret.
    bitToolDialog_ = ShowBitToolDialog(window_, instance_, bitToolDialog_,
                                       resolvedLanguage_, profile_);
}

void BinEditApp::MoveCaret(std::ptrdiff_t delta, bool extend) {
    const auto signedCaret = static_cast<std::ptrdiff_t>(caret_);
    const auto maximum = static_cast<std::ptrdiff_t>(document_.Size());
    const std::size_t target = static_cast<std::size_t>(std::clamp(signedCaret + delta, std::ptrdiff_t{0}, maximum));
    SetCaret(target, extend, hexPane_);
}

void BinEditApp::SetCaret(std::size_t offset, bool extend, bool hexPane) {
    // Size itself is a valid insertion position represented by the virtual EOF
    // cell. Existing-byte operations continue to reject that offset explicitly.
    offset = std::min(offset, document_.Size());
    const std::size_t old = caret_;
    const bool oldPane = hexPane_;
    caret_ = selectionActive_ = offset;
    if (!extend) selectionAnchor_ = offset;
    hexPane_ = hexPane || !renderer_.Metrics().textPaneVisible;
    lowNibble_ = false;
    EnsureCaretVisible();
    // Extending can affect every cell between anchor and active positions. A
    // simple move damages only the old and new rows through ByteCell requests.
    if (extend) ScheduleRender(R2PRRegion::Data);
    else { ScheduleRender(R2PRRegion::ByteCell, old); ScheduleRender(R2PRRegion::ByteCell, caret_); }
    if (oldPane != hexPane_) ScheduleRender(R2PRRegion::Header);
    ScheduleRender(R2PRRegion::Status);
}

void BinEditApp::EnsureCaretVisible() {
    const std::size_t row = caret_ / LayoutMetrics::bytesPerRow;
    const std::size_t visible = renderer_.Metrics().visibleRows;
    const std::size_t oldFirst = firstRow_;
    if (row < firstRow_) firstRow_ = row;
    else if (row >= firstRow_ + visible) firstRow_ = row - visible + 1;
    UpdateScrollBar();
    if (oldFirst != firstRow_) ScheduleRender(R2PRRegion::Data);
}

void BinEditApp::EditHexDigit(int digit) {
    if (caret_ > document_.Size()) return;
    if (!lowNibble_ && (insertMode_ || caret_ == document_.Size())) {
        // A high nibble materializes exactly one byte. Insert mode places it before
        // the current byte, while overwrite mode grows only from the virtual EOF.
        const std::uint8_t value = static_cast<std::uint8_t>(digit << 4);
        const bool atEnd = caret_ == document_.Size();
        if (atEnd) {
            document_.AppendByte(value);
            QueueSearchRefresh();
        } else {
            const std::array<std::uint8_t, 1> inserted{value};
            if (!document_.InsertRange(caret_, inserted)) return;
            RebaseViewAfterInsert(caret_, inserted.size());
        }
        lowNibble_ = true;
        UpdateTitle();
        UpdateScrollBar();
        ScheduleRender(atEnd ? R2PRRegion::ByteCell : R2PRRegion::Data, caret_);
        ScheduleRender(R2PRRegion::Status);
        return;
    }
    const auto old = document_.ByteAt(caret_);
    const std::uint8_t value = lowNibble_ ? static_cast<std::uint8_t>((old & 0xf0) | digit) :
                                          static_cast<std::uint8_t>((digit << 4) | (old & 0x0f));
    if (document_.SetByte(caret_, value)) {
        QueueSearchRefresh();
        ScheduleRender(R2PRRegion::ByteCell, caret_);
        ScheduleRender(R2PRRegion::Status);
        UpdateTitle();
    }
    // A complete byte consumes two hexadecimal keystrokes; advance only after
    // the low nibble so the next input starts at the following byte.
    if (lowNibble_) { lowNibble_ = false; MoveCaret(1, false); }
    else lowNibble_ = true;
}

void BinEditApp::EditCharacter(wchar_t ch) {
    SearchPatternError error{};
    auto encoded = EncodeTextPattern(std::wstring(1, ch), profile_.encoding, error);
    if (!encoded) { MessageBeep(MB_ICONWARNING); return; }
    std::size_t offset = caret_;
    if (insertMode_) {
        // Encoded characters are inserted atomically so one Undo removes the
        // complete UTF-8, UTF-16, or Shift-JIS sequence rather than a partial tail.
        std::vector<std::uint8_t> inserted;
        inserted.reserve(encoded->bytes.size());
        for (const auto& byte : encoded->bytes) inserted.push_back(byte.value);
        if (!document_.InsertRange(offset, inserted)) return;
        RebaseViewAfterInsert(offset, inserted.size());
        offset += inserted.size();
    } else {
        // Overwrite mode replaces consecutive existing bytes and grows only after
        // the encoded sequence reaches the virtual EOF position.
        for (const auto& byte : encoded->bytes) {
            if (offset < document_.Size()) document_.SetByte(offset, byte.value);
            else document_.AppendByte(byte.value);
            ++offset;
        }
    }
    if (offset != caret_) {
        if (!insertMode_) QueueSearchRefresh();
        UpdateScrollBar();
        UpdateTitle(); ScheduleRender(R2PRRegion::Data); ScheduleRender(R2PRRegion::Status);
        SetCaret(std::min(offset, document_.Size()), false, false);
    }
}

void BinEditApp::RebaseViewAfterInsert(std::size_t offset, std::size_t count) {
    for (auto& range : markups_) {
        if (range.first >= offset) {
            range.first += count;
            range.last += count;
        } else if (range.last >= offset) {
            // Inserting inside a marked interval makes the new bytes part of it.
            range.last += count;
        }
    }
    QueueSearchRefresh();
}

void BinEditApp::RebaseViewAfterErase(std::size_t offset, std::size_t count) {
    const std::size_t end = offset + count;
    std::vector<MarkupRange> rebased;
    rebased.reserve(markups_.size());
    for (auto range : markups_) {
        if (range.last < offset) {
            rebased.push_back(range);
            continue;
        }
        if (range.first >= end) {
            range.first -= count;
            range.last -= count;
            rebased.push_back(range);
            continue;
        }
        // Preserve surviving portions on either side of the removed interval.
        // A range spanning the deletion naturally closes over the shifted bytes.
        if (range.first < offset) {
            range.last = range.last >= end ? range.last - count : offset - 1;
            rebased.push_back(range);
        } else if (range.last >= end) {
            range.first = offset;
            range.last -= count;
            rebased.push_back(range);
        }
    }
    markups_ = std::move(rebased);
    QueueSearchRefresh();
}

void BinEditApp::Backspace() {
    if (document_.Empty()) return;
    std::size_t eraseAt{};
    std::size_t eraseCount{};
    if (selectionAnchor_ != selectionActive_) {
        const std::size_t low = std::min(selectionAnchor_, selectionActive_);
        const std::size_t high = std::min(std::max(selectionAnchor_, selectionActive_), document_.Size() - 1);
        if (low >= document_.Size()) return;
        eraseAt = low;
        eraseCount = high - low + 1;
    } else {
        const std::size_t end = std::min(caret_, document_.Size());
        if (end == 0) return;
        eraseAt = hexPane_ ? end - 1 : PreviousCharacterStart(document_, end, profile_.encoding);
        eraseCount = end - eraseAt;
    }
    if (!document_.EraseRange(eraseAt, eraseCount)) return;
    RebaseViewAfterErase(eraseAt, eraseCount);
    caret_ = selectionAnchor_ = selectionActive_ = eraseAt;
    lowNibble_ = false;
    EnsureCaretVisible();
    UpdateTitle();
    UpdateScrollBar();
    // Deletion shifts every following byte, so the data viewport is the minimum
    // safe R2PR region even when only one character was removed.
    ScheduleRender(R2PRRegion::Data);
    ScheduleRender(R2PRRegion::Status);
}

void BinEditApp::DeleteForward() {
    if (document_.Empty()) return;
    const std::size_t low = std::min(selectionAnchor_, selectionActive_);
    const std::size_t high = std::max(selectionAnchor_, selectionActive_);
    if (low >= document_.Size()) return;
    const std::size_t last = std::min(high, document_.Size() - 1);
    lowNibble_ = false;
    if (insertMode_) {
        const std::size_t count = last - low + 1;
        if (!document_.EraseRange(low, count)) return;
        RebaseViewAfterErase(low, count);
        caret_ = selectionAnchor_ = selectionActive_ = low;
        EnsureCaretVisible();
        UpdateScrollBar();
    } else {
        // Overwrite mode preserves byte addresses by clearing the target range.
        for (std::size_t at = low; at <= last; ++at) document_.SetByte(at, 0);
        QueueSearchRefresh();
    }
    UpdateTitle();
    ScheduleRender(R2PRRegion::Data);
    ScheduleRender(R2PRRegion::Status);
}

void BinEditApp::Undo() {
    const std::size_t oldSize = document_.Size();
    std::size_t offset{};
    std::size_t changedCount{};
    if (!document_.Undo(offset, &changedCount)) return;
    caret_ = selectionAnchor_ = selectionActive_ = offset; EnsureCaretVisible(); UpdateTitle();
    // Both size-changing and in-place history entries can create or remove
    // search matches, so rebuild highlights after every successful replay.
    QueueSearchRefresh();
    if (document_.Size() != oldSize) {
        UpdateScrollBar();
        ScheduleRender(R2PRRegion::Data);
    } else if (changedCount == 1) ScheduleRender(R2PRRegion::ByteCell, offset);
    else ScheduleRender(R2PRRegion::Data);
    ScheduleRender(R2PRRegion::Status);
}

void BinEditApp::Redo() {
    const std::size_t oldSize = document_.Size();
    std::size_t offset{};
    std::size_t changedCount{};
    if (!document_.Redo(offset, &changedCount)) return;
    caret_ = selectionAnchor_ = selectionActive_ = offset; EnsureCaretVisible(); UpdateTitle();
    QueueSearchRefresh();
    if (document_.Size() != oldSize) {
        UpdateScrollBar();
        ScheduleRender(R2PRRegion::Data);
    } else if (changedCount == 1) ScheduleRender(R2PRRegion::ByteCell, offset);
    else ScheduleRender(R2PRRegion::Data);
    ScheduleRender(R2PRRegion::Status);
}

void BinEditApp::ToggleMarkup() {
    if (document_.Empty()) return;
    const std::size_t low = std::min(selectionAnchor_, selectionActive_);
    if (low >= document_.Size()) return;
    const std::size_t high = std::min(std::max(selectionAnchor_, selectionActive_), document_.Size() - 1);
    const auto existing = std::find_if(markups_.begin(), markups_.end(), [=](const MarkupRange& range) { return range.first == low && range.last == high; });
    if (existing != markups_.end()) markups_.erase(existing);
    else markups_.push_back({low, high, profile_.markupColor});
    ScheduleRender(R2PRRegion::Data);
}

std::optional<std::uint32_t> BinEditApp::PickColor(std::uint32_t initial) const {
    // The application-owned picker returns the same platform-neutral 0xRRGGBB
    // representation used by Profile and every renderer. No CHOOSECOLOR state or
    // COLORREF byte-order conversion crosses this boundary.
    return ShowColorPickerDialog(window_, instance_, initial, resolvedLanguage_, profile_);
}

void BinEditApp::ChooseMarkupColor() {
    if (auto color = PickColor(profile_.markupColor)) {
        profile_.markupColor = *color;
        for (auto& markup : markups_) markup.color = *color;
        CommitProfileChange();
    }
}

void BinEditApp::ChooseByteColor() {
    // Commands can be synthesized without traversing the native menu. Keep the
    // same virtual-EOF guard here so stale or automated WM_COMMAND delivery can
    // never index one byte beyond the document.
    if (caret_ >= document_.Size()) return;
    const auto value = document_.ByteAt(caret_);
    const std::uint32_t initial = profile_.byteColors[value].value_or(profile_.markupColor);
    if (auto color = PickColor(initial)) {
        profile_.byteColors[value] = *color;
        CommitProfileChange();
    }
}

void BinEditApp::CommitProfileChange(bool localizedMenusChanged) {
    // Detached hosts are separate controllers but expose one application-level
    // preferences file. Copying a single committed snapshot prevents stale
    // windows from retaining an old theme or overwriting a newer selection.
    profile_.Save();
    const Profile committed = profile_;
    for (BinEditApp* app : gEditorWindows) {
        if (!app || !app->window_) continue;
        app->profile_ = committed;
        app->searchDialog_.encoding = committed.encoding;
        const UiLanguage language = ResolveLanguage(committed.language);
        const bool rebuildMenu = localizedMenusChanged || language != app->resolvedLanguage_;
        app->resolvedLanguage_ = language;
        app->searchDialog_.language = language;
        if (rebuildMenu) {
            app->RebuildMenu();
            // Another host may currently lend its document to the file worker.
            // Defer title localization until that worker has joined.
            if (app->ioBusy_) app->ioTitleRefreshPending_ = true;
            else app->UpdateTitle();
        }
        // ApplyTheme also refreshes checks and schedules a full R2PR frame, so
        // byte colors, encoding labels, and palette changes become atomic from
        // the user's perspective across all top-level windows.
        app->ApplyTheme();
    }
}

void BinEditApp::ApplyTheme() {
    // SetWindowTheme can synchronously emit WM_THEMECHANGED. The guard prevents
    // recursive re-entry while still allowing later operating-system changes.
    if (applyingTheme_) return;
    applyingTheme_ = true;
    const Palette palette = profile_.ResolvePalette();
    BOOL dark = palette.dark && !palette.highContrast;
    constexpr DWORD immersiveDarkMode = 20;
    DwmSetWindowAttribute(window_, immersiveDarkMode, &dark, sizeof(dark));
    ThemeMenu::ApplyPreferredAppMode(window_, dark != FALSE, palette.highContrast);
    SetWindowTheme(window_, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    UpdateMenuChecks();
    ScheduleRender(R2PRRegion::Full);
    applyingTheme_ = false;
}

void BinEditApp::ApplyLanguage(LanguagePreference language) {
    // Rebuild instead of mutating individual menu items, which guarantees every
    // accelerator and nested popup label comes from one consistent string table.
    profile_.language = language;
    CommitProfileChange(true);
}

const wchar_t* BinEditApp::SearchErrorMessage(SearchPatternError error) const {
    const auto& strings = GetStrings(resolvedLanguage_);
    switch (error) {
    case SearchPatternError::Empty: return strings.searchEmpty;
    case SearchPatternError::InvalidHex: return strings.searchInvalidHex;
    case SearchPatternError::MissingSeparator: return strings.searchSeparator;
    case SearchPatternError::Unrepresentable: return strings.searchUnrepresentable;
    case SearchPatternError::EncodingFailed: return strings.searchEncodingFailed;
    case SearchPatternError::TooLarge: return strings.searchTooLarge;
    case SearchPatternError::AllocationFailed: return strings.searchAllocationFailed;
    default: return strings.searchEncodingFailed;
    }
}

LRESULT CALLBACK BinEditApp::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    try {
        // WM_NCCREATE is the earliest message carrying lpCreateParams. Persisting
        // the pointer here makes it available to all later creation messages.
        BinEditApp* app = reinterpret_cast<BinEditApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<BinEditApp*>(create->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    } catch (...) {
        Security::FailFast(L"BinEditApp::WindowProc");
    }
}

LRESULT BinEditApp::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    // UAH draw messages are intercepted before the standard switch because their
    // numeric values are undocumented and not declared by the Windows SDK.
    if (message == ThemeMenu::DrawMenuMessage || message == ThemeMenu::DrawMenuItemMessage) {
        LRESULT result{};
        if (ThemeMenu::HandleDrawMessage(window_, message, lParam, profile_.ResolvePalette(), result)) return result;
    }
    if (ioBusy_) {
        // The nested pump remains responsive for painting, resizing, DPI, and
        // system-theme changes, but no interaction may observe or mutate the
        // ByteDocument while it is exclusively owned by the file worker.
        switch (message) {
        case WM_COMMAND:
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_CHAR:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_CONTEXTMENU:
            return 0;
        default:
            break;
        }
    }
    switch (message) {
    case WM_COPYDATA: {
        // Only the documented single-instance payload is accepted. Size,
        // alignment, termination, and every string boundary are validated before
        // an external process can request any file-system operation.
        Security::TrustedWindowPeer senderPeer;
        if (!Security::CaptureTrustedWindowPeer(
            reinterpret_cast<HWND>(wParam), senderPeer)) return FALSE;
        std::vector<std::filesystem::path> paths;
        const auto* transfer = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!OpenFilesProtocol::Parse(transfer, paths)) return FALSE;
        if (!Security::RevalidateTrustedWindowPeer(senderPeer)) return FALSE;
        QueueOpenPaths(std::move(paths));
        if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
        else if (!IsWindowVisible(window_)) ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
        return TRUE;
    }
    case WM_COMMAND:
        // Menu commands and keyboard accelerators converge on identical actions.
        switch (LOWORD(wParam)) {
        case IDM_FILE_NEW: CreateNewTab(); break;
        case IDM_FILE_OPEN: OpenDialog(); break;
        case IDM_FILE_SAVE: Save(false); break;
        case IDM_FILE_SAVE_AS: Save(true); break;
        case IDM_FILE_CLOSE: CloseTab(activeTab_); break;
        case IDM_FILE_DETACH_TAB: {
            if (!CanDetachTab()) break;
            RECT rect{}; GetWindowRect(window_, &rect);
            DetachActiveTab({rect.left + 80, rect.top + 80});
            break;
        }
        case IDM_FILE_EXIT:
            // This process-level command is intentionally absent from sub menus;
            // retain the same role boundary against synthesized WM_COMMAND input.
            if (primaryWindow_) SendMessageW(window_, WM_CLOSE, 0, 0);
            break;
        case IDM_EDIT_UNDO: Undo(); break;
        case IDM_EDIT_REDO: Redo(); break;
        case IDM_EDIT_FIND: ShowFind(); break;
        case IDM_EDIT_FIND_NEXT: FindNext(); break;
        case IDM_EDIT_MARK: ToggleMarkup(); break;
        case IDM_EDIT_CLEAR_MARKS: markups_.clear(); ScheduleRender(R2PRRegion::Data); break;
        case IDM_TOOLS_BIT_OPERATION: ShowBitTool(); break;
        case IDM_COLOR_MARK: ChooseMarkupColor(); break;
        case IDM_COLOR_BYTE: ChooseByteColor(); break;
        case IDM_COLOR_CLEAR_BYTE:
            if (caret_ < document_.Size()) {
                profile_.byteColors[document_.ByteAt(caret_)].reset();
                CommitProfileChange();
            }
            break;
        case IDM_THEME_SYSTEM:
            if (primaryWindow_) { profile_.theme = ThemePreference::System; CommitProfileChange(); }
            break;
        case IDM_THEME_LIGHT:
            if (primaryWindow_) { profile_.theme = ThemePreference::Light; CommitProfileChange(); }
            break;
        case IDM_THEME_DARK:
            if (primaryWindow_) { profile_.theme = ThemePreference::Dark; CommitProfileChange(); }
            break;
        case IDM_ENCODING_ASCII: profile_.encoding = TextEncoding::Ascii; CommitProfileChange(); break;
        case IDM_ENCODING_UTF8: profile_.encoding = TextEncoding::Utf8; CommitProfileChange(); break;
        case IDM_ENCODING_UTF16LE: profile_.encoding = TextEncoding::Utf16Le; CommitProfileChange(); break;
        case IDM_ENCODING_SHIFTJIS: profile_.encoding = TextEncoding::ShiftJis; CommitProfileChange(); break;
        case IDM_LANGUAGE_SYSTEM:
            if (primaryWindow_) ApplyLanguage(LanguagePreference::System);
            break;
        case IDM_LANGUAGE_JAPANESE:
            if (primaryWindow_) ApplyLanguage(LanguagePreference::Japanese);
            break;
        case IDM_LANGUAGE_ENGLISH:
            if (primaryWindow_) ApplyLanguage(LanguagePreference::English);
            break;
        case IDM_HELP_ABOUT:
            ShowAboutDialog(window_, instance_, resolvedLanguage_, profile_);
            break;
        }
        return 0;
    case WM_KEYDOWN: {
        // Navigation changes selection/caret state; printable text is handled by
        // WM_CHAR so keyboard layout and IME translation remain intact.
        UpdateTabHover(std::nullopt);
        if (autoScrollActive_) {
            StopAutoScroll();
            if (wParam == VK_ESCAPE) return 0;
        }
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl) {
            if (wParam == 'N') { CreateNewTab(); return 0; }
            if (wParam == 'O') { OpenDialog(); return 0; }
            if (wParam == 'S') { Save(shift); return 0; }
            if (wParam == 'W') { CloseTab(activeTab_); return 0; }
            if (wParam == 'F') { ShowFind(); return 0; }
            if (wParam == 'Z') { Undo(); return 0; }
            if (wParam == 'Y') { Redo(); return 0; }
            if (wParam == VK_TAB && tabs_.size() > 1) {
                const std::size_t next = shift ? (activeTab_ + tabs_.size() - 1) % tabs_.size() :
                                                  (activeTab_ + 1) % tabs_.size();
                SwitchTab(next);
                return 0;
            }
        }
        switch (wParam) {
        case VK_F3: FindNext(); return 0;
        case VK_LEFT: MoveCaret(-1, shift); return 0;
        case VK_RIGHT: MoveCaret(1, shift); return 0;
        case VK_UP: MoveCaret(-static_cast<std::ptrdiff_t>(LayoutMetrics::bytesPerRow), shift); return 0;
        case VK_DOWN: MoveCaret(LayoutMetrics::bytesPerRow, shift); return 0;
        case VK_PRIOR: MoveCaret(-static_cast<std::ptrdiff_t>(renderer_.Metrics().visibleRows * LayoutMetrics::bytesPerRow), shift); return 0;
        case VK_NEXT: MoveCaret(static_cast<std::ptrdiff_t>(renderer_.Metrics().visibleRows * LayoutMetrics::bytesPerRow), shift); return 0;
        case VK_HOME: SetCaret(ctrl ? 0 : (caret_ / LayoutMetrics::bytesPerRow) * LayoutMetrics::bytesPerRow, shift, hexPane_); return 0;
        case VK_END:
            SetCaret(ctrl ? document_.Size() :
                std::min(document_.Size(), (caret_ / LayoutMetrics::bytesPerRow + 1) * LayoutMetrics::bytesPerRow - 1),
                shift, hexPane_);
            return 0;
        case VK_TAB:
            // Tab changes input target only when both panes are actually visible.
            // Header and status cues make this mode switch unambiguous.
            if (renderer_.Metrics().textPaneVisible) {
                hexPane_ = !hexPane_;
                lowNibble_ = false;
                ScheduleRender(R2PRRegion::Header);
                ScheduleRender(R2PRRegion::ByteCell, caret_);
                ScheduleRender(R2PRRegion::Status);
            }
            return 0;
        case VK_INSERT:
            insertMode_ = !insertMode_;
            lowNibble_ = false;
            ScheduleRender(R2PRRegion::Status);
            return 0;
        case VK_ESCAPE: selectionAnchor_ = selectionActive_ = caret_; ScheduleRender(R2PRRegion::Data); return 0;
        case VK_BACK: Backspace(); return 0;
        case VK_DELETE: DeleteForward(); return 0;
        }
        break;
    }
    case WM_CHAR:
        // WM_CHAR already represents the character produced by TranslateMessage
        // (or committed by an IME). Do not sample the *current* Ctrl key state:
        // posted characters can wait in the queue until after modifier state has
        // changed, which would incorrectly discard otherwise valid input. Ctrl
        // shortcuts translate to control codes below U+0020 and remain filtered.
        if (wParam >= 0x20) {
            if (hexPane_) { const int digit = HexValue(static_cast<wchar_t>(wParam)); if (digit >= 0) EditHexDigit(digit); }
            else EditCharacter(static_cast<wchar_t>(wParam));
        }
        return 0;
    case WM_GETDLGCODE:
        // The editor owns navigation, Tab, and printable characters directly.
        // This also documents the contract for accessibility/input frameworks
        // that query dialog-code behavior even on a top-level HWND.
        return DLGC_WANTARROWS | DLGC_WANTTAB | DLGC_WANTCHARS;
    case WM_MBUTTONDOWN: {
        if (autoScrollActive_) StopAutoScroll();
        else {
            SetFocus(window_);
            StartAutoScroll({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        }
        return 0;
    }
    case WM_MBUTTONUP:
        // Auto-scroll intentionally remains active after the initiating button
        // is released and is canceled by the next middle click or normal input.
        return 0;
    case WM_RBUTTONDOWN:
        if (autoScrollActive_) { StopAutoScroll(); return 0; }
        break;
    case WM_LBUTTONDOWN: {
        if (autoScrollActive_) { StopAutoScroll(); return 0; }
        UpdateTabHover(std::nullopt);
        SetFocus(window_);
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        const std::size_t totalRows = RowCount(document_.Size());
        const ScrollBarPart scrollPart = renderer_.HitTestScrollBar(x, y, firstRow_, totalRows);
        if (scrollPart != ScrollBarPart::None) {
            SetScrollBarHovered(true);
            if (scrollPart == ScrollBarPart::Thumb) {
                const ScrollBarGeometry geometry = renderer_.GetScrollBarGeometry(firstRow_, totalRows);
                scrollBarDragging_ = true;
                scrollBarDragOffset_ = y - geometry.thumbTop;
                SetCapture(window_);
                ScheduleRender(R2PRRegion::ScrollBar);
            } else {
                const std::size_t page = std::max<std::size_t>(1, renderer_.Metrics().visibleRows);
                if (scrollPart == ScrollBarPart::TrackBefore)
                    ScrollToFirstRow(firstRow_ - std::min(firstRow_, page));
                else
                    ScrollToFirstRow(firstRow_ + page);
            }
            return 0;
        }
        if (const auto tab = renderer_.HitTestTab(x, y, tabs_.size(), firstVisibleTab_)) {
            if (tab->closeButton) {
                CloseTab(tab->index);
            } else {
                SwitchTab(tab->index);
                tabDragging_ = true;
                tabDragStart_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(window_, &tabDragStart_);
                // Physical drags need capture after leaving the source HWND.
                // Message-based UI automation has no asynchronous button state;
                // retaining the logical drag without global capture keeps that
                // accessibility/test path deterministic until its delivered button-up.
                if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) SetCapture(window_);
            }
            return 0;
        }
        const auto hit = renderer_.HitTest(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)), firstRow_, document_.Size());
        if (hit) { dragging_ = true; SetCapture(window_); SetCaret(hit->first, (GetKeyState(VK_SHIFT) & 0x8000) != 0, hit->second); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        if (autoScrollActive_) {
            autoScrollPointer_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            UpdateAutoScrollCursor();
            return 0;
        }
        if (tabDragging_) {
            UpdateTabHover(std::nullopt);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return 0;
        }
        const auto hoveredTab = renderer_.HitTestTab(x, y, tabs_.size(), firstVisibleTab_);
        const std::optional<std::size_t> hoveredTitle = hoveredTab && !hoveredTab->closeButton ?
            std::optional<std::size_t>(hoveredTab->index) : std::nullopt;
        UpdateTabHover(hoveredTitle);
        if (hoveredTitle && !tabMouseTracked_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
            if (TrackMouseEvent(&tracking)) tabMouseTracked_ = true;
        }
        const std::size_t totalRows = RowCount(document_.Size());
        if (scrollBarDragging_) {
            UpdateTabHover(std::nullopt);
            ScrollToFirstRow(renderer_.ScrollRowFromThumb(y, scrollBarDragOffset_, totalRows));
            SetScrollBarHovered(true);
            return 0;
        }
        const bool overScrollBar = renderer_.HitTestScrollBar(x, y, firstRow_, totalRows) != ScrollBarPart::None;
        SetScrollBarHovered(overScrollBar);
        if (overScrollBar && !scrollBarMouseTracked_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
            if (TrackMouseEvent(&tracking)) scrollBarMouseTracked_ = true;
        }
        if (dragging_ && (wParam & MK_LBUTTON)) {
            const auto hit = renderer_.HitTest(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)), firstRow_, document_.Size());
            if (hit) SetCaret(hit->first, true, hit->second);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (scrollBarDragging_) {
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            scrollBarDragging_ = false;
            ScrollToFirstRow(renderer_.ScrollRowFromThumb(
                y, scrollBarDragOffset_, RowCount(document_.Size())));
            ReleaseCapture();
            ScheduleRender(R2PRRegion::ScrollBar);
            SetScrollBarHovered(renderer_.HitTestScrollBar(
                static_cast<float>(GET_X_LPARAM(lParam)), y, firstRow_,
                RowCount(document_.Size())) != ScrollBarPart::None);
            return 0;
        }
        if (tabDragging_) {
            tabDragging_ = false;
            ReleaseCapture();
            POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(window_, &screenPoint);
            if (BinEditApp* target = WindowAtPoint(screenPoint);
                target && target != this && !target->ioBusy_) {
                POINT targetClient = screenPoint;
                ScreenToClient(target->window_, &targetClient);
                if (target->renderer_.IsInTabStrip(static_cast<float>(targetClient.y))) {
                    DockActiveTabTo(*target,
                        target->renderer_.TabDropIndex(static_cast<float>(targetClient.x),
                            target->tabs_.size(), target->firstVisibleTab_));
                    return 0;
                }
            }

            POINT local = screenPoint;
            ScreenToClient(window_, &local);
            const int dragX = std::abs(screenPoint.x - tabDragStart_.x);
            const int dragY = std::abs(screenPoint.y - tabDragStart_.y);
            if (renderer_.IsInTabStrip(static_cast<float>(local.y))) {
                ReorderActiveTab(renderer_.TabDropIndex(static_cast<float>(local.x),
                    tabs_.size(), firstVisibleTab_));
            } else if (dragX >= GetSystemMetrics(SM_CXDRAG) || dragY >= GetSystemMetrics(SM_CYDRAG)) {
                DetachActiveTab(screenPoint);
            }
            return 0;
        }
        if (dragging_) { dragging_ = false; ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED:
        dragging_ = false;
        tabDragging_ = false;
        UpdateTabHover(std::nullopt);
        if (scrollBarDragging_) {
            scrollBarDragging_ = false;
            ScheduleRender(R2PRRegion::ScrollBar);
        }
        if (autoScrollActive_) StopAutoScroll();
        return 0;
    case WM_CANCELMODE:
        dragging_ = false;
        tabDragging_ = false;
        UpdateTabHover(std::nullopt);
        scrollBarDragging_ = false;
        StopAutoScroll();
        ReleaseCapture();
        return 0;
    case WM_MOUSELEAVE:
        scrollBarMouseTracked_ = false;
        tabMouseTracked_ = false;
        UpdateTabHover(std::nullopt);
        if (!scrollBarDragging_) SetScrollBarHovered(false);
        return 0;
    case WM_MOUSEWHEEL: {
        if (autoScrollActive_) StopAutoScroll();
        if ((GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0) {
            tabWheelDeltaRemainder_ += GET_WHEEL_DELTA_WPARAM(wParam);
            const int notches = tabWheelDeltaRemainder_ / WHEEL_DELTA;
            tabWheelDeltaRemainder_ %= WHEEL_DELTA;
            if (notches == 0) return 0;
            UINT configuredCharacters = 3;
            static_cast<void>(SystemParametersInfoW(
                SPI_GETWHEELSCROLLCHARS, 0, &configuredCharacters, 0));
            if (configuredCharacters == 0) return 0;
            const std::size_t requestedPerNotch = configuredCharacters == WHEEL_PAGESCROLL ?
                std::max<std::size_t>(1,
                    tabs_.size() - renderer_.MaximumFirstVisibleTab(tabs_.size())) :
                static_cast<std::size_t>(configuredCharacters);
            const int perNotch = static_cast<int>(std::min<std::size_t>(
                requestedPerNotch, static_cast<std::size_t>(std::numeric_limits<int>::max())));
            const long long requested = -static_cast<long long>(notches) * perNotch;
            ScrollTabStrip(static_cast<int>(std::clamp<long long>(requested,
                std::numeric_limits<int>::min(), std::numeric_limits<int>::max())));
            return 0;
        }
        wheelDeltaRemainder_ += GET_WHEEL_DELTA_WPARAM(wParam);
        const int notches = wheelDeltaRemainder_ / WHEEL_DELTA;
        wheelDeltaRemainder_ %= WHEEL_DELTA;
        if (notches == 0) return 0;
        UINT configuredLines = 3;
        static_cast<void>(SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &configuredLines, 0));
        if (configuredLines == 0) return 0;
        const std::size_t visible = std::max<std::size_t>(1, renderer_.Metrics().visibleRows);
        const std::size_t steps = static_cast<std::size_t>(std::abs(notches));
        const std::size_t amount = configuredLines == WHEEL_PAGESCROLL ? visible * steps :
            static_cast<std::size_t>(configuredLines) * steps;
        if (notches > 0) ScrollToFirstRow(firstRow_ - std::min(firstRow_, amount));
        else ScrollToFirstRow(firstRow_ + amount);
        return 0;
    }
    case WM_SIZE:
        StopAutoScroll();
        if (renderer_.Metrics().width > 0) {
            renderer_.Resize(LOWORD(lParam), HIWORD(lParam));
            if (!renderer_.Metrics().textPaneVisible) {
                hexPane_ = true;
                lowNibble_ = false;
            }
            EnsureActiveTabVisible();
            if (!ioBusy_) UpdateScrollBar();
            ScheduleRender(R2PRRegion::Full);
        }
        return 0;
    case WM_DPICHANGED: {
        StopAutoScroll();
        // Accept the operating system's suggested rectangle to avoid resize loops
        // when crossing monitors with different scaling factors.
        const RECT* suggested = reinterpret_cast<RECT*>(lParam);
        renderer_.DpiChanged(HIWORD(wParam));
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        EnsureActiveTabVisible();
        ScheduleRender(R2PRRegion::Full); return 0;
    }
    case WM_KILLFOCUS:
        UpdateTabHover(std::nullopt);
        StopAutoScroll();
        return 0;
    case WM_SETCURSOR:
        if (ioBusy_ && LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            return TRUE;
        }
        if (autoScrollActive_ && LOWORD(lParam) == HTCLIENT) {
            // WM_SETCURSOR may follow a non-client transition even when the
            // logical direction did not change, so reinstall the shared cursor.
            UpdateAutoScrollCursor(true);
            return TRUE;
        }
        break;
    case WM_SETTINGCHANGE:
        // System-language preference is resolved again only when Windows reports
        // a settings change; explicit Japanese/English choices remain stable.
        if (profile_.language == LanguagePreference::System) {
            const UiLanguage current = ResolveLanguage(profile_.language);
            if (current != resolvedLanguage_) {
                resolvedLanguage_ = current;
                searchDialog_.language = current;
                RebuildMenu();
                if (!ioBusy_) UpdateTitle();
                ScheduleRender(R2PRRegion::Full);
            }
        }
        ApplyTheme(); return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        // Accent changes update selection, focus, caret, primary controls, and
        // UAH menu interaction colors without requiring an application restart.
        ApplyTheme(); return 0;
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED: ApplyTheme(); return 0;
    case WM_NCPAINT:
    case WM_NCACTIVATE: {
        const LRESULT result = DefWindowProcW(window_, message, wParam, lParam);
        ThemeMenu::DrawMenuBorder(window_, profile_.ResolvePalette());
        return result;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        std::vector<wchar_t> path(32768, L'\0');
        std::vector<std::filesystem::path> paths;
        // Match the IPC transaction cap so a synthetic or accidental enormous
        // drop cannot create an unbounded number of tabs in one dispatch.
        const UINT count = std::min<UINT>(DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0), 256);
        try { paths.reserve(count); } catch (...) {}
        for (UINT index = 0; index < count; ++index) {
            if (!DragQueryFileW(drop, index, path.data(), static_cast<UINT>(path.size()))) continue;
            try { paths.emplace_back(path.data()); }
            catch (...) { break; }
        }
        DragFinish(drop);
        QueueOpenPaths(std::move(paths));
        return 0;
    }
    case WM_INITMENUPOPUP: UpdateMenuChecks(); return 0;
    case WM_TIMER:
        if (wParam == kIoProgressTimer) {
            ioProgressPhase_ = std::fmod(ioProgressPhase_ + 1.0f, 12.0f);
            ScheduleRender(R2PRRegion::IoProgress);
            return 0;
        }
        if (ioBusy_) return 0;
        if (wParam == kTabTooltipTimer) {
            KillTimer(window_, kTabTooltipTimer);
            if (tabHoverCandidate_ && !tabDragging_) {
                tabTooltipTab_ = tabHoverCandidate_;
                ScheduleRender(R2PRRegion::Full);
            }
            return 0;
        }
        if (wParam == kAutoScrollTimer) {
            TickAutoScroll();
            return 0;
        }
        if (wParam == kExternalChangeTimer) {
            CheckExternalChanges();
            return 0;
        }
        if (wParam == kSearchRefreshTimer) {
            KillTimer(window_, kSearchRefreshTimer);
            StartAsyncSearch(false, false, false);
            return 0;
        }
        if (wParam == kRecoveryTimer) {
            KillTimer(window_, kRecoveryTimer);
            StartRecoveryCheckpoint();
            return 0;
        }
        break;
    case WM_PAINT: {
        // BeginPaint validates the update region. Actual pixels are produced later
        // by the coalesced R2PR message, never by the WM_PAINT handler itself.
        PAINTSTRUCT paint{}; BeginPaint(window_, &paint); EndPaint(window_, &paint);
        ScheduleRenderRect(paint.rcPaint); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_APP_SEARCH_COMPLETE:
        HandleSearchCompletion();
        return 0;
    case WM_APP_IO_COMPLETE:
        // RunIoOperation waits on a kernel event; this message is intentionally
        // side-effect free and merely wakes conventional message observers.
        return 0;
    case WM_APP_PROCESS_PENDING_OPEN:
        ProcessPendingOpenPaths();
        return 0;
    case WM_APP_COLLECT_DETACHED:
        CollectClosedDetachedHosts();
        return 0;
    case WM_APP_R2PR_RENDER:
        // A device-loss recovery can enqueue an initialization redraw. Repost one
        // frame if Renderer still owns damage after this presentation.
        renderMessageQueued_ = false;
        renderer_.Render(MakeRenderModel());
        if (renderer_.HasPendingFrame() && !renderMessageQueued_)
            renderMessageQueued_ = PostMessageW(window_, WM_APP_R2PR_RENDER, 0, 0) != FALSE;
        return 0;
    case WM_GETMINMAXINFO: {
        auto* minmax = reinterpret_cast<MINMAXINFO*>(lParam);
        minmax->ptMinTrackSize = {720, 420}; return 0;
    }
    case WM_QUERYENDSESSION:
        // Session shutdown cannot safely block on a modal save workflow. A dirty
        // document vetoes this query and the pre-registered Restart Manager text
        // tells Windows exactly why. Clean documents allow shutdown immediately.
        if (ioBusy_) return FALSE;
        if (HasDirtyTabs()) {
            // Session shutdown is infrequent and explicitly data-sensitive, so
            // finish the active generation before vetoing the first query.
            if (document_.Dirty() && !recoveryId_.empty()) StartRecoveryCheckpoint();
            UpdateShutdownBlockReason();
            return FALSE;
        }
        ClearShutdownBlockReason();
        return TRUE;
    case WM_ENDSESSION:
        // Destroy the registration after either a completed or canceled session
        // transition; a canceled shutdown recreates it for the still-dirty file.
        ClearShutdownBlockReason();
        if (!wParam) UpdateShutdownBlockReason();
        return 0;
    case WM_CLOSE:
        if (ioBusy_) return 0;
        StopAutoScroll();
        if (primaryWindow_) {
            // The main host owns process lifetime. Confirm every document across
            // every sub window before destroying anything, then close all HWNDs
            // as one transaction so no orphaned sub host survives its owner.
            if (closingForElevation_ || ConfirmAllWindows()) DestroyAllWindows();
        } else if (closingForElevation_ || ConfirmAllTabs()) {
            DestroyWindow(window_);
        }
        return 0;
    case WM_DESTROY:
        // Release the heavyweight swap-chain/device graph synchronously. The
        // remaining controller is destroyed by the root on a later message so
        // this window procedure never returns through an invalid `this`.
        renderer_.DiscardDeviceResources();
        KillTimer(window_, kIoProgressTimer);
        KillTimer(window_, kAutoScrollTimer);
        autoScrollActive_ = false;
        KillTimer(window_, kExternalChangeTimer);
        CancelAsyncSearch();
        StopRecoveryWriter();
        ClearShutdownBlockReason();
        BinEditApp* detachedOwner = !primaryWindow_ ? root_ : nullptr;
        std::erase(gEditorWindows, this);
        window_ = nullptr;
        if (detachedOwner && detachedOwner != this && detachedOwner->window_)
            PostMessageW(detachedOwner->window_, WM_APP_COLLECT_DETACHED, 0, 0);
        if (gEditorWindows.empty()) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}
