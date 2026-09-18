#pragma once

// Core data model and search API for BinEdit.
// The document exposes insertion, overwrite, deletion, and EOF append primitives.
// The UI selects the appropriate primitive for its current Insert-key mode, and
// every successful mutation is reversible.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

// Encodings supported by string search and the character pane. Binary storage
// is never transcoded; this enum only controls interpretation of existing bytes.
enum class TextEncoding { Ascii, Utf8, Utf16Le, ShiftJis };

// One byte in a compiled search expression. Wildcards match any byte and ignore
// value, allowing patterns such as "4D 5A ?? 00".
struct PatternByte {
    std::uint8_t value{};
    bool wildcard{};
};

struct SearchPattern {
    // Compiled representation consumed by the search loop.
    std::vector<PatternByte> bytes;
    // Original user input retained for future UI/history features.
    std::wstring displayText;
};

// Combined output from one parallel pass. Highlight offsets remain globally
// sorted and capped, while next independently reports the first match at/after
// the requested start or the first wrapped match before it.
struct ParallelSearchResult {
    std::vector<std::size_t> offsets;
    std::optional<std::size_t> next;
    unsigned workerCount{};
    bool canceled{};
    bool failed{};
};

// Immutable logical byte sequence shared by search and recovery workers. Its
// internal piece table retains mapped-file and edited blocks without flattening
// the complete document into private RAM.
class ByteSequenceSnapshot {
public:
    // State is defined only in Document.cpp. The public forward declaration lets
    // ByteDocument own the same representation without exposing its layout.
    struct State;
    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] bool Empty() const noexcept { return Size() == 0; }
    [[nodiscard]] std::uint8_t ByteAt(std::size_t offset) const noexcept;
    // Copies exactly output.size() bytes. False indicates invalid bounds or an
    // operating-system in-page error while reading a mapped file.
    [[nodiscard]] bool CopyRange(std::size_t offset,
        std::span<std::uint8_t> output) const noexcept;

private:
    explicit ByteSequenceSnapshot(std::shared_ptr<const State> state) noexcept :
        state_(std::move(state)) {}
    std::shared_ptr<const State> state_;
    friend class ByteDocument;
};

using ImmutableByteSnapshot = std::shared_ptr<const ByteSequenceSnapshot>;

// Stable, language-neutral parser errors. The application maps these values to
// the active Japanese or English string table at the presentation boundary.
enum class SearchPatternError {
    None,
    Empty,
    InvalidHex,
    MissingSeparator,
    Unrepresentable,
    EncodingFailed,
    TooLarge,
    AllocationFailed
};

// Result of comparing a loaded document with its current on-disk identity and
// metadata. Missing is distinct from Changed because there is nothing to reload;
// Unavailable represents a transient query failure such as a sharing violation.
enum class ExternalFileState { Unchanged, Changed, Missing, Unavailable };

#ifdef BINEDIT_CORE_TESTS
struct ByteDocumentTestAccess;
#endif

class ByteDocument {
public:
    // A minimal reversible splice. before is removed and after is inserted at
    // offset; replacement, append, and range deletion share the same replay path.
    struct Edit {
        std::size_t offset{};
        std::vector<std::uint8_t> before;
        std::vector<std::uint8_t> after;
    };

    ByteDocument();
    ~ByteDocument();
    ByteDocument(const ByteDocument&) = delete;
    ByteDocument& operator=(const ByteDocument&) = delete;
    ByteDocument(ByteDocument&& other) noexcept;
    ByteDocument& operator=(ByteDocument&& other) noexcept;

    // Replaces the document with a read-only Windows file mapping. The mapping
    // reserves address space but does not copy the complete file into private RAM;
    // Windows faults only the pages actually viewed or searched. On failure, the
    // existing editor data remains intact and error receives GetLastError.
    bool Load(const std::filesystem::path& path, DWORD& error);
    // Saves ordinary overwrite edits in place by writing only dirty byte ranges.
    // A mapped size-changing edit streams the logical piece table to a sibling
    // and publishes it atomically because Windows forbids shrinking the mapped
    // stream. Recovery, acknowledged external conflicts, and Save As also use
    // full atomic publication. Every attempt re-queries FILE_ATTRIBUTE_READONLY
    // and reports ERROR_FILE_READ_ONLY without relying on the Load-time state.
    // Sparse writes are not atomic by themselves; the application publishes a
    // current durable recovery checkpoint before calling this path.
    bool Save(DWORD& error);
    // Uses the same identity/conflict checks as Save but always publishes a full
    // sibling snapshot atomically. The application uses this fallback whenever a
    // durable pre-save recovery checkpoint cannot be guaranteed.
    bool SaveAtomically(DWORD& error);
    // Persists atomically and adopts path as the document's new identity.
    bool SaveAs(const std::filesystem::path& path, DWORD& error);
    // Writes an exact snapshot without changing path, dirty state, or history.
    // Elevated restart uses this method to stage otherwise-unsavable edits.
    bool WriteCopy(const std::filesystem::path& path, DWORD& error) const;

    // Creates a genuinely empty untitled document. The UI exposes its offset-zero
    // virtual EOF caret; the first input appends the first real byte.
    void NewEmpty();
    // Changes identity without I/O; reserved for recovery/elevation workflows.
    void AdoptPath(const std::filesystem::path& path, bool dirty);
    // Restores a validated crash snapshot as dirty data without manufacturing an
    // Undo history entry. The original path remains a save target, never a load.
    void RestoreSnapshot(std::vector<std::uint8_t>&& bytes, const std::filesystem::path& originalPath);
    // Overwrites one byte and records a history entry. Returns false for a no-op
    // or an out-of-range offset.
    bool SetByte(std::size_t offset, std::uint8_t value);
    // Appends one byte and records an insertion history entry.
    bool AppendByte(std::uint8_t value);
    // Inserts a complete byte sequence before offset and records one history
    // entry. offset may equal Size() so encoded character input can append using
    // the same atomic operation it uses in the middle of a document.
    bool InsertRange(std::size_t offset, std::span<const std::uint8_t> bytes);
    // Replaces an existing range without changing document length. The complete
    // range is recorded as one history entry so bulk tools undo atomically.
    bool ReplaceRange(std::size_t offset, std::span<const std::uint8_t> replacement);
    // Removes one contiguous byte range as a single reversible history entry.
    bool EraseRange(std::size_t offset, std::size_t count);
    // Replays one history entry and reports the invalidated range. changedCount
    // is optional for callers that need only the first affected offset.
    bool Undo(std::size_t& changedOffset, std::size_t* changedCount = nullptr);
    bool Redo(std::size_t& changedOffset, std::size_t* changedCount = nullptr);

    // Consumes a directory-change notification and compares the path with the
    // exact file identity captured by Load/Save. A metadata poll is used when the
    // notification API is unavailable, so monitoring degrades safely.
    [[nodiscard]] ExternalFileState CheckExternalChange(DWORD& error, bool forceQuery = false);
    // Records the current external version as the new conflict baseline while
    // retaining the in-memory bytes. The next save deliberately uses a complete
    // atomic replacement because sparse ranges were based on an older file.
    bool ContinueAfterExternalChange(DWORD& error);

    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] bool Empty() const noexcept { return Size() == 0; }
    [[nodiscard]] std::uint8_t ByteAt(std::size_t offset) const noexcept;
    [[nodiscard]] bool CopyRange(std::size_t offset,
        std::span<std::uint8_t> output) const noexcept;
    [[nodiscard]] ImmutableByteSnapshot CreateSnapshot() const;
    // Exposed for diagnostics and memory-regression automation. A document can
    // contain both mapped original pieces and small private edit blocks.
    [[nodiscard]] bool IsFileMapped() const noexcept;
    // Counts private piece metadata and owned edit/recovery byte capacity but not
    // file-backed mapped pages. Used by diagnostics and memory regression tests.
    [[nodiscard]] std::size_t PrivateStorageBytes() const;
    [[nodiscard]] bool Dirty() const noexcept { return dirty_; }
    // Used only after an explicit user decision to discard unsaved data.
    void MarkClean() noexcept;
    [[nodiscard]] bool CanUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool CanRedo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }
    // Advances only when bytes or recovery-relevant document identity changes.
    // Recovery workers use this value to prove their durable snapshot is current.
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
    [[nodiscard]] bool RequiresAtomicSave() const noexcept { return forceFullSave_; }

private:
#ifdef BINEDIT_CORE_TESTS
    // Core tests inspect retained container capacities after NewEmpty without
    // exposing diagnostic-only storage details in production builds.
    friend struct ByteDocumentTestAccess;
#endif
    struct DirtyRange {
        std::size_t begin{};
        std::size_t end{};
    };

    // A path stamp detects both in-place writes and atomic file replacement.
    // File identity prevents a replacement with coincidentally equal size and
    // timestamp from being mistaken for the version originally loaded.
    struct FileStamp {
        std::uint64_t volumeSerial{};
        std::uint64_t fileIndex{};
        std::uint64_t lastWrite{};
        std::uint64_t size{};
        bool known{};
        bool exists{};
    };

    // Implements crash-resistant persistence: write and flush a unique sibling,
    // then use ReplaceFile to preserve existing security metadata or MoveFileEx
    // only when publishing a genuinely new destination.
    bool WriteTo(const std::filesystem::path& path, DWORD& error) const;
    // Writes pending fixed-size sparse ranges through one locked handle. Mapped
    // structural edits are routed to atomic publication before this is called.
    bool WritePendingChanges(DWORD& error);
    bool SaveInternal(DWORD& error, bool forceAtomic);
    static bool QueryFileStamp(const std::filesystem::path& path, FileStamp& stamp, DWORD& error) noexcept;
    static bool QueryHandleStamp(HANDLE file, FileStamp& stamp, DWORD& error) noexcept;
    [[nodiscard]] static bool SameFileStamp(const FileStamp& left, const FileStamp& right) noexcept;
    // Creates one mapped baseline state without changing this document. The
    // caller installs it only after all identity checks have succeeded.
    static bool CreateReadOnlyStorage(const std::filesystem::path& path,
        std::shared_ptr<ByteSequenceSnapshot::State>& storage, FileStamp& stamp,
        DWORD& error) noexcept;
    // Replaces a logical interval with a small owned block while retaining all
    // unaffected mapped pieces. Construction is transactional and snapshot-safe.
    bool SpliceStorage(std::size_t offset, std::size_t removeCount,
        std::span<const std::uint8_t> insertion) noexcept;
    // Streams a logical byte range to an already-positioned file handle.
    bool WriteStorageRange(HANDLE file, std::size_t offset, std::size_t count,
        DWORD& error, bool& mutated) const noexcept;
    // Best-effort post-save remapping releases obsolete edit blocks.
    void RemapSavedFile() noexcept;
    void RefreshChangeNotification() noexcept;
    void CloseChangeNotification() noexcept;
    void ResetDirtyTracking() noexcept;
    void BumpRevision() noexcept;
    bool TrackOverwrite(std::size_t offset, std::size_t count) noexcept;
    void TrackStructuralChange(std::size_t offset) noexcept;
    // Logical storage is a persistent piece table. Snapshots copy only piece
    // metadata and share immutable mapped/owned blocks across worker lifetimes.
    std::shared_ptr<ByteSequenceSnapshot::State> storage_;
    std::filesystem::path path_;
    std::vector<Edit> undo_;
    std::vector<Edit> redo_;
    // Sorted, non-overlapping overwrite ranges minimize disk traffic without
    // sacrificing correctness for edits scattered across a very large file.
    std::vector<DirtyRange> dirtyRanges_;
    std::optional<std::size_t> rewriteFrom_;
    FileStamp diskStamp_;
    HANDLE changeNotification_{INVALID_HANDLE_VALUE};
    // Includes byte payloads held by both stacks. Entry count alone is not a
    // useful memory bound because one range operation may span a huge file.
    std::size_t historyBytes_{};
    std::uint64_t revision_{};
    bool dirty_{};
    bool forceFullSave_{};
    // Remains set until reload/continue resolves a detected change, even after
    // the one-shot directory notification has been rearmed.
    bool externalChangePending_{};
};

// Parses whitespace-separated hexadecimal bytes; "??" produces a wildcard.
std::optional<SearchPattern> ParseBinaryPattern(const std::wstring& text, SearchPatternError& error);
// Encodes Unicode input without best-fit substitution, so searches never silently
// target bytes different from the text entered by the user.
std::optional<SearchPattern> EncodeTextPattern(const std::wstring& text, TextEncoding encoding, SearchPatternError& error);
// Finds the first match at or after start. Inputs are borrowed and never
// copied. The pattern is split at wildcard bytes into literal fragments, which
// one Aho-Corasick pass per bounded window matches together.
std::optional<std::size_t> FindPattern(std::span<const std::uint8_t> bytes, const SearchPattern& pattern, std::size_t start);
// Collects overlapping highlights in ascending order, capped at limit to bound
// memory and per-frame lookup cost.
std::vector<std::size_t> FindAllPatterns(std::span<const std::uint8_t> bytes, const SearchPattern& pattern, std::size_t limit = 10000);
// Partitions candidate start offsets across worker threads. snapshot is owned
// and immutable for the complete call, including until all internal jthreads
// have joined. stopToken is checked both between candidates and inside long
// patterns. workerHint zero selects a bounded hardware-aware count; tests may
// request an exact upper bound.
ParallelSearchResult FindPatternsParallel(ImmutableByteSnapshot snapshot, const SearchPattern& pattern,
    std::size_t start, std::size_t limit = 10000, std::stop_token stopToken = {}, unsigned workerHint = 0);
// Returns the locale-independent display name used by both language variants.
std::wstring EncodingName(TextEncoding encoding);
