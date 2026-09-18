// Focused executable tests for the UI-independent byte-document layer.
// Tests use real temporary files to exercise the same Win32 I/O and atomic
// replacement path used by the application, rather than mocking persistence.

#include "../src/Document.h"
#include "../src/BitOperations.h"
#include "../src/ColorMath.h"
#include "../src/CommandLineEscaping.h"
#include "../src/TabPolicy.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

// The production build does not expose allocation diagnostics. The test-only
// friend verifies that closing a document returns every owning vector capacity,
// including the outer Undo/Redo arrays, instead of merely setting their sizes to
// zero while retaining the closed file's heap reservation.
struct ByteDocumentTestAccess {
    static std::size_t RetainedVectorBytes(const ByteDocument& document) {
        std::size_t total{};
        total += document.PrivateStorageBytes();
        total += document.undo_.capacity() * sizeof(ByteDocument::Edit);
        total += document.redo_.capacity() * sizeof(ByteDocument::Edit);
        total += document.dirtyRanges_.capacity() * sizeof(ByteDocument::DirtyRange);
        for (const auto& edit : document.undo_)
            total += edit.before.capacity() + edit.after.capacity();
        for (const auto& edit : document.redo_)
            total += edit.before.capacity() + edit.after.capacity();
        return total;
    }
};

namespace {
int failures = 0;

std::vector<std::uint8_t> DocumentBytes(const ByteDocument& document) {
    std::vector<std::uint8_t> bytes(document.Size());
    if (!bytes.empty() && !document.CopyRange(0, bytes)) bytes.clear();
    return bytes;
}

// Accumulate failures so one run reports every independent regression.
void Check(bool condition, const wchar_t* message) {
    if (!condition) { std::wcerr << L"FAILED: " << message << L'\n'; ++failures; }
}

bool SeedFile(const std::filesystem::path& path, const std::array<std::uint8_t, 8>& bytes) {
    // Use Win32 directly to keep the fixture independent from ByteDocument.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{};
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(file);
    return ok;
}

std::uint64_t FileIdentity(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool ok = GetFileInformationByHandle(file, &information) != FALSE;
    CloseHandle(file);
    return ok ? (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
                    information.nFileIndexLow : 0;
}

bool OverwriteExternalByte(const std::filesystem::path& path, std::size_t offset, std::uint8_t value) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD written{};
    const bool ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
        WriteFile(file, &value, 1, &written, nullptr) && written == 1 && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

bool SeedLargeZeroFile(const std::filesystem::path& path, std::uint64_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(size);
    const bool ok = SetFilePointerEx(file, end, nullptr, FILE_BEGIN) &&
        SetEndOfFile(file) && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

bool QuotedArgumentRoundTrips(std::wstring_view value) {
    const auto quoted = QuoteWindowsCommandLineArgument(value);
    if (!quoted) return false;
    std::wstring commandLine = L"BinEditTest.exe ";
    commandLine += *quoted;
    int count{};
    LPWSTR* arguments = CommandLineToArgvW(commandLine.c_str(), &count);
    const bool matches = arguments && count == 2 && value == arguments[1];
    if (arguments) LocalFree(arguments);
    return matches;
}
}

int wmain() {
    // PID-qualified names allow parallel test processes without collisions.
    const auto root = std::filesystem::temp_directory_path();
    const auto input = root / (L"BinEdit.CoreTests." + std::to_wstring(GetCurrentProcessId()) + L".input");
    const auto output = root / (L"BinEdit.CoreTests." + std::to_wstring(GetCurrentProcessId()) + L".output");
    const auto stressOutput = root / (L"BinEdit.CoreTests." + std::to_wstring(GetCurrentProcessId()) + L".stress");
    const auto missingOutput = root / (L"BinEdit.CoreTests." + std::to_wstring(GetCurrentProcessId()) + L".missing");
    const auto largeOutput = root / (L"BinEdit.CoreTests." + std::to_wstring(GetCurrentProcessId()) + L".large");
    const auto readOnlyOutput = root / (L"BinEdit.CoreTests." + std::to_wstring(GetCurrentProcessId()) + L".readonly");
    const auto readOnlySaveAs = root / (L"BinEdit.CoreTests." + std::to_wstring(GetCurrentProcessId()) + L".readonly-save-as");
    const auto fragmentedOutput = root / (L"BinEdit.CoreTests." + std::to_wstring(GetCurrentProcessId()) + L".fragmented");
    const std::array<std::uint8_t, 8> seed{0x4D, 0x5A, 0x7F, 0x00, 0x41, 0x42, 0x43, 0xFF};
    Check(SeedFile(input, seed), L"seed file creation");

    ByteDocument document;
    DWORD error{};
    Check(document.Load(input, error), L"document load");
    Check(document.Size() == seed.size(), L"loaded size");
    Check(document.ByteAt(2) == 0x7F, L"loaded content");

    SearchPatternError parseError{};
    // Pattern coverage includes wildcard matching and invalid-token rejection.
    auto binary = ParseBinaryPattern(L"4D 5A ?? 00", parseError);
    Check(binary.has_value() && binary->bytes.size() == 4, L"binary wildcard parse");
    const auto initialDocumentBytes = DocumentBytes(document);
    Check(binary && FindPattern(initialDocumentBytes, *binary, 0) == 0, L"binary wildcard search");
    Check(!ParseBinaryPattern(L"4D ZZ", parseError), L"invalid binary rejected");

    auto utf8 = EncodeTextPattern(L"日本", TextEncoding::Utf8, parseError);
    Check(utf8.has_value() && utf8->bytes.size() == 6, L"UTF-8 encoding");
    auto utf16 = EncodeTextPattern(L"A", TextEncoding::Utf16Le, parseError);
    Check(utf16.has_value() && utf16->bytes.size() == 2 && utf16->bytes[1].value == 0, L"UTF-16 LE encoding");
    const std::wstring oversizedPattern(65537u, L'A');
    Check(!EncodeTextPattern(oversizedPattern, TextEncoding::Utf8, parseError) &&
        parseError == SearchPatternError::TooLarge, L"oversized text pattern rejected");
    Check(!ParseBinaryPattern(oversizedPattern, parseError) &&
        parseError == SearchPatternError::TooLarge, L"oversized binary pattern rejected");

    // UAC relaunch parameters use one ShellExecute command-line string. Verify
    // the exact parser round trip for spaces, trailing slashes, embedded quotes,
    // and the empty argument rather than relying on file-name restrictions.
    Check(QuotedArgumentRoundTrips(LR"(C:\safe path\file.bin)"),
        L"quoted path with spaces round trip");
    Check(QuotedArgumentRoundTrips(LR"(C:\directory\)"),
        L"quoted trailing backslash round trip");
    Check(QuotedArgumentRoundTrips(LR"(prefix\"quoted\"suffix)"),
        L"quoted embedded quotes round trip");
    Check(QuotedArgumentRoundTrips(L""), L"quoted empty argument round trip");
    const std::wstring embeddedNull{L'a', L'\0', L'b'};
    Check(!QuoteWindowsCommandLineArgument(embeddedNull),
        L"embedded NUL command-line argument rejected");
    Check(!QuoteWindowsCommandLineArgument(std::wstring(32767, L'\\')),
        L"over-limit command-line argument rejected");

    // Tab command availability is intentionally UI-independent. Exercise every
    // branch of the final-primary-tab exception without creating an HWND or
    // entering a modal save-confirmation loop.
    const TabPolicy::State pristineUntitled{1, 0, true, false, false};
    Check(!TabPolicy::CanClose(pristineUntitled, 0),
        L"pristine final primary tab cannot close");
    Check(!TabPolicy::CanDetach(pristineUntitled),
        L"final primary tab cannot detach");

    TabPolicy::State dirtyUntitled = pristineUntitled;
    dirtyUntitled.activeDocumentDirty = true;
    Check(TabPolicy::CanClose(dirtyUntitled, 0),
        L"dirty untitled final primary tab can close");
    Check(!TabPolicy::CanDetach(dirtyUntitled),
        L"dirty untitled exception does not enable detach");

    TabPolicy::State dirtyNamed = dirtyUntitled;
    dirtyNamed.activeDocumentHasPath = true;
    Check(TabPolicy::CanClose(dirtyNamed, 0),
        L"dirty named final primary tab can close into placeholder");
    TabPolicy::State cleanNamed = pristineUntitled;
    cleanNamed.activeDocumentHasPath = true;
    Check(TabPolicy::CanClose(cleanNamed, 0),
        L"clean named final primary tab can close into placeholder");
    Check(!TabPolicy::CanClose(dirtyUntitled, 1),
        L"out-of-range tab cannot close");

    const TabPolicy::State primaryMultiple{2, 0, true, true, false};
    Check(TabPolicy::CanClose(primaryMultiple, 0) &&
        TabPolicy::CanClose(primaryMultiple, 1),
        L"any valid primary tab can close when multiple tabs exist");
    Check(TabPolicy::CanDetach(primaryMultiple),
        L"primary tab can detach when multiple tabs exist");

    const TabPolicy::State detachedFinal{1, 0, false, true, false};
    Check(TabPolicy::CanClose(detachedFinal, 0),
        L"final detached-window tab can close");
    Check(!TabPolicy::CanDetach(detachedFinal),
        L"final detached-window tab cannot create a redundant window");
    Check(!TabPolicy::CanClose({}, 0) && !TabPolicy::CanDetach({}),
        L"empty tab state rejects commands");

    // Parallel search partitions candidate starts rather than byte ranges, so a
    // match crossing a partition boundary is still evaluated exactly once.
    std::vector<std::uint8_t> parallelBytes(2u * 1024u * 1024u, 0x10);
    auto parallelPattern = ParseBinaryPattern(L"DE ?? ?? ??", parseError);
    const std::vector<std::size_t> parallelExpected{
        7u, 262143u, 262144u, 1048575u, parallelBytes.size() - 4u};
    for (const std::size_t offset : parallelExpected) {
        parallelBytes[offset] = 0xDE;
        parallelBytes[offset + 1] = 0xAD;
        parallelBytes[offset + 2] = 0x55;
        parallelBytes[offset + 3] = 0xEF;
    }
    const std::size_t parallelByteCount = parallelBytes.size();
    ByteDocument parallelDocument;
    parallelDocument.RestoreSnapshot(std::move(parallelBytes), {});
    const auto parallelSnapshot = parallelDocument.CreateSnapshot();
    auto parallelResult = FindPatternsParallel(parallelSnapshot, *parallelPattern, 262144u, 100u, {}, 4u);
    Check(!parallelResult.failed && !parallelResult.canceled, L"parallel search completes");
    Check(parallelResult.workerCount >= 1 && parallelResult.workerCount <= 4, L"parallel worker bound");
    Check(parallelResult.offsets == parallelExpected, L"parallel search sorted boundary results");
    Check(parallelResult.next == 262144u, L"parallel find-next start selection");
    auto wrappedResult = FindPatternsParallel(parallelSnapshot, *parallelPattern,
        parallelByteCount, 100u, {}, 4u);
    Check(wrappedResult.next == parallelExpected.front(), L"parallel find-next wrap");

    SearchPattern overlappingPattern;
    overlappingPattern.bytes = {{0xAA, false}, {0xAA, false}};
    std::vector<std::uint8_t> overlappingBytes(300000u, 0xAA);
    ByteDocument overlappingDocument;
    overlappingDocument.RestoreSnapshot(std::move(overlappingBytes), {});
    const auto overlappingSnapshot = overlappingDocument.CreateSnapshot();
    auto cappedResult = FindPatternsParallel(overlappingSnapshot, overlappingPattern, 250000u, 100u, {}, 4u);
    Check(cappedResult.offsets.size() == 100 && cappedResult.offsets.front() == 0 &&
        cappedResult.offsets.back() == 99, L"parallel highlight cap remains globally ordered");
    Check(cappedResult.next == 250000u, L"find-next remains exact beyond highlight cap");
    std::stop_source canceledSearch;
    canceledSearch.request_stop();
    Check(FindPatternsParallel(parallelSnapshot, *parallelPattern, 0, 100u,
        canceledSearch.get_token(), 4u).canceled, L"parallel search cancellation");
    Check(FindPatternsParallel({}, *parallelPattern, 0).failed,
        L"parallel search rejects missing owned snapshot");

    // Exercise the complete edit -> undo -> redo -> atomic persistence chain.
    Check(document.SetByte(2, 0x11), L"edit byte");
    std::size_t changed{};
    Check(document.Undo(changed) && changed == 2 && document.ByteAt(2) == 0x7F, L"undo");
    Check(document.Redo(changed) && document.ByteAt(2) == 0x11, L"redo");
    Check(document.SaveAs(output, error), L"atomic save-as");
    ByteDocument reloaded;
    Check(reloaded.Load(output, error) && reloaded.ByteAt(2) == 0x11, L"saved content reload");

    // Ordinary fixed-size overwrites must preserve the file identity, proving
    // that Save did not replace or rewrite the complete stream.
    const std::uint64_t sparseIdentity = FileIdentity(output);
    Check(sparseIdentity != 0, L"partial-save initial file identity");
    Check(document.SetByte(0, 0x42) && document.SetByte(7, 0x7E),
        L"track disjoint overwrite ranges");
    Check(document.Save(error), L"partial overwrite save");
    Check(FileIdentity(output) == sparseIdentity, L"partial overwrite preserves file identity");
    ByteDocument sparseReloaded;
    Check(sparseReloaded.Load(output, error) && sparseReloaded.ByteAt(0) == 0x42 &&
        sparseReloaded.ByteAt(7) == 0x7E, L"partial overwrite bytes persisted");

    const std::array<std::uint8_t, 2> structuralBytes{0xD1, 0xD2};
    Check(document.InsertRange(3, structuralBytes), L"track structural suffix insertion");
    Check(document.Save(error), L"mapped structural save");
    Check(FileIdentity(output) != 0, L"mapped structural save retains destination");
    Check(sparseReloaded.Load(output, error) && sparseReloaded.Size() == seed.size() + 2 &&
        sparseReloaded.ByteAt(3) == 0xD1 && sparseReloaded.ByteAt(4) == 0xD2,
        L"structural suffix bytes persisted");

    // A mapped data-file section prevents SetEndOfFile from shrinking the same
    // stream. The save path must automatically use bounded-memory atomic
    // publication and retain the exact logical suffix instead of surfacing
    // ERROR_USER_MAPPED_FILE to the UI.
    Check(document.EraseRange(2, 3), L"mapped structural shrink edit");
    Check(document.Save(error), L"mapped structural shrink save");
    Check(sparseReloaded.Load(output, error) && sparseReloaded.Size() == seed.size() - 1 &&
        sparseReloaded.ByteAt(0) == 0x42 && sparseReloaded.ByteAt(1) == seed[1] &&
        sparseReloaded.ByteAt(2) == seed[3], L"mapped structural shrink bytes persisted");

    // The explicit atomic mode retains the normal identity/conflict validation
    // while providing a safe fallback when a durable recovery checkpoint cannot
    // be published before an otherwise sparse save.
    ByteDocument atomicFallback;
    Check(atomicFallback.Load(output, error), L"atomic fallback fixture load");
    const std::uint64_t atomicRevision = atomicFallback.Revision();
    Check(atomicFallback.SetByte(1, 0xC7) && atomicFallback.Revision() != atomicRevision,
        L"document revision advances after mutation");
    Check(atomicFallback.SaveAtomically(error), L"explicit atomic save");
    Check(sparseReloaded.Load(output, error) && sparseReloaded.ByteAt(1) == 0xC7,
        L"explicit atomic save bytes persisted");

    // Exceeding the sparse-range cap must switch to atomic publication instead
    // of widening distant ranges into one enormous non-atomic in-place write.
    constexpr std::size_t fragmentedEdits = 4097;
    ByteDocument fragmented;
    Check(SeedLargeZeroFile(fragmentedOutput, fragmentedEdits * 2u + 1u) &&
        fragmented.Load(fragmentedOutput, error), L"fragmented save fixture load");
    bool fragmentedEditsApplied = true;
    for (std::size_t index = 0; index < fragmentedEdits; ++index) {
        fragmentedEditsApplied = fragmented.SetByte(index * 2u, 0x5A) && fragmentedEditsApplied;
    }
    Check(fragmentedEditsApplied, L"fragmented sparse edits applied");
    Check(fragmented.RequiresAtomicSave(), L"fragmented range cap selects atomic save");
    Check(fragmented.Save(error), L"fragmented atomic fallback save");
    ByteDocument fragmentedReloaded;
    Check(fragmentedReloaded.Load(fragmentedOutput, error) &&
        fragmentedReloaded.ByteAt(0) == 0x5A &&
        fragmentedReloaded.ByteAt((fragmentedEdits - 1u) * 2u) == 0x5A,
        L"fragmented atomic fallback bytes persisted");

    // A save-time stamp check is mandatory even if the asynchronous directory
    // notification has not yet been delivered to the UI timer.
    ByteDocument conflict;
    Check(conflict.Load(output, error), L"external-conflict fixture load");
    Check(conflict.SetByte(0, 0xA1), L"external-conflict local edit");
    Check(OverwriteExternalByte(output, 1, 0xB2), L"external-conflict disk edit");
    DWORD conflictError{};
    Check(conflict.CheckExternalChange(conflictError, true) == ExternalFileState::Changed,
        L"external file change detected");
    Check(!conflict.Save(conflictError) && conflictError == ERROR_FILE_INVALID,
        L"unacknowledged external change blocks save");
    Check(sparseReloaded.Load(output, error) && sparseReloaded.ByteAt(1) == 0xB2,
        L"blocked save preserves external bytes");
    Check(conflict.ContinueAfterExternalChange(conflictError),
        L"external change continuation acknowledged");
    Check(conflict.Save(conflictError), L"acknowledged conflict full fallback save");
    Check(sparseReloaded.Load(output, error) && sparseReloaded.ByteAt(0) == 0xA1,
        L"continued in-memory version persisted");

    // Read-only is a live save-time policy rather than state cached by Load.
    // Setting it after loading must reject without modifying bytes; clearing it
    // must let the same dirty document save without being reopened.
    ByteDocument readOnlyTransition;
    Check(SeedFile(readOnlyOutput, seed) && readOnlyTransition.Load(readOnlyOutput, error),
        L"read-only transition fixture load");
    Check(readOnlyTransition.SetByte(0, 0x91), L"read-only transition local edit");
    const DWORD originalReadOnlyAttributes = GetFileAttributesW(readOnlyOutput.c_str());
    Check(originalReadOnlyAttributes != INVALID_FILE_ATTRIBUTES &&
        SetFileAttributesW(readOnlyOutput.c_str(),
            originalReadOnlyAttributes | FILE_ATTRIBUTE_READONLY) != FALSE,
        L"set read-only after load");
    DWORD readOnlyError{};
    Check(!readOnlyTransition.Save(readOnlyError) && readOnlyError == ERROR_FILE_READ_ONLY &&
        readOnlyTransition.Dirty(), L"save-time read-only attribute rejects dirty save");
    ByteDocument readOnlyUnchanged;
    Check(readOnlyUnchanged.Load(readOnlyOutput, error) &&
        readOnlyUnchanged.ByteAt(0) == seed[0], L"rejected read-only save preserves disk bytes");
    Check(SetFileAttributesW(readOnlyOutput.c_str(),
        originalReadOnlyAttributes & ~FILE_ATTRIBUTE_READONLY) != FALSE,
        L"clear read-only after rejected save");
    Check(readOnlyTransition.Save(readOnlyError),
        L"same dirty document saves after current attribute becomes writable");

    // Loading while read-only must not permanently mark the document read-only.
    // The current attribute is cleared after Load and the subsequent save succeeds.
    Check(SetFileAttributesW(readOnlyOutput.c_str(),
        originalReadOnlyAttributes | FILE_ATTRIBUTE_READONLY) != FALSE,
        L"set read-only before load");
    ByteDocument loadedReadOnly;
    Check(loadedReadOnly.Load(readOnlyOutput, error), L"load read-only file");
    Check(loadedReadOnly.SetByte(1, 0x92), L"edit document loaded read-only");
    Check(SetFileAttributesW(readOnlyOutput.c_str(),
        originalReadOnlyAttributes & ~FILE_ATTRIBUTE_READONLY) != FALSE,
        L"clear read-only before save");
    Check(loadedReadOnly.Save(readOnlyError),
        L"file loaded read-only saves after current attribute is cleared");

    // Atomic Save As has the same rule when its selected destination already
    // exists, including paths used later by the elevated helper.
    ByteDocument saveAsReadOnly;
    saveAsReadOnly.NewEmpty();
    Check(saveAsReadOnly.AppendByte(0xA5), L"read-only Save As source edit");
    Check(SeedFile(readOnlySaveAs, seed), L"read-only Save As fixture creation");
    const DWORD originalSaveAsAttributes = GetFileAttributesW(readOnlySaveAs.c_str());
    Check(originalSaveAsAttributes != INVALID_FILE_ATTRIBUTES &&
        SetFileAttributesW(readOnlySaveAs.c_str(),
            originalSaveAsAttributes | FILE_ATTRIBUTE_READONLY) != FALSE,
        L"set Save As destination read-only");
    Check(!saveAsReadOnly.SaveAs(readOnlySaveAs, readOnlyError) &&
        readOnlyError == ERROR_FILE_READ_ONLY,
        L"Save As rejects current read-only destination");
    ByteDocument saveAsUnchanged;
    Check(saveAsUnchanged.Load(readOnlySaveAs, error) &&
        saveAsUnchanged.Size() == seed.size() && saveAsUnchanged.ByteAt(0) == seed[0],
        L"rejected read-only Save As preserves destination");
    SetFileAttributesW(readOnlySaveAs.c_str(),
        originalSaveAsAttributes & ~FILE_ATTRIBUTE_READONLY);

    ByteDocument removed;
    Check(SeedFile(missingOutput, seed) && removed.Load(missingOutput, error),
        L"external-removal fixture load");
    Check(DeleteFileW(missingOutput.c_str()) != FALSE, L"external file removal");
    Check(removed.CheckExternalChange(error, true) == ExternalFileState::Missing,
        L"external file removal detected");
    Check(removed.ContinueAfterExternalChange(error) && removed.Dirty(),
        L"removed file retained as unsaved content");
    Check(removed.Save(error) && FileIdentity(missingOutput) != 0,
        L"continued removed file recreated atomically");

    // A 32 MiB fixture makes accidental complete rewriting visible through the
    // process I/O counters without relying on elapsed-time thresholds. The one
    // byte overwrite should stay far below a one-MiB transfer allowance.
    constexpr std::size_t largeSize = 32u * 1024u * 1024u;
    constexpr std::size_t largeEditOffset = largeSize / 2u + 17u;
    ByteDocument large;
    Check(SeedLargeZeroFile(largeOutput, largeSize) && large.Load(largeOutput, error),
        L"large partial-save fixture load");
    Check(large.IsFileMapped() && ByteDocumentTestAccess::RetainedVectorBytes(large) < 1024u * 1024u,
        L"large load uses file mapping without private byte vector");
    Check(large.SetByte(largeEditOffset, 0xE7), L"large partial-save byte edit");
    Check(large.IsFileMapped() && ByteDocumentTestAccess::RetainedVectorBytes(large) < 1024u * 1024u,
        L"single-byte edit retains mapped baseline plus bounded private pieces");
    IO_COUNTERS beforeIo{};
    IO_COUNTERS afterIo{};
    Check(GetProcessIoCounters(GetCurrentProcess(), &beforeIo) != FALSE,
        L"large partial-save initial I/O counters");
    Check(large.Save(error), L"large partial-save commit");
    Check(GetProcessIoCounters(GetCurrentProcess(), &afterIo) != FALSE,
        L"large partial-save final I/O counters");
    Check(afterIo.WriteTransferCount >= beforeIo.WriteTransferCount &&
        afterIo.WriteTransferCount - beforeIo.WriteTransferCount < 1024u * 1024u,
        L"large partial save avoids complete stream write");
    Check(large.IsFileMapped() && ByteDocumentTestAccess::RetainedVectorBytes(large) < 1024u * 1024u,
        L"successful save remaps and releases writable byte vector");
    ByteDocument largeReloaded;
    Check(largeReloaded.Load(largeOutput, error) &&
        largeReloaded.ByteAt(largeEditOffset) == 0xE7,
        L"large partial-save byte persisted");

    // Empty documents expose a virtual EOF caret in the UI. Verify the model
    // operation used by that input path, including reversible growth.
    ByteDocument appended;
    appended.NewEmpty();
    Check(appended.Empty(), L"new document is truly empty");
    Check(appended.AppendByte(0xAB) && appended.Size() == 1 && appended.ByteAt(0) == 0xAB,
        L"append first byte");
    Check(appended.Undo(changed) && appended.Empty() && changed == 0, L"undo append");
    Check(appended.Redo(changed) && appended.Size() == 1 && appended.ByteAt(0) == 0xAB,
        L"redo append");
    Check(appended.AppendByte(0xCD) && appended.EraseRange(0, 2) && appended.Empty(),
        L"erase byte range");
    Check(appended.Undo(changed) && appended.Size() == 2 && appended.ByteAt(0) == 0xAB &&
        appended.ByteAt(1) == 0xCD, L"undo range erase as one operation");

    // Bulk replacement is the history primitive used by the bit-operation tool.
    // The entire transformed range must undo and redo as one user action.
    const std::array<std::uint8_t, 2> replacement{0x54, 0x32};
    Check(appended.ReplaceRange(0, replacement) && appended.ByteAt(0) == 0x54 &&
        appended.ByteAt(1) == 0x32, L"bulk range replacement");
    std::size_t changedCount{};
    Check(appended.Undo(changed, &changedCount) && changed == 0 && changedCount == 2 &&
        appended.ByteAt(0) == 0xAB && appended.ByteAt(1) == 0xCD,
        L"undo bulk replacement as one operation");
    Check(appended.Redo(changed, &changedCount) && changedCount == 2 &&
        appended.ByteAt(0) == 0x54 && appended.ByteAt(1) == 0x32,
        L"redo bulk replacement as one operation");

    // InsertRange is the model primitive behind the default UI insertion mode.
    // A multi-byte insertion must shift the tail and replay as one history item.
    const std::array<std::uint8_t, 2> insertedMiddle{0x10, 0x20};
    Check(appended.InsertRange(1, insertedMiddle) && appended.Size() == 4 &&
        appended.ByteAt(0) == 0x54 && appended.ByteAt(1) == 0x10 &&
        appended.ByteAt(2) == 0x20 && appended.ByteAt(3) == 0x32,
        L"insert byte range in middle");
    Check(appended.Undo(changed, &changedCount) && changed == 1 && changedCount == 2 &&
        appended.Size() == 2 && appended.ByteAt(1) == 0x32, L"undo inserted byte range");
    Check(appended.Redo(changed, &changedCount) && appended.Size() == 4 &&
        appended.ByteAt(1) == 0x10 && appended.ByteAt(2) == 0x20,
        L"redo inserted byte range");

    // Verify representative and boundary behavior for every operation family.
    std::uint8_t transformed{};
    Check(ApplyBitOperation(0xA5, BitOperation::And, 0x0F, transformed) && transformed == 0x05, L"bit AND");
    Check(ApplyBitOperation(0xA0, BitOperation::Or, 0x0F, transformed) && transformed == 0xAF, L"bit OR");
    Check(ApplyBitOperation(0xAA, BitOperation::Xor, 0xFF, transformed) && transformed == 0x55, L"bit XOR");
    Check(ApplyBitOperation(0x55, BitOperation::Not, 0, transformed) && transformed == 0xAA, L"bit NOT");
    Check(ApplyBitOperation(0x81, BitOperation::ShiftLeft, 1, transformed) && transformed == 0x02, L"left shift wraps width");
    Check(ApplyBitOperation(0x81, BitOperation::ShiftRight, 1, transformed) && transformed == 0x40, L"logical right shift");
    Check(ApplyBitOperation(0x81, BitOperation::RotateLeft, 1, transformed) && transformed == 0x03, L"rotate left");
    Check(ApplyBitOperation(0x81, BitOperation::RotateRight, 1, transformed) && transformed == 0xC0, L"rotate right");
    Check(!ApplyBitOperation(0x01, BitOperation::ShiftLeft, 8, transformed), L"invalid shift rejected");
    Check(ApplyBitOperation(0xFE, BitOperation::Add, 3, transformed) && transformed == 0x01, L"addition modulo 256");
    Check(ApplyBitOperation(0x01, BitOperation::Subtract, 2, transformed) && transformed == 0xFF, L"subtraction modulo 256");
    Check(ApplyBitOperation(0x80, BitOperation::Multiply, 2, transformed) && transformed == 0x00, L"multiplication modulo 256");
    Check(ApplyBitOperation(0x81, BitOperation::Divide, 3, transformed) && transformed == 0x2B, L"integer division");
    Check(!ApplyBitOperation(0x81, BitOperation::Divide, 0, transformed), L"division by zero rejected");

    // The custom picker relies on stable conversion at primary/secondary colors,
    // grayscale, and an arbitrary persisted profile value.
    Check(HsvToRgb({0.0, 1.0, 1.0}) == 0xFF0000, L"HSV red conversion");
    Check(HsvToRgb({120.0, 1.0, 1.0}) == 0x00FF00, L"HSV green conversion");
    Check(HsvToRgb({240.0, 1.0, 1.0}) == 0x0000FF, L"HSV blue conversion");
    Check(HsvToRgb({0.0, 0.0, 0.5}) == 0x808080, L"HSV grayscale rounding");
    const std::uint32_t roundTripColor = 0x5B8DEF;
    Check(HsvToRgb(RgbToHsv(roundTripColor)) == roundTripColor, L"RGB HSV round trip");

    // Reproduce a sustained editing session with deterministic random positions.
    // The default UI mode maps both Backspace and Delete to EraseRange, while
    // hexadecimal input inserts at the caret. The two removal phases subtract 512
    // bytes and the random insertion phase restores exactly 20 KiB. A final set of
    // replacements exercises the alternate overwrite mode without changing size.
    constexpr std::size_t targetSize = 20u * 1024u;
    constexpr std::size_t backspaceCount = 256;
    constexpr std::size_t deleteCount = 256;
    constexpr std::size_t insertCount = backspaceCount + deleteCount;
    constexpr std::size_t overwriteCount = 256;
    std::mt19937 random(0xB1E2026u);
    auto nextByte = [&random]() {
        return static_cast<std::uint8_t>(std::uniform_int_distribution<unsigned>(0, 255)(random));
    };

    ByteDocument stress;
    stress.NewEmpty();
    std::vector<std::uint8_t> expected;
    expected.reserve(targetSize);
    for (std::size_t index = 0; index < targetSize; ++index) {
        const std::uint8_t value = nextByte();
        stress.AppendByte(value);
        expected.push_back(value);
    }
    Check(stress.Size() == targetSize, L"stress bulk input size");

    for (std::size_t operation = 0; operation < backspaceCount; ++operation) {
        const std::size_t offset = std::uniform_int_distribution<std::size_t>(0, expected.size() - 1)(random);
        Check(stress.EraseRange(offset, 1), L"stress random backspace");
        expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    for (std::size_t operation = 0; operation < deleteCount; ++operation) {
        const std::size_t offset = std::uniform_int_distribution<std::size_t>(0, expected.size() - 1)(random);
        Check(stress.EraseRange(offset, 1), L"stress random forward delete");
        expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    for (std::size_t operation = 0; operation < insertCount; ++operation) {
        const std::size_t offset = std::uniform_int_distribution<std::size_t>(0, expected.size())(random);
        const std::uint8_t value = nextByte();
        const std::array<std::uint8_t, 1> insertion{value};
        Check(stress.InsertRange(offset, insertion), L"stress random insertion");
        expected.insert(expected.begin() + static_cast<std::ptrdiff_t>(offset), value);
    }
    for (std::size_t operation = 0; operation < overwriteCount; ++operation) {
        const std::size_t offset = std::uniform_int_distribution<std::size_t>(0, expected.size() - 1)(random);
        const std::uint8_t value = nextByte();
        stress.SetByte(offset, value);
        expected[offset] = value;
    }

    Check(stress.Size() == targetSize, L"stress final size is exactly 20 KiB");
    Check(DocumentBytes(stress) == expected,
        L"stress in-memory byte-for-byte result");
    Check(stress.SaveAs(stressOutput, error), L"stress atomic save");
    ByteDocument stressReloaded;
    Check(stressReloaded.Load(stressOutput, error), L"stress saved file reload");
    Check(stressReloaded.Size() == targetSize, L"stress persisted size is exactly 20 KiB");
    Check(stressReloaded.Size() == expected.size() &&
        DocumentBytes(stressReloaded) == expected,
        L"stress persisted byte-for-byte result");

    // Closing the final main-window file calls NewEmpty on the hot document
    // object. Exercise a multi-megabyte byte buffer plus range history and prove
    // that the replacement Untitled session retains no vector allocation from
    // the closed file.
    ByteDocument memoryRelease;
    std::vector<std::uint8_t> largeSession(8u * 1024u * 1024u, 0x00);
    memoryRelease.RestoreSnapshot(std::move(largeSession), {});
    std::vector<std::uint8_t> memoryReplacement(2u * 1024u * 1024u, 0xA5);
    Check(memoryRelease.ReplaceRange(0, memoryReplacement),
        L"memory-release fixture creates document history");
    Check(ByteDocumentTestAccess::RetainedVectorBytes(memoryRelease) >= 12u * 1024u * 1024u,
        L"memory-release fixture owns substantial byte and history storage");
    memoryRelease.NewEmpty();
    Check(memoryRelease.Empty() && !memoryRelease.CanUndo() && !memoryRelease.CanRedo() &&
        ByteDocumentTestAccess::RetainedVectorBytes(memoryRelease) == 0,
        L"NewEmpty releases closed document byte and history capacities");

    // Every file-mapped test document must release its view before deleting the
    // fixture. Windows intentionally rejects deletion while a user-mapped data
    // section still references that file object.
    document.NewEmpty();
    reloaded.NewEmpty();
    sparseReloaded.NewEmpty();
    atomicFallback.NewEmpty();
    fragmented.NewEmpty();
    fragmentedReloaded.NewEmpty();
    conflict.NewEmpty();
    readOnlyTransition.NewEmpty();
    readOnlyUnchanged.NewEmpty();
    loadedReadOnly.NewEmpty();
    saveAsReadOnly.NewEmpty();
    saveAsUnchanged.NewEmpty();
    removed.NewEmpty();
    large.NewEmpty();
    largeReloaded.NewEmpty();
    stress.NewEmpty();
    stressReloaded.NewEmpty();

    DeleteFileW(input.c_str());
    DeleteFileW(output.c_str());
    DeleteFileW(stressOutput.c_str());
    DeleteFileW(missingOutput.c_str());
    DeleteFileW(largeOutput.c_str());
    SetFileAttributesW(readOnlyOutput.c_str(), FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesW(readOnlySaveAs.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(readOnlyOutput.c_str());
    DeleteFileW(readOnlySaveAs.c_str());
    DeleteFileW(fragmentedOutput.c_str());
    if (failures == 0) std::wcout << L"All BinEdit core tests passed.\n";
    return failures == 0 ? 0 : 1;
}
