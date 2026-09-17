// Process entry point, single-instance router, and elevated-save bootstrap.
// Normal launches either acquire the named mutex or forward their paths to the
// existing editor. Elevated relaunches first restore a staged byte-for-byte
// snapshot, then coordinate with the same primary-instance contract.

#include "App.h"
#include "MessageDialog.h"
#include "RecoveryStore.h"
#include "Security.h"

#include <windows.h>
#include <roapi.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
// Holds a validation handle across the subsequent load so another process
// cannot replace or rewrite the staged file between validation and consumption.
class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    UniqueHandle& operator=(UniqueHandle&&) = delete;
    void Reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

bool IsCurrentProcessElevated() noexcept {
    HANDLE rawToken{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return false;
    UniqueHandle token(rawToken);
    TOKEN_ELEVATION elevation{};
    DWORD returned{};
    return GetTokenInformation(token.Get(), TokenElevation, &elevation,
        sizeof(elevation), &returned) && elevation.TokenIsElevated != 0;
}

bool IsHexIdentifier(std::wstring_view value) noexcept {
    return value.size() == 32 && std::ranges::all_of(value, [](wchar_t character) {
        return (character >= L'0' && character <= L'9') ||
            (character >= L'a' && character <= L'f');
    });
}

bool ReadFinalPath(HANDLE handle, std::span<wchar_t> output) noexcept {
    const DWORD length = GetFinalPathNameByHandleW(handle, output.data(),
        static_cast<DWORD>(output.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    return length > 0 && length < output.size();
}

// Accepts only the CNG-named file created by RestartElevatedForSave and only
// when the kernel resolves it immediately beneath this user's temporary folder.
// The returned read handle intentionally denies concurrent writes and deletion.
UniqueHandle OpenValidatedStagingFile(const std::filesystem::path& path) noexcept {
    try {
        const std::wstring name = path.filename().native();
        if (name.size() != 3 + 32 + 4 || !name.starts_with(L"BNE") ||
            !name.ends_with(L".tmp") || !IsHexIdentifier(
                std::wstring_view(name).substr(3, 32))) return {};

        std::vector<wchar_t> tempPath(32768, L'\0');
        const DWORD tempLength = GetTempPathW(static_cast<DWORD>(tempPath.size()), tempPath.data());
        if (tempLength == 0 || tempLength >= tempPath.size()) return {};
        std::wstring tempDirectoryName(tempPath.data(), tempLength);
        UniqueHandle tempDirectory(CreateFileW(tempDirectoryName.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!tempDirectory && tempDirectoryName.size() > 3 &&
            (tempDirectoryName.back() == L'\\' || tempDirectoryName.back() == L'/')) {
            tempDirectoryName.pop_back();
            tempDirectory.Reset(CreateFileW(tempDirectoryName.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        }
        if (!tempDirectory) return {};

        UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!file) return {};
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(file.Get(), FileAttributeTagInfo,
            &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};

        std::vector<wchar_t> finalFile(32768, L'\0');
        std::vector<wchar_t> finalTemp(32768, L'\0');
        if (!ReadFinalPath(file.Get(), finalFile) || !ReadFinalPath(tempDirectory.Get(), finalTemp)) return {};
        const std::filesystem::path resolvedParent =
            std::filesystem::path(finalFile.data()).parent_path().lexically_normal();
        const std::filesystem::path resolvedTemp =
            std::filesystem::path(finalTemp.data()).lexically_normal();
        if (_wcsicmp(resolvedParent.c_str(), resolvedTemp.c_str()) != 0) return {};
        return file;
    } catch (...) {
        return {};
    }
}

bool IsOrdinaryElevatedTarget(const std::filesystem::path& target) noexcept {
    try {
        if (target.empty() || !target.is_absolute() || target.native().size() > 32766) return false;
        const std::wstring_view value(target.native());
        if (value.starts_with(L"\\\\.\\") || value.starts_with(L"\\??\\") ||
            value.starts_with(L"\\\\?\\GLOBALROOT\\")) return false;
        const DWORD attributes = GetFileAttributesW(target.c_str());
        return attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    } catch (...) {
        return false;
    }
}

void ShowElevatedFailure(HINSTANCE instance, DWORD error) {
    Profile profile;
    profile.Load();
    const UiLanguage language = ResolveLanguage(profile.language);
    std::wstring message = GetStrings(language).elevatedSaveFailed;
    message += language == UiLanguage::Japanese ? L"\nエラーコード: " : L"\nError code: ";
    message += std::to_wstring(error);
    ShowMessageDialog(nullptr, instance, L"BinEdit", message, MessageDialogButtons::Ok,
        MessageDialogIcon::Error, profile, language);
}

// Owns the exact named mutex requested by the application contract. The mutex
// remains acquired for the complete primary-process lifetime, which closes the
// startup race before the main HWND has been registered.
class InstanceMutex final {
public:
    InstanceMutex() {
        handle_ = CreateMutexW(nullptr, TRUE, BinEditIdentity::InstanceMutex);
        if (!handle_) return;
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            owned_ = true;
            return;
        }
        // An existing object can be signaled after an abnormal predecessor exit.
        // Acquire it immediately instead of treating a stale kernel object as a
        // live application instance.
        const DWORD wait = WaitForSingleObject(handle_, 0);
        owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }

    ~InstanceMutex() {
        if (owned_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }

    InstanceMutex(const InstanceMutex&) = delete;
    InstanceMutex& operator=(const InstanceMutex&) = delete;

    [[nodiscard]] bool AnotherInstanceOwns() const noexcept { return handle_ && !owned_; }
    [[nodiscard]] bool Valid() const noexcept { return handle_ != nullptr; }

    bool WaitToAcquire(DWORD milliseconds) {
        if (!handle_ || owned_) return true;
        const DWORD wait = WaitForSingleObject(handle_, milliseconds);
        owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        return owned_;
    }

private:
    HANDLE handle_{};
    bool owned_{};
};

HWND FindExistingEditorWindow() {
    // The primary class is unique. A detached-class fallback keeps shell-open
    // functional after the user closes the primary HWND but leaves another
    // detached BinEdit window running.
    if (HWND window = FindWindowW(BinEditIdentity::MainWindowClass, nullptr)) return window;
    return FindWindowW(BinEditIdentity::DetachedWindowClass, nullptr);
}

bool ForwardOpenRequest(std::span<const std::filesystem::path> files) {
    HWND window = FindExistingEditorWindow();
    Security::TrustedWindowPeer targetPeer;
    if (!window || !Security::CaptureTrustedWindowPeer(window, targetPeer)) return false;

    // WM_COPYDATA defines wParam as the sender HWND. A message-only built-in
    // STATIC window gives the receiver a kernel-verified PID/token identity
    // without showing UI or registering another application window class.
    HWND sender = CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!sender) return false;

    if (!files.empty()) {
        // WM_COPYDATA is synchronous, so this double-null-terminated buffer stays
        // alive until the receiving process has copied and validated every path.
        std::wstring payload;
        for (const auto& file : files) {
            std::error_code error;
            const std::filesystem::path absolute = std::filesystem::absolute(file, error);
            const std::wstring value = error ? file.wstring() : absolute.wstring();
            payload.append(value);
            payload.push_back(L'\0');
        }
        payload.push_back(L'\0');
        const std::size_t bytes = payload.size() * sizeof(wchar_t);
        if (bytes > std::numeric_limits<DWORD>::max()) {
            DestroyWindow(sender);
            return false;
        }
        COPYDATASTRUCT transfer{};
        transfer.dwData = BinEditIdentity::OpenFilesCopyDataId;
        transfer.cbData = static_cast<DWORD>(bytes);
        transfer.lpData = payload.data();
        DWORD_PTR accepted{};
        if (!Security::RevalidateTrustedWindowPeer(targetPeer) ||
            !SendMessageTimeoutW(window, WM_COPYDATA, reinterpret_cast<WPARAM>(sender),
            reinterpret_cast<LPARAM>(&transfer), SMTO_ABORTIFHUNG | SMTO_BLOCK, 10000,
            &accepted) || !accepted) {
            DestroyWindow(sender);
            return false;
        }
    }

    DestroyWindow(sender);

    if (!Security::RevalidateTrustedWindowPeer(targetPeer)) return false;

    if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
    else if (!IsWindowVisible(window)) ShowWindowAsync(window, SW_SHOW);
    SetForegroundWindow(window);
    return true;
}

bool ForwardWhenWindowIsReady(std::span<const std::filesystem::path> files, DWORD timeoutMilliseconds) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
    do {
        if (ForwardOpenRequest(files)) return true;
        Sleep(50);
    } while (GetTickCount64() < deadline);
    return false;
}
}

int RunApplication(HINSTANCE instance, int showCommand) {
    // Per-Monitor-V2 must be enabled before creating any HWND so Windows does not
    // bitmap-scale the D2D swap chains or synthesize legacy DPI coordinates.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // RoInitialize supplies the STA COM apartment required by the Win32 common
    // dialogs and also enables Windows Runtime activation. Profile uses that
    // activation to read the current user's UISettings accent color exactly.
    const HRESULT runtime = RoInitialize(RO_INIT_SINGLETHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::filesystem::path> initialFiles;
    std::filesystem::path recovery;
    std::filesystem::path target;
    std::wstring recoveryId;
    bool elevatedRelaunch = false;
    bool internalArgumentsPresent = false;
    bool internalArgumentsValid = true;
    bool sawRecovery = false;
    bool sawTarget = false;
    bool sawRecoveryId = false;
    bool sawElevated = false;
    bool unknownSwitch = false;
    // CommandLineToArgvW applies the same quoting rules used by the elevation
    // relaunch parameters. Unknown switches remain harmless on normal launches
    // and invalidate any privileged internal-protocol invocation.
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--recover") {
            internalArgumentsPresent = true;
            if (sawRecovery || i + 1 >= argc) internalArgumentsValid = false;
            else { sawRecovery = true; recovery = argv[++i]; }
        } else if (arg == L"--target") {
            internalArgumentsPresent = true;
            if (sawTarget || i + 1 >= argc) internalArgumentsValid = false;
            else { sawTarget = true; target = argv[++i]; }
        } else if (arg == L"--recovery-id") {
            internalArgumentsPresent = true;
            if (sawRecoveryId || i + 1 >= argc) internalArgumentsValid = false;
            else { sawRecoveryId = true; recoveryId = argv[++i]; }
        } else if (arg == L"--elevated") {
            internalArgumentsPresent = true;
            if (sawElevated) internalArgumentsValid = false;
            sawElevated = true;
            elevatedRelaunch = true;
        }
        else if (!arg.starts_with(L"--")) initialFiles.emplace_back(argv[i]);
        else unknownSwitch = true;
    }

    // Internal switches form a closed protocol. A partial, duplicated, malformed,
    // or non-elevated invocation must never become a privileged file-copy oracle.
    if (internalArgumentsPresent) {
        const bool validProtocol = internalArgumentsValid && elevatedRelaunch &&
            sawRecovery && sawTarget && IsCurrentProcessElevated() &&
            IsOrdinaryElevatedTarget(target) &&
            (!sawRecoveryId || IsHexIdentifier(recoveryId)) &&
            initialFiles.empty() && !unknownSwitch;
        if (!validProtocol) {
            if (argv) LocalFree(argv);
            ShowElevatedFailure(instance, ERROR_INVALID_PARAMETER);
            if (SUCCEEDED(runtime)) RoUninitialize();
            return 1;
        }

        UniqueHandle validatedStaging = OpenValidatedStagingFile(recovery);
        if (!validatedStaging) {
            if (argv) LocalFree(argv);
            ShowElevatedFailure(instance, ERROR_ACCESS_DENIED);
            if (SUCCEEDED(runtime)) RoUninitialize();
            return 1;
        }

        Profile startupProfile;
        startupProfile.Load();
        const UiLanguage startupLanguage = ResolveLanguage(startupProfile.language);
        std::wstring prompt = GetStrings(startupLanguage).elevatedTargetPrompt;
        prompt += target.wstring();
        const auto answer = ShowMessageDialog(nullptr, instance, L"BinEdit", prompt,
            MessageDialogButtons::YesNo, MessageDialogIcon::Warning,
            startupProfile, startupLanguage);
        if (answer != MessageDialogResult::Yes) {
            if (argv) LocalFree(argv);
            validatedStaging.Reset();
            DeleteFileW(recovery.c_str());
            if (SUCCEEDED(runtime)) RoUninitialize();
            return 0;
        }

        ByteDocument stagedDocument;
        DWORD error{};
        if (!stagedDocument.Load(recovery, error) || !stagedDocument.SaveAs(target, error)) {
            if (argv) LocalFree(argv);
            ShowElevatedFailure(instance, error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error);
            if (SUCCEEDED(runtime)) RoUninitialize();
            return 1;
        }
        validatedStaging.Reset();
        DeleteFileW(recovery.c_str());
        if (!recoveryId.empty()) RecoveryStore::Remove(recoveryId);
        initialFiles.clear();
        initialFiles.push_back(target);
    }
    if (argv) LocalFree(argv);

    InstanceMutex instanceMutex;
    if (!instanceMutex.Valid()) {
        Profile startupProfile;
        startupProfile.Load();
        const UiLanguage startupLanguage = ResolveLanguage(startupProfile.language);
        ShowMessageDialog(nullptr, instance, L"BinEdit",
            GetStrings(startupLanguage).instanceCoordinationFailed,
            MessageDialogButtons::Ok, MessageDialogIcon::Error,
            startupProfile, startupLanguage);
        if (SUCCEEDED(runtime)) RoUninitialize();
        return 1;
    }
    if (instanceMutex.AnotherInstanceOwns()) {
        // An elevated recovery waits first because the original HWND is destroyed
        // immediately after ShellExecuteEx returns. Normal shell launches instead
        // route to the existing host as soon as its primary class appears.
        if (elevatedRelaunch && instanceMutex.WaitToAcquire(5000)) {
            // The prior process has exited; this process now becomes the sole
            // primary instance and opens the recovered target below.
        } else if (ForwardWhenWindowIsReady(initialFiles, 5000)) {
            if (SUCCEEDED(runtime)) RoUninitialize();
            return 0;
        } else if (!instanceMutex.WaitToAcquire(0)) {
            // A live but unresponsive instance still owns the contract. Starting
            // another primary HWND would violate the single-main-window guarantee.
            if (SUCCEEDED(runtime)) RoUninitialize();
            return 0;
        }
    }

    // Windows Error Reporting may restart BinEdit after a crash or hang. Patch
    // and reboot shutdowns are excluded; taskkill /F cannot run callbacks, so
    // the durable recovery record is consumed when the user next launches it.
    RegisterApplicationRestart(L"", RESTART_NO_PATCH | RESTART_NO_REBOOT);

    BinEditApp app(instance);
    const int result = app.Run(showCommand, initialFiles);
    // Every successful RoInitialize call, including S_FALSE, owns one matching
    // RoUninitialize. Failure leaves the application on its fallback color path.
    if (SUCCEEDED(runtime)) RoUninitialize();
    return result;
}

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
    _In_ LPWSTR, _In_ int showCommand) {
    Security::HardenCurrentProcess();
    try {
        return RunApplication(instance, showCommand);
    } catch (...) {
        // No C++ exception may unwind through the operating-system entry thunk.
        // Recovery snapshots are process-independent and survive this fail-fast.
        Security::FailFast(L"wWinMain");
    }
}
