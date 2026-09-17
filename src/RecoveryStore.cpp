// Implements bounded, integrity-checked recovery records and private storage.
// The format is fixed-width little endian because records are process-local to
// Windows; explicit decoding still avoids compiler packing and alignment rules.

#include "RecoveryStore.h"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace {
constexpr std::size_t kHeaderSize = 64;
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kDigestOffset = 32;
constexpr std::size_t kDigestSize = 32;
constexpr std::size_t kMaximumPathCharacters = 32766;
constexpr std::array<std::uint8_t, 8> kMagic{'B', 'N', 'R', 'E', 'C', 'V', '1', 0};
constexpr wchar_t kRecordExtension[] = L".binrecovery";

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) { Reset(); value_ = std::exchange(other.value_, nullptr); }
        return *this;
    }
    void Reset(HANDLE value = nullptr) noexcept {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
private:
    HANDLE value_{};
};

class UniqueFindHandle final {
public:
    explicit UniqueFindHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~UniqueFindHandle() { if (value_ != INVALID_HANDLE_VALUE) FindClose(value_); }
    UniqueFindHandle(const UniqueFindHandle&) = delete;
    UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

void Put32(std::span<std::uint8_t> output, std::size_t at, std::uint32_t value) noexcept {
    for (unsigned byte = 0; byte < 4; ++byte) output[at + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
}

void Put64(std::span<std::uint8_t> output, std::size_t at, std::uint64_t value) noexcept {
    for (unsigned byte = 0; byte < 8; ++byte) output[at + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
}

std::uint32_t Get32(std::span<const std::uint8_t> input, std::size_t at) noexcept {
    std::uint32_t value{};
    for (unsigned byte = 0; byte < 4; ++byte) value |= static_cast<std::uint32_t>(input[at + byte]) << (byte * 8);
    return value;
}

std::uint64_t Get64(std::span<const std::uint8_t> input, std::size_t at) noexcept {
    std::uint64_t value{};
    for (unsigned byte = 0; byte < 8; ++byte) value |= static_cast<std::uint64_t>(input[at + byte]) << (byte * 8);
    return value;
}

bool IsValidId(std::wstring_view id) noexcept {
    return id.size() == 32 && std::ranges::all_of(id, [](wchar_t character) {
        return (character >= L'0' && character <= L'9') ||
            (character >= L'a' && character <= L'f');
    });
}

bool IsRecoveryTemporaryName(std::wstring_view name) noexcept {
    constexpr std::size_t idLength = 32;
    constexpr std::size_t expectedLength = idLength + 1 + idLength + 4;
    return name.size() == expectedLength && name[idLength] == L'.' &&
        name.ends_with(L".tmp") && IsValidId(name.substr(0, idLength)) &&
        IsValidId(name.substr(idLength + 1, idLength));
}

bool WriteAll(HANDLE file, std::span<const std::uint8_t> bytes,
    std::stop_token stopToken, DWORD& error) noexcept {
    std::size_t done{};
    while (done < bytes.size()) {
        if (stopToken.stop_requested()) { error = ERROR_CANCELLED; return false; }
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 4u * 1024u * 1024u));
        DWORD written{};
        if (!WriteFile(file, bytes.data() + done, chunk, &written, nullptr) || written != chunk) {
            error = GetLastError() == ERROR_SUCCESS ? ERROR_WRITE_FAULT : GetLastError();
            return false;
        }
        done += written;
    }
    return true;
}

bool ReadAll(HANDLE file, std::span<std::uint8_t> bytes) noexcept {
    std::size_t done{};
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 4u * 1024u * 1024u));
        DWORD read{};
        if (!ReadFile(file, bytes.data() + done, chunk, &read, nullptr) || read == 0) return false;
        done += read;
    }
    return true;
}

bool ComputeDigest(std::span<const std::uint8_t> metadata,
    std::span<const std::uint8_t> pathBytes, std::span<const std::uint8_t> data,
    std::array<std::uint8_t, kDigestSize>& digest, std::stop_token stopToken = {}) noexcept {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::vector<std::uint8_t> object;
    bool success = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    DWORD objectSize{};
    DWORD returned{};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0) < 0) goto cleanup;
    try { object.resize(objectSize); } catch (...) { goto cleanup; }
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) goto cleanup;
    for (const auto input : {metadata, pathBytes, data}) {
        std::size_t done{};
        while (done < input.size()) {
            if (stopToken.stop_requested()) goto cleanup;
            const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(input.size() - done, 4u * 1024u * 1024u));
            if (BCryptHashData(hash, const_cast<PUCHAR>(input.data() + done), chunk, 0) < 0) goto cleanup;
            done += chunk;
        }
    }
    success = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
cleanup:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

bool ComputeSnapshotDigest(std::span<const std::uint8_t> metadata,
    std::span<const std::uint8_t> pathBytes, const ImmutableByteSnapshot& snapshot,
    std::array<std::uint8_t, kDigestSize>& digest,
    std::stop_token stopToken = {}) noexcept {
    if (!snapshot) return false;
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::vector<std::uint8_t> object;
    std::vector<std::uint8_t> buffer;
    bool success = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    DWORD objectSize{};
    DWORD returned{};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0) < 0) goto cleanup;
    try {
        object.resize(objectSize);
        buffer.resize(std::min<std::size_t>(snapshot->Size(), 4u * 1024u * 1024u));
    } catch (...) { goto cleanup; }
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0)
        goto cleanup;
    for (const auto input : {metadata, pathBytes}) {
        std::size_t done{};
        while (done < input.size()) {
            if (stopToken.stop_requested()) goto cleanup;
            const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(
                input.size() - done, 4u * 1024u * 1024u));
            if (BCryptHashData(hash, const_cast<PUCHAR>(input.data() + done), chunk, 0) < 0)
                goto cleanup;
            done += chunk;
        }
    }
    for (std::size_t offset = 0; offset < snapshot->Size();) {
        if (stopToken.stop_requested()) goto cleanup;
        const std::size_t chunk = std::min(buffer.size(), snapshot->Size() - offset);
        if (!snapshot->CopyRange(offset, std::span<std::uint8_t>(buffer).first(chunk)) ||
            BCryptHashData(hash, buffer.data(), static_cast<ULONG>(chunk), 0) < 0) goto cleanup;
        offset += chunk;
    }
    success = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
cleanup:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

bool WriteSnapshotAll(HANDLE file, const ImmutableByteSnapshot& snapshot,
    std::stop_token stopToken, DWORD& error) noexcept {
    if (!snapshot) { error = ERROR_INVALID_PARAMETER; return false; }
    std::vector<std::uint8_t> buffer;
    try { buffer.resize(std::min<std::size_t>(snapshot->Size(), 4u * 1024u * 1024u)); }
    catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    for (std::size_t offset = 0; offset < snapshot->Size();) {
        if (stopToken.stop_requested()) { error = ERROR_CANCELLED; return false; }
        const std::size_t chunk = std::min(buffer.size(), snapshot->Size() - offset);
        if (!snapshot->CopyRange(offset, std::span<std::uint8_t>(buffer).first(chunk))) {
            error = ERROR_READ_FAULT;
            return false;
        }
        if (!WriteAll(file, std::span<const std::uint8_t>(buffer).first(chunk),
            stopToken, error)) return false;
        offset += chunk;
    }
    return true;
}

bool ApplyPrivateAcl(const std::filesystem::path& directory) noexcept {
    UniqueHandle token;
    HANDLE rawToken{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return false;
    token.Reset(rawToken);
    DWORD required{};
    GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) return false;
    std::vector<std::byte> userBuffer;
    try { userBuffer.resize(required); } catch (...) { return false; }
    if (!GetTokenInformation(token.Get(), TokenUser, userBuffer.data(), required, &required)) return false;
    auto* user = reinterpret_cast<TOKEN_USER*>(userBuffer.data());
    if (!IsValidSid(user->User.Sid)) return false;

    std::array<std::byte, SECURITY_MAX_SID_SIZE> systemBuffer{};
    DWORD systemSize = static_cast<DWORD>(systemBuffer.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemBuffer.data(), &systemSize)) return false;

    std::array<EXPLICIT_ACCESSW, 2> access{};
    const auto configure = [](EXPLICIT_ACCESSW& entry, PSID sid) {
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    };
    configure(access[0], user->User.Sid);
    configure(access[1], systemBuffer.data());
    PACL acl{};
    if (SetEntriesInAclW(static_cast<ULONG>(access.size()), access.data(), nullptr, &acl) != ERROR_SUCCESS) return false;
    const DWORD status = SetNamedSecurityInfoW(const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return status == ERROR_SUCCESS;
}

bool EnsurePrivateDirectory(std::filesystem::path& directory) noexcept {
    directory = RecoveryStore::Directory();
    if (directory.empty()) return false;
    try {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return false;
    } catch (...) { return false; }
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    if (!ApplyPrivateAcl(directory)) return false;
    // On EFS-capable volumes this marks the directory so current and future
    // recovery files are encrypted transparently for the signed-in user. Home
    // editions and non-NTFS volumes may reject EFS; the protected DACL and digest
    // remain mandatory even when this best-effort defense is unavailable.
    if (!(attributes & FILE_ATTRIBUTE_ENCRYPTED)) static_cast<void>(EncryptFileW(directory.c_str()));
    return true;
}

std::filesystem::path RecordPath(const std::filesystem::path& directory,
    std::wstring_view id) {
    return directory / (std::wstring(id) + kRecordExtension);
}

std::optional<RecoveryStore::Record> LoadRecord(const std::filesystem::path& path,
    std::wstring id) noexcept {
    try {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            return std::nullopt;
        UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file) return std::nullopt;
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file.Get(), &length) || length.QuadPart < static_cast<LONGLONG>(kHeaderSize)) return std::nullopt;

        std::array<std::uint8_t, kHeaderSize> header{};
        if (!ReadAll(file.Get(), header) || !std::equal(kMagic.begin(), kMagic.end(), header.begin()) ||
            Get32(header, 8) != kFormatVersion) return std::nullopt;
        const std::uint32_t pathByteCount = Get32(header, 12);
        const std::uint64_t dataByteCount = Get64(header, 16);
        if ((pathByteCount % sizeof(wchar_t)) != 0 ||
            pathByteCount / sizeof(wchar_t) > kMaximumPathCharacters ||
            dataByteCount > std::numeric_limits<std::size_t>::max() ||
            dataByteCount > static_cast<std::uint64_t>(PTRDIFF_MAX)) return std::nullopt;
        const std::uint64_t expectedLength = kHeaderSize + static_cast<std::uint64_t>(pathByteCount) + dataByteCount;
        if (expectedLength > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()) ||
            length.QuadPart != static_cast<LONGLONG>(expectedLength)) return std::nullopt;

        std::wstring originalPath(pathByteCount / sizeof(wchar_t), L'\0');
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(dataByteCount));
        auto pathBytes = std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(originalPath.data()), pathByteCount);
        if (!ReadAll(file.Get(), pathBytes) || !ReadAll(file.Get(), bytes) ||
            originalPath.find(L'\0') != std::wstring::npos) return std::nullopt;

        std::array<std::uint8_t, kDigestSize> actual{};
        if (!ComputeDigest(std::span<const std::uint8_t>(header).first(kDigestOffset), pathBytes, bytes, actual))
            return std::nullopt;
        // Deliberately inspect every byte without an early exit. Although this
        // digest protects integrity rather than a secret MAC key, preserving a
        // non-short-circuiting comparison avoids accidental timing regressions.
        unsigned difference{};
        for (std::size_t index = 0; index < actual.size(); ++index)
            difference |= actual[index] ^ header[kDigestOffset + index];
        if (difference != 0) return std::nullopt;
        return RecoveryStore::Record{std::move(id), std::filesystem::path(std::move(originalPath)),
            std::move(bytes), Get64(header, 24)};
    } catch (...) { return std::nullopt; }
}
}

namespace RecoveryStore {
std::filesystem::path Directory() noexcept {
#if defined(BINEDIT_EXPLOIT_TEST)
    // Keep deliberately corrupted test records out of the user's real recovery
    // directory. The macro is defined only by BinEdit.ExploitTest.
    std::array<wchar_t, 32768> testRoot{};
    const DWORD testLength = GetEnvironmentVariableW(
        L"BINEDIT_EXPLOIT_TEST_LOCALAPPDATA", testRoot.data(),
        static_cast<DWORD>(testRoot.size()));
    if (testLength > 0 && testLength < testRoot.size()) {
        try { return std::filesystem::path(testRoot.data()) / L"BinEdit" / L"Recovery"; }
        catch (...) { return {}; }
    }
#endif
    PWSTR knownFolder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &knownFolder))) return {};
    std::filesystem::path result;
    try { result = std::filesystem::path(knownFolder) / L"BinEdit" / L"Recovery"; }
    catch (...) { result.clear(); }
    CoTaskMemFree(knownFolder);
    return result;
}

std::wstring NewId() {
    std::array<std::uint8_t, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring id(random.size() * 2, L'0');
    for (std::size_t index = 0; index < random.size(); ++index) {
        id[index * 2] = digits[random[index] >> 4];
        id[index * 2 + 1] = digits[random[index] & 0x0fu];
    }
    return id;
}

bool Write(std::wstring_view id, const std::filesystem::path& originalPath,
    std::span<const std::uint8_t> bytes, std::stop_token stopToken, DWORD& error) noexcept {
    error = ERROR_SUCCESS;
    if (!IsValidId(id) || originalPath.native().size() > kMaximumPathCharacters) {
        error = ERROR_INVALID_PARAMETER; return false;
    }
    std::filesystem::path temporary;
    try {
        std::filesystem::path directory;
        if (!EnsurePrivateDirectory(directory)) { error = ERROR_ACCESS_DENIED; return false; }
        const std::size_t pathByteCount = originalPath.native().size() * sizeof(wchar_t);
        auto pathBytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(originalPath.c_str()), pathByteCount);
        std::array<std::uint8_t, kHeaderSize> header{};
        std::copy(kMagic.begin(), kMagic.end(), header.begin());
        Put32(header, 8, kFormatVersion);
        Put32(header, 12, static_cast<std::uint32_t>(pathByteCount));
        Put64(header, 16, static_cast<std::uint64_t>(bytes.size()));
        FILETIME time{};
        GetSystemTimeAsFileTime(&time);
        Put64(header, 24, (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime);
        std::array<std::uint8_t, kDigestSize> digest{};
        if (!ComputeDigest(std::span<const std::uint8_t>(header).first(kDigestOffset),
            pathBytes, bytes, digest, stopToken)) {
            error = stopToken.stop_requested() ? ERROR_CANCELLED : ERROR_INVALID_DATA;
            return false;
        }
        std::copy(digest.begin(), digest.end(), header.begin() + kDigestOffset);

        const auto finalPath = RecordPath(directory, id);
        UniqueHandle file;
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            const std::wstring suffix = NewId();
            if (suffix.empty()) { error = ERROR_GEN_FAILURE; return false; }
            temporary = directory / (std::wstring(id) + L"." + suffix + L".tmp");
            file.Reset(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr));
            if (file) break;
            error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return false;
        }
        if (!file) return false;
        const bool complete = WriteAll(file.Get(), header, stopToken, error) &&
            WriteAll(file.Get(), pathBytes, stopToken, error) &&
            WriteAll(file.Get(), bytes, stopToken, error) &&
            !stopToken.stop_requested() && FlushFileBuffers(file.Get());
        if (!complete && error == ERROR_SUCCESS) error = stopToken.stop_requested() ? ERROR_CANCELLED : GetLastError();
        file.Reset();
        if (!complete) { DeleteFileW(temporary.c_str()); return false; }
        if (!MoveFileExW(temporary.c_str(), finalPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = GetLastError(); DeleteFileW(temporary.c_str()); return false;
        }
        return true;
    } catch (...) {
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
}

bool Write(std::wstring_view id, const std::filesystem::path& originalPath,
    const ImmutableByteSnapshot& snapshot, std::stop_token stopToken,
    DWORD& error) noexcept {
    error = ERROR_SUCCESS;
    if (!snapshot || !IsValidId(id) ||
        originalPath.native().size() > kMaximumPathCharacters) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }
    std::filesystem::path temporary;
    try {
        std::filesystem::path directory;
        if (!EnsurePrivateDirectory(directory)) { error = ERROR_ACCESS_DENIED; return false; }
        const std::size_t pathByteCount = originalPath.native().size() * sizeof(wchar_t);
        auto pathBytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(originalPath.c_str()), pathByteCount);
        std::array<std::uint8_t, kHeaderSize> header{};
        std::copy(kMagic.begin(), kMagic.end(), header.begin());
        Put32(header, 8, kFormatVersion);
        Put32(header, 12, static_cast<std::uint32_t>(pathByteCount));
        Put64(header, 16, static_cast<std::uint64_t>(snapshot->Size()));
        FILETIME time{};
        GetSystemTimeAsFileTime(&time);
        Put64(header, 24, (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
            time.dwLowDateTime);
        std::array<std::uint8_t, kDigestSize> digest{};
        if (!ComputeSnapshotDigest(std::span<const std::uint8_t>(header).first(kDigestOffset),
            pathBytes, snapshot, digest, stopToken)) {
            error = stopToken.stop_requested() ? ERROR_CANCELLED : ERROR_INVALID_DATA;
            return false;
        }
        std::copy(digest.begin(), digest.end(), header.begin() + kDigestOffset);

        const auto finalPath = RecordPath(directory, id);
        UniqueHandle file;
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            const std::wstring suffix = NewId();
            if (suffix.empty()) { error = ERROR_GEN_FAILURE; return false; }
            temporary = directory / (std::wstring(id) + L"." + suffix + L".tmp");
            file.Reset(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (file) break;
            error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return false;
        }
        if (!file) return false;
        const bool complete = WriteAll(file.Get(), header, stopToken, error) &&
            WriteAll(file.Get(), pathBytes, stopToken, error) &&
            WriteSnapshotAll(file.Get(), snapshot, stopToken, error) &&
            !stopToken.stop_requested() && FlushFileBuffers(file.Get());
        if (!complete && error == ERROR_SUCCESS)
            error = stopToken.stop_requested() ? ERROR_CANCELLED : GetLastError();
        file.Reset();
        if (!complete) { DeleteFileW(temporary.c_str()); return false; }
        if (!MoveFileExW(temporary.c_str(), finalPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = GetLastError();
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    } catch (...) {
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
}

void Remove(std::wstring_view id) noexcept {
    if (!IsValidId(id)) return;
    try {
        const auto directory = Directory();
        if (!directory.empty()) DeleteFileW(RecordPath(directory, id).c_str());
    } catch (...) {}
}

std::vector<Record> Enumerate() noexcept {
    std::vector<Record> records;
    try {
        std::filesystem::path directory;
        if (!EnsurePrivateDirectory(directory)) return records;

        // A forced process termination cannot execute a worker's cleanup path.
        // Only the exact two-random-ID temporary grammar is removed; malformed
        // final records remain available for diagnosis and are never loaded.
        WIN32_FIND_DATAW temporaryFound{};
        UniqueFindHandle temporarySearch(FindFirstFileW(
            (directory / L"*.tmp").c_str(), &temporaryFound));
        if (temporarySearch) {
            do {
                if (!(temporaryFound.dwFileAttributes &
                    (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                    IsRecoveryTemporaryName(temporaryFound.cFileName)) {
                    DeleteFileW((directory / temporaryFound.cFileName).c_str());
                }
            } while (FindNextFileW(temporarySearch.Get(), &temporaryFound));
        }

        WIN32_FIND_DATAW found{};
        UniqueFindHandle search(FindFirstFileW(
            (directory / (L"*" + std::wstring(kRecordExtension))).c_str(), &found));
        if (!search) return records;
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring_view name(found.cFileName);
            if (!name.ends_with(kRecordExtension)) continue;
            const std::wstring id(name.substr(0, name.size() - std::size(kRecordExtension) + 1));
            if (!IsValidId(id)) continue;
            if (auto record = LoadRecord(directory / found.cFileName, id)) records.push_back(std::move(*record));
        } while (FindNextFileW(search.Get(), &found));
        std::ranges::sort(records, {}, &Record::timestamp);
    } catch (...) {
        records.clear();
    }
    return records;
}
}
