// Implements early process hardening and the single-instance IPC trust check.
// All routines are noexcept because they run at Win32 callback/startup boundaries.

#include "Security.h"

#include <windows.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace {
class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~UniqueHandle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
private:
    HANDLE value_{};
};

bool QueryTokenBuffer(HANDLE token, TOKEN_INFORMATION_CLASS informationClass,
    std::vector<std::byte>& buffer) noexcept {
    DWORD required{};
    GetTokenInformation(token, informationClass, nullptr, 0, &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) return false;
    try {
        buffer.resize(required);
    } catch (...) {
        return false;
    }
    return GetTokenInformation(token, informationClass, buffer.data(), required, &required) != FALSE;
}

bool OpenQueryToken(DWORD processId, UniqueHandle& process, UniqueHandle& token) noexcept {
    process = UniqueHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!process) return false;
    HANDLE rawToken{};
    if (!OpenProcessToken(process.Get(), TOKEN_QUERY, &rawToken)) return false;
    token = UniqueHandle(rawToken);
    return true;
}

bool SameUser(HANDLE left, HANDLE right) noexcept {
    std::vector<std::byte> leftBuffer;
    std::vector<std::byte> rightBuffer;
    if (!QueryTokenBuffer(left, TokenUser, leftBuffer) ||
        !QueryTokenBuffer(right, TokenUser, rightBuffer)) return false;
    const auto* leftUser = reinterpret_cast<const TOKEN_USER*>(leftBuffer.data());
    const auto* rightUser = reinterpret_cast<const TOKEN_USER*>(rightBuffer.data());
    return IsValidSid(leftUser->User.Sid) && IsValidSid(rightUser->User.Sid) &&
        EqualSid(leftUser->User.Sid, rightUser->User.Sid) != FALSE;
}

bool QueryIntegrity(HANDLE token, DWORD& level) noexcept {
    std::vector<std::byte> buffer;
    if (!QueryTokenBuffer(token, TokenIntegrityLevel, buffer)) return false;
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    if (!IsValidSid(label->Label.Sid)) return false;
    const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (count == 0) return false;
    level = *GetSidSubAuthority(label->Label.Sid, count - 1);
    return true;
}

bool IsTrustedPeerProcess(DWORD peerProcessId, std::uint64_t& creationTime) noexcept {
    if (!peerProcessId || peerProcessId == GetCurrentProcessId()) return false;
    DWORD peerSession{};
    DWORD currentSession{};
    if (!ProcessIdToSessionId(peerProcessId, &peerSession) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &currentSession) ||
        peerSession != currentSession) return false;

    UniqueHandle peerProcess;
    UniqueHandle peerToken;
    UniqueHandle currentProcess;
    UniqueHandle currentToken;
    if (!OpenQueryToken(peerProcessId, peerProcess, peerToken) ||
        !OpenQueryToken(GetCurrentProcessId(), currentProcess, currentToken) ||
        !SameUser(peerToken.Get(), currentToken.Get())) return false;
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(peerProcess.Get(), &created, &exited, &kernel, &user)) return false;
    creationTime = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
        created.dwLowDateTime;
    DWORD peerIntegrity{};
    DWORD currentIntegrity{};
    return QueryIntegrity(peerToken.Get(), peerIntegrity) &&
        QueryIntegrity(currentToken.Get(), currentIntegrity) &&
        currentIntegrity >= SECURITY_MANDATORY_MEDIUM_RID &&
        peerIntegrity >= currentIntegrity;
}
}

namespace Security {
void HardenCurrentProcess() noexcept {
    // Remove the current working directory from implicit DLL resolution while
    // retaining application-local deployment and trusted System32 components.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY handles{};
    handles.RaiseExceptionOnInvalidHandleReference = 1;
    handles.HandleExceptionsPermanentlyEnabled = 1;
    SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &handles, sizeof(handles));

    // Disable AppInit DLLs and similarly legacy injection surfaces. This does
    // not prohibit signed GPU drivers, IMEs, accessibility, or WinRT activation.
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensions{};
    extensions.DisableExtensionPoints = 1;
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &extensions, sizeof(extensions));

    // Reject images from remote shares and low-integrity locations, then prefer
    // System32 for ambiguous base names. Explicit application-directory images
    // remain available for normal side-by-side deployment.
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY images{};
    images.NoRemoteImages = 1;
    images.NoLowMandatoryLabelImages = 1;
    images.PreferSystem32Images = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &images, sizeof(images));
}

[[noreturn]] void FailFast(const wchar_t* boundary) noexcept {
    OutputDebugStringW(L"BinEdit: an exception escaped a Win32 callback boundary: ");
    OutputDebugStringW(boundary ? boundary : L"unknown");
    OutputDebugStringW(L"\r\n");

    // RaiseFailFastException bypasses ordinary exception handlers and asks WER
    // to capture the fault. TerminateProcess is a last-resort fallback for an
    // environment that unexpectedly returns from the fail-fast request.
    RaiseFailFastException(nullptr, nullptr, 0);
    TerminateProcess(GetCurrentProcess(), ERROR_UNHANDLED_EXCEPTION);
    __assume(false);
}

bool CaptureTrustedWindowPeer(HWND window, TrustedWindowPeer& peer) noexcept {
    peer = {};
    if (!window || !IsWindow(window)) return false;
    DWORD processId{};
    GetWindowThreadProcessId(window, &processId);
    std::uint64_t creationTime{};
    if (!IsTrustedPeerProcess(processId, creationTime)) return false;

    // Re-read the mapping after token/process inspection. An HWND destroyed and
    // recycled during that work must not inherit the previous peer's trust.
    DWORD confirmedProcessId{};
    if (!IsWindow(window) || !GetWindowThreadProcessId(window, &confirmedProcessId) ||
        confirmedProcessId != processId) return false;
    peer = {window, processId, creationTime};
    return true;
}

bool RevalidateTrustedWindowPeer(const TrustedWindowPeer& peer) noexcept {
    if (!peer.window || !peer.processId || !IsWindow(peer.window)) return false;
    DWORD currentProcessId{};
    if (!GetWindowThreadProcessId(peer.window, &currentProcessId) ||
        currentProcessId != peer.processId) return false;
    std::uint64_t currentCreationTime{};
    if (!IsTrustedPeerProcess(currentProcessId, currentCreationTime) ||
        currentCreationTime != peer.processCreationTime) return false;
    DWORD confirmedProcessId{};
    return IsWindow(peer.window) &&
        GetWindowThreadProcessId(peer.window, &confirmedProcessId) != 0 &&
        confirmedProcessId == peer.processId;
}
}
