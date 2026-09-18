// Implements the mapped/piece-table byte document, sparse and atomic file
// persistence, edit history, binary-pattern parsing, text encoding, and search.
// This module is UI-independent apart from returning native Win32 error codes.

#include "Document.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <unordered_set>
#include <utility>

// A block is immutable after publication. Mapped blocks keep only a file view;
// owned blocks hold inserted/replaced/recovered bytes. Pieces add logical offsets
// so structural edits can reuse both kinds without moving the untouched suffix.
struct ByteSequenceSnapshot::State {
    struct Block {
        std::vector<std::uint8_t> owned;
        const std::uint8_t* mapped{};
        std::size_t mappedSize{};
        ~Block() { if (mapped) UnmapViewOfFile(mapped); }
        [[nodiscard]] std::size_t Size() const noexcept {
            return mapped ? mappedSize : owned.size();
        }
        [[nodiscard]] const std::uint8_t* Data() const noexcept {
            return mapped ? mapped : owned.data();
        }
        [[nodiscard]] bool IsMapped() const noexcept { return mapped != nullptr; }
    };
    struct Piece {
        std::shared_ptr<Block> block;
        std::size_t blockOffset{};
        std::size_t length{};
        std::size_t documentOffset{};
    };
    std::vector<Piece> pieces;
    std::size_t size{};
};

namespace {
// Bounds memory usage while retaining a long practical editing history.
constexpr std::size_t kMaxHistory = 100000;
constexpr std::size_t kMaxHistoryBytes = 256u * 1024u * 1024u;
// The dialog enforces this same limit, while the core check also protects
// callers such as tests, future automation, and IPC-facing extensions.
constexpr std::size_t kMaxSearchCharacters = 64u * 1024u;
// A pathological stream of isolated byte writes switches to full atomic
// publication instead of widening sparse I/O across an enormous file span.
constexpr std::size_t kMaxDirtyRanges = 4096;

bool SafeCopyMappedBytes(const std::uint8_t* source, std::uint8_t* destination,
    std::size_t count) noexcept {
#if defined(_MSC_VER)
    __try {
        std::memcpy(destination, source, count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(destination, source, count);
    return true;
#endif
}

const ByteSequenceSnapshot::State::Piece* FindStoragePiece(
    const ByteSequenceSnapshot::State& state, std::size_t offset) noexcept {
    const auto found = std::upper_bound(state.pieces.begin(), state.pieces.end(), offset,
        [](std::size_t value, const ByteSequenceSnapshot::State::Piece& piece) {
            return value < piece.documentOffset;
        });
    if (found == state.pieces.begin()) return nullptr;
    const auto& piece = *std::prev(found);
    return offset - piece.documentOffset < piece.length ? &piece : nullptr;
}

bool CopyStorageRange(const ByteSequenceSnapshot::State& state, std::size_t offset,
    std::span<std::uint8_t> output) noexcept {
    if (offset > state.size || output.size() > state.size - offset) return false;
    std::size_t destinationOffset{};
    std::size_t logicalOffset = offset;
    while (destinationOffset < output.size()) {
        const auto* piece = FindStoragePiece(state, logicalOffset);
        if (!piece) return false;
        const std::size_t inPiece = logicalOffset - piece->documentOffset;
        const std::size_t chunk = std::min(output.size() - destinationOffset,
            piece->length - inPiece);
        const auto* source = piece->block->Data() + piece->blockOffset + inPiece;
        if (!SafeCopyMappedBytes(source, output.data() + destinationOffset, chunk)) return false;
        logicalOffset += chunk;
        destinationOffset += chunk;
    }
    return true;
}

void AppendStoragePiece(std::vector<ByteSequenceSnapshot::State::Piece>& pieces,
    const std::shared_ptr<ByteSequenceSnapshot::State::Block>& block,
    std::size_t blockOffset, std::size_t length, std::size_t& documentOffset) {
    if (length == 0) return;
    if (!pieces.empty()) {
        auto& previous = pieces.back();
        if (previous.block == block && previous.blockOffset + previous.length == blockOffset) {
            previous.length += length;
            documentOffset += length;
            return;
        }
    }
    pieces.push_back({block, blockOffset, length, documentOffset});
    documentOffset += length;
}

void AppendStorageRange(const ByteSequenceSnapshot::State& source,
    std::size_t begin, std::size_t end,
    std::vector<ByteSequenceSnapshot::State::Piece>& destination,
    std::size_t& documentOffset) {
    if (begin >= end) return;
    for (const auto& piece : source.pieces) {
        const std::size_t pieceEnd = piece.documentOffset + piece.length;
        if (pieceEnd <= begin) continue;
        if (piece.documentOffset >= end) break;
        const std::size_t intersectionBegin = std::max(begin, piece.documentOffset);
        const std::size_t intersectionEnd = std::min(end, pieceEnd);
        AppendStoragePiece(destination, piece.block,
            piece.blockOffset + intersectionBegin - piece.documentOffset,
            intersectionEnd - intersectionBegin, documentOffset);
    }
}

// File attributes are deliberately queried for every persistence attempt. A
// read-only bit observed during Load is not document state: another process may
// clear or set it while the in-memory bytes remain open in BinEdit.
bool CheckCurrentTargetWritable(const std::filesystem::path& path, DWORD& error) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD attributeError = GetLastError();
        // An absent Save As target is writable in principle; its parent-directory
        // access is authoritatively checked when the sibling temporary is created.
        if (attributeError == ERROR_FILE_NOT_FOUND || attributeError == ERROR_PATH_NOT_FOUND) {
            error = ERROR_SUCCESS;
            return true;
        }
        error = attributeError;
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        error = ERROR_FILE_READ_ONLY;
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

// Attribute changes can race the initial check. Prefer the explicit read-only
// result over ERROR_ACCESS_DENIED so the UI never mistakes this policy rejection
// for an ACL failure that could justify an elevated restart.
void PreferCurrentReadOnlyError(const std::filesystem::path& path, DWORD& error) noexcept {
    if (error != ERROR_ACCESS_DENIED) return;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_READONLY) != 0) error = ERROR_FILE_READ_ONLY;
}

std::size_t EditBytes(const ByteDocument::Edit& edit) noexcept {
    return edit.before.size() + edit.after.size();
}

void ReserveHistorySlot(std::vector<ByteDocument::Edit>& history) {
    if (history.size() >= kMaxHistory || history.size() < history.capacity()) return;
    const std::size_t growth = std::max<std::size_t>(8, history.capacity() / 2);
    history.reserve(std::min(kMaxHistory, history.size() + growth));
}

std::wstring RandomHexName() {
    std::array<std::uint8_t, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result(random.size() * 2, L'0');
    for (std::size_t index = 0; index < random.size(); ++index) {
        result[index * 2] = digits[random[index] >> 4];
        result[index * 2 + 1] = digits[random[index] & 0x0fu];
    }
    return result;
}

// Strict hexadecimal conversion used by the binary-pattern grammar.
int HexDigit(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}

bool MatchesAt(std::span<const std::uint8_t> data, const SearchPattern& pattern, std::size_t at,
    std::stop_token stopToken = {}, bool* canceled = nullptr) {
    if (canceled) *canceled = false;
    // Subtraction after the explicit at check avoids size_t addition overflow.
    if (at > data.size() || pattern.bytes.size() > data.size() - at) return false;
    for (std::size_t index = 0; index < pattern.bytes.size(); ++index) {
        // Verification of a long pattern is itself a long loop, so cancellation
        // is polled here as well as between candidates.
        if ((index & 0xfffu) == 0 && stopToken.stop_requested()) {
            if (canceled) *canceled = true;
            return false;
        }
        if (!pattern.bytes[index].wildcard && data[at + index] != pattern.bytes[index].value) return false;
    }
    return true;
}

// Aho-Corasick automaton built from the literal runs produced by splitting a
// search pattern at its wildcard bytes. One linear pass over a candidate window
// then discovers every fragment occurrence instead of rescanning the complete
// pattern at every offset. Transitions are sparse linked lists because the byte
// alphabet is 256 while a pattern introduces only a small number of trie edges.
class WildcardAutomaton {
public:
    static constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

    // One maximal run of non-wildcard bytes. offset is the run position inside
    // the complete pattern, so a fragment end maps directly to a candidate
    // pattern start.
    struct Segment {
        std::size_t offset{};
        std::size_t length{};
    };

    explicit WildcardAutomaton(const SearchPattern& pattern) {
        std::size_t index = 0;
        while (index < pattern.bytes.size()) {
            if (pattern.bytes[index].wildcard) { ++index; continue; }
            const std::size_t begin = index;
            while (index < pattern.bytes.size() && !pattern.bytes[index].wildcard) ++index;
            segments_.push_back({begin, index - begin});
        }
        nodes_.emplace_back();
        outputHead_.push_back(kNone);
        outputTail_.push_back(kNone);
        for (std::uint32_t segment = 0; segment < segments_.size(); ++segment) {
            const Segment& run = segments_[segment];
            std::uint32_t node = 0;
            for (std::size_t at = 0; at < run.length; ++at) {
                const std::uint8_t byte = pattern.bytes[run.offset + at].value;
                std::uint32_t child = FindChild(node, byte);
                if (child == kNone) {
                    child = static_cast<std::uint32_t>(nodes_.size());
                    nodes_.push_back({});
                    outputHead_.push_back(kNone);
                    outputTail_.push_back(kNone);
                    const std::uint32_t edge = static_cast<std::uint32_t>(edges_.size());
                    edges_.push_back({byte, child, nodes_[node].firstEdge});
                    nodes_[node].firstEdge = edge;
                }
                node = child;
            }
            const std::uint32_t output = static_cast<std::uint32_t>(outputSegment_.size());
            outputSegment_.push_back(segment);
            outputNext_.push_back(kNone);
            if (outputHead_[node] == kNone) outputHead_[node] = output;
            else outputNext_[outputTail_[node]] = output;
            outputTail_[node] = output;
        }
        BuildFailureLinks();
    }

    [[nodiscard]] bool Empty() const noexcept { return segments_.empty(); }
    [[nodiscard]] const Segment& GetSegment(std::uint32_t index) const noexcept { return segments_[index]; }
    [[nodiscard]] std::uint32_t Root() const noexcept { return 0; }

    // Consumes one byte and returns the deepest trie node matching a suffix of
    // the text read so far, following failure links as needed.
    [[nodiscard]] std::uint32_t Step(std::uint32_t state, std::uint8_t byte) const noexcept {
        while (state != 0 && FindChild(state, byte) == kNone) state = nodes_[state].failure;
        const std::uint32_t child = FindChild(state, byte);
        return child == kNone ? 0 : child;
    }

    // Visits every segment ending at state. Lists already include segments
    // inherited through failure links, so one visit is exhaustive.
    template <typename Report>
    void ForEachOutput(std::uint32_t state, Report&& report) const {
        for (std::uint32_t output = outputHead_[state]; output != kNone;
            output = outputNext_[output]) {
            report(outputSegment_[output]);
        }
    }

private:
    struct Node {
        std::uint32_t failure{};
        std::uint32_t firstEdge{kNone};
    };
    struct Edge {
        std::uint8_t byte{};
        std::uint32_t child{};
        std::uint32_t next{kNone};
    };

    [[nodiscard]] std::uint32_t FindChild(std::uint32_t node, std::uint8_t byte) const noexcept {
        for (std::uint32_t edge = nodes_[node].firstEdge; edge != kNone; edge = edges_[edge].next) {
            if (edges_[edge].byte == byte) return edges_[edge].child;
        }
        return kNone;
    }

    // Appends failure's already-transitive output list to node's own list.
    void MergeOutputs(std::uint32_t node, std::uint32_t failure) {
        if (outputHead_[failure] == kNone) return;
        if (outputHead_[node] == kNone) {
            outputHead_[node] = outputHead_[failure];
            outputTail_[node] = outputTail_[failure];
            return;
        }
        outputNext_[outputTail_[node]] = outputHead_[failure];
        outputTail_[node] = outputTail_[failure];
    }

    void BuildFailureLinks() {
        std::vector<std::uint32_t> breadthFirst;
        breadthFirst.reserve(nodes_.size());
        for (std::uint32_t edge = nodes_[0].firstEdge; edge != kNone; edge = edges_[edge].next) {
            breadthFirst.push_back(edges_[edge].child);
        }
        for (std::size_t head = 0; head < breadthFirst.size(); ++head) {
            const std::uint32_t node = breadthFirst[head];
            for (std::uint32_t edge = nodes_[node].firstEdge; edge != kNone; edge = edges_[edge].next) {
                const std::uint8_t byte = edges_[edge].byte;
                const std::uint32_t child = edges_[edge].child;
                std::uint32_t failure = nodes_[node].failure;
                while (failure != 0 && FindChild(failure, byte) == kNone) {
                    failure = nodes_[failure].failure;
                }
                const std::uint32_t target = FindChild(failure, byte);
                nodes_[child].failure = target == kNone ? 0 : target;
                // The failure node is shallower and was processed first, so its
                // output list is already transitively complete.
                MergeOutputs(child, nodes_[child].failure);
                breadthFirst.push_back(child);
            }
        }
    }

    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<Segment> segments_;
    std::vector<std::uint32_t> outputHead_;
    std::vector<std::uint32_t> outputTail_;
    std::vector<std::uint32_t> outputSegment_;
    std::vector<std::uint32_t> outputNext_;
};

// Retains the smallest offsets seen in any arrival order. Automaton outputs are
// ordered by fragment end, so the candidate starts belonging to one match can
// arrive out of ascending order; a bounded max-heap restores display order
// without ever holding more than limit entries per partition.
class SmallestOffsets {
public:
    explicit SmallestOffsets(std::size_t limit) : limit_(limit) {}

    void Add(std::size_t offset) {
        if (heap_.size() < limit_) {
            heap_.push_back(offset);
            std::push_heap(heap_.begin(), heap_.end());
        } else if (limit_ != 0 && offset < heap_.front()) {
            std::pop_heap(heap_.begin(), heap_.end());
            heap_.back() = offset;
            std::push_heap(heap_.begin(), heap_.end());
        }
    }

    [[nodiscard]] std::vector<std::size_t> TakeSorted() {
        std::sort_heap(heap_.begin(), heap_.end());
        return std::move(heap_);
    }

private:
    std::vector<std::size_t> heap_;
    std::size_t limit_{};
};

// Scans one bounded window through the automaton and reports every matching
// pattern start in [begin, end) exactly once. buffer starts at document offset
// bufferOffset and must cover every candidate byte in the range; verified holds
// ceil((end - begin) / 8) zeroed bytes and suppresses duplicate verification
// when several fragments of the same match hit the same candidate. Returns
// false when stopToken requested cancellation.
template <typename Report>
bool ScanCandidateWindow(std::span<const std::uint8_t> buffer, std::size_t bufferOffset,
    const SearchPattern& pattern, const WildcardAutomaton& automaton,
    std::size_t begin, std::size_t end, std::span<std::uint8_t> verified,
    std::stop_token stopToken, Report&& report) {
    std::uint32_t state = automaton.Root();
    for (std::size_t position = 0; position < buffer.size(); ++position) {
        if ((position & 0x3ffu) == 0 && stopToken.stop_requested()) return false;
        state = automaton.Step(state, buffer[position]);
        bool canceled = false;
        automaton.ForEachOutput(state, [&](std::uint32_t segmentIndex) {
            if (canceled) return;
            const WildcardAutomaton::Segment& segment = automaton.GetSegment(segmentIndex);
            if (position + 1 < segment.length) return;
            const std::size_t segmentStart = position + 1 - segment.length;
            if (segmentStart < segment.offset) return;
            const std::size_t candidate = bufferOffset + segmentStart - segment.offset;
            if (candidate < begin || candidate >= end) return;
            const std::size_t relative = candidate - begin;
            const std::uint8_t mask = static_cast<std::uint8_t>(1u << (relative & 7u));
            if ((verified[relative >> 3] & mask) != 0) return;
            verified[relative >> 3] |= mask;
            if (MatchesAt(buffer, pattern, candidate - bufferOffset, stopToken, &canceled)) {
                report(candidate);
            }
        });
        if (canceled) return false;
    }
    return true;
}
}

std::size_t ByteSequenceSnapshot::Size() const noexcept {
    return state_ ? state_->size : 0;
}

std::uint8_t ByteSequenceSnapshot::ByteAt(std::size_t offset) const noexcept {
    if (!state_ || offset >= state_->size) return 0;
    const auto* piece = FindStoragePiece(*state_, offset);
    if (!piece) return 0;
    std::uint8_t value{};
    const auto* source = piece->block->Data() + piece->blockOffset +
        offset - piece->documentOffset;
    return SafeCopyMappedBytes(source, &value, 1) ? value : 0;
}

bool ByteSequenceSnapshot::CopyRange(std::size_t offset,
    std::span<std::uint8_t> output) const noexcept {
    return state_ && CopyStorageRange(*state_, offset, output);
}

ByteDocument::ByteDocument() : storage_(std::make_shared<ByteSequenceSnapshot::State>()) {}

ByteDocument::~ByteDocument() {
    CloseChangeNotification();
}

ByteDocument::ByteDocument(ByteDocument&& other) noexcept { *this = std::move(other); }

ByteDocument& ByteDocument::operator=(ByteDocument&& other) noexcept {
    if (this == &other) return *this;
    CloseChangeNotification();
    storage_ = std::move(other.storage_);
    path_ = std::move(other.path_);
    undo_ = std::move(other.undo_);
    redo_ = std::move(other.redo_);
    dirtyRanges_ = std::move(other.dirtyRanges_);
    rewriteFrom_ = other.rewriteFrom_;
    diskStamp_ = other.diskStamp_;
    changeNotification_ = std::exchange(other.changeNotification_, INVALID_HANDLE_VALUE);
    historyBytes_ = other.historyBytes_;
    revision_ = other.revision_;
    dirty_ = other.dirty_;
    forceFullSave_ = other.forceFullSave_;
    externalChangePending_ = other.externalChangePending_;
    other.rewriteFrom_.reset();
    other.diskStamp_ = {};
    other.historyBytes_ = 0;
    other.revision_ = 0;
    other.dirty_ = false;
    other.forceFullSave_ = false;
    other.externalChangePending_ = false;
    return *this;
}

std::size_t ByteDocument::Size() const noexcept {
    return storage_ ? storage_->size : 0;
}

std::uint8_t ByteDocument::ByteAt(std::size_t offset) const noexcept {
    if (!storage_ || offset >= storage_->size) return 0;
    const auto* piece = FindStoragePiece(*storage_, offset);
    if (!piece) return 0;
    std::uint8_t value{};
    const auto* source = piece->block->Data() + piece->blockOffset +
        offset - piece->documentOffset;
    return SafeCopyMappedBytes(source, &value, 1) ? value : 0;
}

bool ByteDocument::CopyRange(std::size_t offset,
    std::span<std::uint8_t> output) const noexcept {
    return storage_ && CopyStorageRange(*storage_, offset, output);
}

ImmutableByteSnapshot ByteDocument::CreateSnapshot() const {
    return ImmutableByteSnapshot(new ByteSequenceSnapshot(storage_));
}

bool ByteDocument::IsFileMapped() const noexcept {
    if (!storage_) return false;
    return std::ranges::any_of(storage_->pieces, [](const auto& piece) {
        return piece.block && piece.block->IsMapped();
    });
}

std::size_t ByteDocument::PrivateStorageBytes() const {
    if (!storage_) return 0;
    std::size_t total = storage_->pieces.capacity() *
        sizeof(ByteSequenceSnapshot::State::Piece);
    std::unordered_set<const ByteSequenceSnapshot::State::Block*> visited;
    for (const auto& piece : storage_->pieces) {
        if (piece.block && visited.insert(piece.block.get()).second)
            total += piece.block->owned.capacity();
    }
    return total;
}

bool ByteDocument::QueryHandleStamp(HANDLE file, FileStamp& stamp, DWORD& error) noexcept {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) {
        error = GetLastError();
        return false;
    }
    stamp.volumeSerial = information.dwVolumeSerialNumber;
    stamp.fileIndex = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
                      information.nFileIndexLow;
    stamp.lastWrite = (static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32) |
                      information.ftLastWriteTime.dwLowDateTime;
    stamp.size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) |
                 information.nFileSizeLow;
    stamp.known = true;
    stamp.exists = true;
    error = ERROR_SUCCESS;
    return true;
}

bool ByteDocument::QueryFileStamp(const std::filesystem::path& path, FileStamp& stamp,
    DWORD& error) noexcept {
    stamp = {};
    if (path.empty()) {
        error = ERROR_INVALID_NAME;
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            stamp.known = true;
            stamp.exists = false;
            error = ERROR_SUCCESS;
            return true;
        }
        return false;
    }
    const bool queried = QueryHandleStamp(file, stamp, error);
    CloseHandle(file);
    return queried;
}

bool ByteDocument::SameFileStamp(const FileStamp& left, const FileStamp& right) noexcept {
    if (!left.known || !right.known || left.exists != right.exists) return false;
    if (!left.exists) return true;
    return left.volumeSerial == right.volumeSerial && left.fileIndex == right.fileIndex &&
           left.lastWrite == right.lastWrite && left.size == right.size;
}

bool ByteDocument::CreateReadOnlyStorage(const std::filesystem::path& path,
    std::shared_ptr<ByteSequenceSnapshot::State>& storage, FileStamp& stamp,
    DWORD& error) noexcept {
    storage.reset();
    stamp = {};
    error = ERROR_SUCCESS;
    // Sharing write/delete preserves external monitoring and sparse-save
    // behavior. The section remains bound to the opened file object if another
    // application later publishes a replacement under the same path.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    FileStamp initial{};
    if (!QueryHandleStamp(file, initial, error)) {
        CloseHandle(file);
        return false;
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 ||
        static_cast<unsigned long long>(length.QuadPart) >
            static_cast<unsigned long long>(PTRDIFF_MAX) ||
        static_cast<unsigned long long>(length.QuadPart) >
            std::numeric_limits<std::size_t>::max()) {
        error = GetLastError() == ERROR_SUCCESS ? ERROR_FILE_TOO_LARGE : GetLastError();
        CloseHandle(file);
        return false;
    }
    const std::size_t requested = static_cast<std::size_t>(length.QuadPart);
    if (requested == 0) {
        FileStamp completed{};
        const bool stable = QueryHandleStamp(file, completed, error) &&
            SameFileStamp(initial, completed);
        CloseHandle(file);
        if (!stable && error == ERROR_SUCCESS) error = ERROR_FILE_INVALID;
        if (!stable) return false;
        try { storage = std::make_shared<ByteSequenceSnapshot::State>(); }
        catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; return false; }
        stamp = completed;
        return true;
    }

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        error = GetLastError();
        CloseHandle(file);
        return false;
    }
    const void* mapped = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    const DWORD mappingError = mapped ? ERROR_SUCCESS : GetLastError();
    CloseHandle(mapping);
    if (!mapped) {
        error = mappingError;
        CloseHandle(file);
        return false;
    }

    // Reject a size/identity transition that raced section creation. The view is
    // not published until this second stamp proves one coherent baseline.
    FileStamp completed{};
    if (!QueryHandleStamp(file, completed, error) || !SameFileStamp(initial, completed)) {
        if (error == ERROR_SUCCESS) error = ERROR_FILE_INVALID;
        UnmapViewOfFile(mapped);
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    bool viewOwned = false;
    try {
        auto block = std::make_shared<ByteSequenceSnapshot::State::Block>();
        block->mapped = static_cast<const std::uint8_t*>(mapped);
        block->mappedSize = requested;
        viewOwned = true;
        auto next = std::make_shared<ByteSequenceSnapshot::State>();
        next->size = requested;
        next->pieces.push_back({std::move(block), 0, requested, 0});
        storage = std::move(next);
    } catch (...) {
        if (!viewOwned) UnmapViewOfFile(mapped);
        error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    stamp = completed;
    return true;
}

bool ByteDocument::SpliceStorage(std::size_t offset, std::size_t removeCount,
    std::span<const std::uint8_t> insertion) noexcept {
    const std::size_t currentSize = Size();
    if (offset > currentSize || removeCount > currentSize - offset ||
        insertion.size() > static_cast<std::size_t>(PTRDIFF_MAX) -
            (currentSize - removeCount)) return false;
    // Sequential typing commonly inserts immediately after the same private
    // block. When no worker snapshot shares the state or block, extend it in
    // place and shift only following piece offsets. This keeps long input linear
    // while preserving immutable storage for every published snapshot.
    if (removeCount == 0 && !insertion.empty() && storage_ && storage_.use_count() == 1) {
        for (std::size_t index = 0; index < storage_->pieces.size(); ++index) {
            auto& piece = storage_->pieces[index];
            if (piece.documentOffset + piece.length != offset ||
                !piece.block || piece.block->IsMapped() || piece.block.use_count() != 1 ||
                piece.blockOffset + piece.length != piece.block->owned.size()) continue;
            try {
                piece.block->owned.insert(piece.block->owned.end(),
                    insertion.begin(), insertion.end());
            } catch (...) { return false; }
            piece.length += insertion.size();
            storage_->size += insertion.size();
            for (std::size_t following = index + 1;
                following < storage_->pieces.size(); ++following)
                storage_->pieces[following].documentOffset += insertion.size();
            return true;
        }
    }
    try {
        auto next = std::make_shared<ByteSequenceSnapshot::State>();
        next->size = currentSize - removeCount + insertion.size();
        const std::size_t reserveCount = storage_ ? storage_->pieces.size() + 3 : 1;
        next->pieces.reserve(reserveCount);
        std::size_t documentOffset{};
        if (storage_) AppendStorageRange(*storage_, 0, offset,
            next->pieces, documentOffset);
        if (!insertion.empty()) {
            auto block = std::make_shared<ByteSequenceSnapshot::State::Block>();
            block->owned.assign(insertion.begin(), insertion.end());
            AppendStoragePiece(next->pieces, block, 0,
                insertion.size(), documentOffset);
        }
        if (storage_) AppendStorageRange(*storage_, offset + removeCount,
            currentSize, next->pieces, documentOffset);
        if (documentOffset != next->size) return false;
        storage_ = std::move(next);
        return true;
    } catch (...) {
        return false;
    }
}

void ByteDocument::RemapSavedFile() noexcept {
    if (path_.empty()) return;
    std::shared_ptr<ByteSequenceSnapshot::State> storage;
    FileStamp stamp{};
    DWORD ignored{};
    if (!CreateReadOnlyStorage(path_, storage, stamp, ignored)) return;
    storage_ = std::move(storage);
    diskStamp_ = stamp;
}

void ByteDocument::CloseChangeNotification() noexcept {
    if (changeNotification_ == INVALID_HANDLE_VALUE) return;
    FindCloseChangeNotification(changeNotification_);
    changeNotification_ = INVALID_HANDLE_VALUE;
}

void ByteDocument::RefreshChangeNotification() noexcept {
    CloseChangeNotification();
    externalChangePending_ = false;
    if (path_.empty()) return;
    try {
        std::filesystem::path directory = path_.parent_path();
        if (directory.empty()) directory = L".";
        changeNotification_ = FindFirstChangeNotificationW(directory.c_str(), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES);
    } catch (...) {
        changeNotification_ = INVALID_HANDLE_VALUE;
    }
}

void ByteDocument::ResetDirtyTracking() noexcept {
    dirtyRanges_.clear();
    rewriteFrom_.reset();
    forceFullSave_ = false;
}

void ByteDocument::BumpRevision() noexcept {
    // Zero represents a pristine/newly loaded baseline. Wrapping is practically
    // unreachable, but reserving zero keeps comparisons unambiguous forever.
    ++revision_;
    if (revision_ == 0) revision_ = 1;
}

bool ByteDocument::TrackOverwrite(std::size_t offset, std::size_t count) noexcept {
    if (count == 0 || forceFullSave_) return true;
    if (offset > std::numeric_limits<std::size_t>::max() - count) return false;
    std::size_t end = offset + count;
    if (rewriteFrom_) {
        if (offset >= *rewriteFrom_) return true;
        end = std::min(end, *rewriteFrom_);
    }
    if (end <= offset) return true;

    // Do not collapse thousands of distant ranges into one potentially huge
    // in-place write. The bounded-memory fallback is a full atomic publication,
    // which is no more I/O than the worst collapsed span and cannot tear the
    // destination if the process terminates during the save.
    if (dirtyRanges_.size() >= kMaxDirtyRanges) {
        dirtyRanges_.clear();
        rewriteFrom_.reset();
        forceFullSave_ = true;
        return true;
    }

    std::size_t first = 0;
    while (first < dirtyRanges_.size() && dirtyRanges_[first].end < offset) ++first;
    std::size_t last = first;
    std::size_t mergedBegin = offset;
    std::size_t mergedEnd = end;
    while (last < dirtyRanges_.size() && dirtyRanges_[last].begin <= mergedEnd) {
        mergedBegin = std::min(mergedBegin, dirtyRanges_[last].begin);
        mergedEnd = std::max(mergedEnd, dirtyRanges_[last].end);
        ++last;
    }
    if (first == last) {
        try {
            if (dirtyRanges_.size() == dirtyRanges_.capacity()) {
                const std::size_t capacity = std::max<std::size_t>(8, dirtyRanges_.capacity() * 2);
                dirtyRanges_.reserve(std::min(kMaxDirtyRanges, capacity));
            }
            dirtyRanges_.insert(dirtyRanges_.begin() + static_cast<std::ptrdiff_t>(first),
                DirtyRange{mergedBegin, mergedEnd});
        } catch (...) {
            return false;
        }
        return true;
    }
    dirtyRanges_[first] = {mergedBegin, mergedEnd};
    dirtyRanges_.erase(dirtyRanges_.begin() + static_cast<std::ptrdiff_t>(first + 1),
        dirtyRanges_.begin() + static_cast<std::ptrdiff_t>(last));
    return true;
}

void ByteDocument::TrackStructuralChange(std::size_t offset) noexcept {
    if (forceFullSave_ || (rewriteFrom_ && *rewriteFrom_ <= offset)) return;
    rewriteFrom_ = offset;
    auto firstCovered = dirtyRanges_.begin();
    while (firstCovered != dirtyRanges_.end() && firstCovered->end <= offset) ++firstCovered;
    if (firstCovered != dirtyRanges_.end() && firstCovered->begin < offset) {
        firstCovered->end = offset;
        ++firstCovered;
    }
    dirtyRanges_.erase(firstCovered, dirtyRanges_.end());
}

bool ByteDocument::Load(const std::filesystem::path& path, DWORD& error) {
    error = ERROR_SUCCESS;
    std::filesystem::path loadedPath;
    try { loadedPath = path; }
    catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    std::shared_ptr<ByteSequenceSnapshot::State> incomingStorage;
    FileStamp completedStamp{};
    if (!CreateReadOnlyStorage(path, incomingStorage, completedStamp, error))
        return false;
    storage_ = std::move(incomingStorage);
    path_ = std::move(loadedPath);
    diskStamp_ = completedStamp;
    // Loading a different file ends the previous document session. Empty swaps
    // return the outer history allocations as well as every Edit payload to the
    // allocator; clear() alone would retain a potentially large history array.
    std::vector<Edit>{}.swap(undo_);
    std::vector<Edit>{}.swap(redo_);
    historyBytes_ = 0; revision_ = 0; dirty_ = false;
    std::vector<DirtyRange>{}.swap(dirtyRanges_);
    ResetDirtyTracking();
    RefreshChangeNotification();
    return true;
}

bool ByteDocument::WriteTo(const std::filesystem::path& path, DWORD& error) const {
    error = ERROR_SUCCESS;
    if (path.empty()) { error = ERROR_INVALID_NAME; return false; }
    if (!CheckCurrentTargetWritable(path, error)) return false;
    // A CNG-generated sibling name prevents pre-creation attacks and retains the
    // same-volume atomic rename guarantee. Retry only genuine name collisions.
    std::filesystem::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    try {
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            const std::wstring random = RandomHexName();
            if (random.empty()) { error = ERROR_GEN_FAILURE; return false; }
            temporary = std::filesystem::path(path.wstring() + L".binedit." + random + L".tmp");
            file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                PreferCurrentReadOnlyError(path, error);
                return false;
            }
        }
        if (file == INVALID_HANDLE_VALUE) return false;
        bool mutated = false;
        if (!WriteStorageRange(file, 0, Size(), error, mutated)) {
            CloseHandle(file); file = INVALID_HANDLE_VALUE;
            DeleteFileW(temporary.c_str()); return false;
        }
        if (!FlushFileBuffers(file)) error = GetLastError();
        CloseHandle(file); file = INVALID_HANDLE_VALUE;
        if (error != ERROR_SUCCESS) { DeleteFileW(temporary.c_str()); return false; }

        // ReplaceFile preserves the destination's DACL, encryption, compression,
        // and other security-sensitive metadata. Only a genuinely absent target
        // uses MoveFileEx to publish the newly created file atomically.
        if (ReplaceFileW(path.c_str(), temporary.c_str(), nullptr,
            REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) return true;
        error = GetLastError();
        const DWORD targetAttributes = GetFileAttributesW(path.c_str());
        const DWORD attributeError = GetLastError();
        if (targetAttributes == INVALID_FILE_ATTRIBUTES &&
            (attributeError == ERROR_FILE_NOT_FOUND || attributeError == ERROR_PATH_NOT_FOUND)) {
            if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
            error = GetLastError();
        }
        DeleteFileW(temporary.c_str());
        PreferCurrentReadOnlyError(path, error);
        return false;
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
}

bool ByteDocument::WriteStorageRange(HANDLE file, std::size_t offset,
    std::size_t count, DWORD& error, bool& mutated) const noexcept {
    if (offset > Size() || count > Size() - offset) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }
    std::vector<std::uint8_t> buffer;
    try { buffer.resize(std::min<std::size_t>(count, 4u * 1024u * 1024u)); }
    catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    std::size_t done{};
    while (done < count) {
        const std::size_t chunkSize = std::min(buffer.size(), count - done);
        if (!CopyRange(offset + done, std::span<std::uint8_t>(buffer).first(chunkSize))) {
            error = ERROR_READ_FAULT;
            return false;
        }
        DWORD written{};
        if (!WriteFile(file, buffer.data(), static_cast<DWORD>(chunkSize),
            &written, nullptr) || written != chunkSize) {
            error = GetLastError() == ERROR_SUCCESS ? ERROR_WRITE_FAULT : GetLastError();
            return false;
        }
        mutated = true;
        done += written;
    }
    return true;
}

bool ByteDocument::WritePendingChanges(DWORD& error) {
    error = ERROR_SUCCESS;
    if (dirtyRanges_.empty() && !rewriteFrom_) return true;

    // FILE_SHARE_READ permits viewers while denying concurrent writers and
    // replacement for the complete sparse-write transaction.
    HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        PreferCurrentReadOnlyError(path_, error);
        return false;
    }

    FileStamp lockedStamp{};
    if (!QueryHandleStamp(file, lockedStamp, error) ||
        (diskStamp_.known && !SameFileStamp(diskStamp_, lockedStamp))) {
        if (error == ERROR_SUCCESS) error = ERROR_FILE_INVALID;
        CloseHandle(file);
        return false;
    }

    bool mutated = false;
    const auto writeAt = [&](std::size_t offset, std::size_t count) -> bool {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
            error = GetLastError();
            return false;
        }
        return WriteStorageRange(file, offset, count, error, mutated);
    };

    bool completed = true;
    for (const DirtyRange& range : dirtyRanges_) {
        if (!writeAt(range.begin, range.end - range.begin)) {
            completed = false;
            break;
        }
    }
    if (completed && rewriteFrom_) {
        const std::size_t offset = *rewriteFrom_;
        if (offset < Size() && !writeAt(offset, Size() - offset)) {
            completed = false;
        }
        if (completed) {
            LARGE_INTEGER end{};
            end.QuadPart = static_cast<LONGLONG>(Size());
            if (!SetFilePointerEx(file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
                error = GetLastError();
                completed = false;
            } else {
                // A tail deletion may perform no WriteFile call but still mutates
                // the durable stream length.
                mutated = true;
            }
        }
    }
    if (completed && !FlushFileBuffers(file)) {
        error = GetLastError();
        completed = false;
    }
    CloseHandle(file);

    if (!completed && mutated) {
        // A device or filter failure can occur after a prefix has reached disk.
        // Re-baseline that partial state and require a full atomic retry so the
        // remaining original dirty-range map is never applied to mixed bytes.
        FileStamp partial{};
        DWORD ignored{};
        if (QueryFileStamp(path_, partial, ignored)) diskStamp_ = partial;
        else diskStamp_ = {};
        forceFullSave_ = true;
        RefreshChangeNotification();
    }
    return completed;
}

bool ByteDocument::SaveInternal(DWORD& error, bool forceAtomic) {
    if (path_.empty()) { error = ERROR_INVALID_NAME; return false; }
    if (!CheckCurrentTargetWritable(path_, error)) return false;
    FileStamp current{};
    if (!QueryFileStamp(path_, current, error)) return false;
    if (diskStamp_.known && !SameFileStamp(diskStamp_, current)) {
        error = ERROR_FILE_INVALID;
        return false;
    }

    // A mapped view pins the current stream length. SetEndOfFile therefore
    // rejects shrink operations with ERROR_USER_MAPPED_FILE. More subtly, an
    // in-place insertion can overwrite mapped source bytes before a later
    // suffix chunk has copied them. Publish every structural edit through the
    // bounded-buffer sibling-file path while keeping ordinary overwrites sparse.
    const bool mappedStructuralEdit = rewriteFrom_.has_value() && IsFileMapped();
    if (forceAtomic || forceFullSave_ || mappedStructuralEdit) {
        if (!WriteTo(path_, error)) return false;
    } else if (!WritePendingChanges(error)) {
        return false;
    }

    // Replacement updates the file identity and in-place writes update metadata.
    // A successful persistence result remains valid even if this best-effort
    // monitoring refresh is temporarily unavailable.
    FileStamp saved{};
    DWORD stampError{};
    diskStamp_ = QueryFileStamp(path_, saved, stampError) ? saved : FileStamp{};
    dirty_ = false;
    ResetDirtyTracking();
    RefreshChangeNotification();
    RemapSavedFile();
    error = ERROR_SUCCESS;
    return true;
}

bool ByteDocument::Save(DWORD& error) { return SaveInternal(error, false); }

bool ByteDocument::SaveAtomically(DWORD& error) { return SaveInternal(error, true); }

bool ByteDocument::SaveAs(const std::filesystem::path& path, DWORD& error) {
    std::filesystem::path destination;
    try { destination = path; }
    catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    if (!WriteTo(destination, error)) return false;
    path_ = std::move(destination);
    FileStamp saved{};
    DWORD stampError{};
    diskStamp_ = QueryFileStamp(path_, saved, stampError) ? saved : FileStamp{};
    dirty_ = false;
    ResetDirtyTracking();
    RefreshChangeNotification();
    RemapSavedFile();
    return true;
}

bool ByteDocument::WriteCopy(const std::filesystem::path& path, DWORD& error) const { return WriteTo(path, error); }

void ByteDocument::NewEmpty() {
    // Closing the final file reuses this ByteDocument object for the Untitled
    // placeholder. clear() would make the logical document empty while retaining
    // the entire file buffer and history capacities. Swapping with genuinely
    // empty owners releases those allocations immediately and deterministically.
    CloseChangeNotification();
    storage_ = std::make_shared<ByteSequenceSnapshot::State>();
    std::filesystem::path emptyPath;
    path_.swap(emptyPath);
    std::vector<Edit>{}.swap(undo_);
    std::vector<Edit>{}.swap(redo_);
    std::vector<DirtyRange>{}.swap(dirtyRanges_);
    historyBytes_ = 0; revision_ = 0; dirty_ = false;
    diskStamp_ = {};
    ResetDirtyTracking();
    externalChangePending_ = false;
}

void ByteDocument::AdoptPath(const std::filesystem::path& path, bool dirty) {
    path_ = path;
    dirty_ = dirty;
    if (dirty) BumpRevision();
    ResetDirtyTracking();
    forceFullSave_ = dirty;
    DWORD error{};
    FileStamp current{};
    diskStamp_ = QueryFileStamp(path_, current, error) ? current : FileStamp{};
    RefreshChangeNotification();
}

void ByteDocument::RestoreSnapshot(std::vector<std::uint8_t>&& bytes,
    const std::filesystem::path& originalPath) {
    auto block = std::make_shared<ByteSequenceSnapshot::State::Block>();
    block->owned = std::move(bytes);
    auto storage = std::make_shared<ByteSequenceSnapshot::State>();
    storage->size = block->owned.size();
    if (storage->size != 0)
        storage->pieces.push_back({std::move(block), 0, storage->size, 0});
    storage_ = std::move(storage);
    path_ = originalPath;
    std::vector<Edit>{}.swap(undo_);
    std::vector<Edit>{}.swap(redo_);
    std::vector<DirtyRange>{}.swap(dirtyRanges_);
    historyBytes_ = 0; dirty_ = true;
    BumpRevision();
    ResetDirtyTracking();
    forceFullSave_ = true;
    DWORD error{};
    FileStamp current{};
    diskStamp_ = QueryFileStamp(path_, current, error) ? current : FileStamp{};
    RefreshChangeNotification();
}

void ByteDocument::MarkClean() noexcept {
    dirty_ = false;
    ResetDirtyTracking();
}

ExternalFileState ByteDocument::CheckExternalChange(DWORD& error, bool forceQuery) {
    error = ERROR_SUCCESS;
    if (path_.empty() || !diskStamp_.known) return ExternalFileState::Unchanged;

    // Directory notifications avoid touching file metadata every timer tick.
    // A missing notification handle intentionally falls through to polling.
    if (!forceQuery && !externalChangePending_ && changeNotification_ != INVALID_HANDLE_VALUE) {
        const DWORD wait = WaitForSingleObject(changeNotification_, 0);
        if (wait == WAIT_TIMEOUT) return ExternalFileState::Unchanged;
        if (wait == WAIT_OBJECT_0) {
            externalChangePending_ = true;
            if (!FindNextChangeNotification(changeNotification_)) CloseChangeNotification();
        } else {
            CloseChangeNotification();
        }
    }

    FileStamp current{};
    if (!QueryFileStamp(path_, current, error)) return ExternalFileState::Unavailable;
    if (SameFileStamp(diskStamp_, current)) {
        externalChangePending_ = false;
        return ExternalFileState::Unchanged;
    }
    externalChangePending_ = true;
    return current.exists ? ExternalFileState::Changed : ExternalFileState::Missing;
}

bool ByteDocument::ContinueAfterExternalChange(DWORD& error) {
    FileStamp current{};
    if (!QueryFileStamp(path_, current, error)) return false;
    diskStamp_ = current;
    dirty_ = true;
    forceFullSave_ = true;
    RefreshChangeNotification();
    return true;
}

bool ByteDocument::SetByte(std::size_t offset, std::uint8_t value) {
    if (offset >= Size()) return false;
    const std::uint8_t previous = ByteAt(offset);
    if (previous == value) return false;
    Edit edit;
    try {
        edit = {offset, {previous}, {value}};
        ReserveHistorySlot(undo_);
    } catch (...) { return false; }
    if (!TrackOverwrite(offset, 1)) return false;
    if (!SpliceStorage(offset, 1, edit.after)) {
        forceFullSave_ = true;
        dirtyRanges_.clear();
        return false;
    }
    for (const auto& redo : redo_) historyBytes_ -= EditBytes(redo);
    redo_.clear();
    while (!undo_.empty() && (undo_.size() >= kMaxHistory ||
        historyBytes_ > kMaxHistoryBytes - EditBytes(edit))) {
        historyBytes_ -= EditBytes(undo_.front());
        undo_.erase(undo_.begin());
    }
    historyBytes_ += EditBytes(edit);
    undo_.push_back(std::move(edit));
    dirty_ = true;
    BumpRevision();
    return true;
}

bool ByteDocument::AppendByte(std::uint8_t value) {
    // Keep the single-byte EOF fast path explicit; InsertRange handles arbitrary
    // positions and multi-byte character sequences.
    const std::size_t originalSize = Size();
    if (originalSize == static_cast<std::size_t>(PTRDIFF_MAX)) return false;
    Edit edit;
    try {
        edit = {originalSize, {}, {value}};
        ReserveHistorySlot(undo_);
    } catch (...) { return false; }
    TrackStructuralChange(originalSize);
    if (!SpliceStorage(originalSize, 0, edit.after)) return false;
    for (const auto& redo : redo_) historyBytes_ -= EditBytes(redo);
    redo_.clear();
    while (!undo_.empty() && (undo_.size() >= kMaxHistory ||
        historyBytes_ > kMaxHistoryBytes - EditBytes(edit))) {
        historyBytes_ -= EditBytes(undo_.front()); undo_.erase(undo_.begin());
    }
    historyBytes_ += EditBytes(edit);
    undo_.push_back(std::move(edit));
    dirty_ = true;
    BumpRevision();
    return true;
}

bool ByteDocument::InsertRange(std::size_t offset, std::span<const std::uint8_t> bytes) {
    const std::size_t originalSize = Size();
    if (bytes.empty() || offset > originalSize || bytes.size() > kMaxHistoryBytes ||
        bytes.size() > static_cast<std::size_t>(PTRDIFF_MAX) - originalSize) return false;
    Edit edit;
    edit.offset = offset;
    // Copy first because callers may pass storage that is released by a future
    // piece-table replacement. The immutable edit block owns its bytes.
    try {
        edit.after.assign(bytes.begin(), bytes.end());
        ReserveHistorySlot(undo_);
    } catch (...) { return false; }
    TrackStructuralChange(offset);
    if (!SpliceStorage(offset, 0, edit.after)) return false;
    for (const auto& redo : redo_) historyBytes_ -= EditBytes(redo);
    redo_.clear();
    while (!undo_.empty() && (undo_.size() >= kMaxHistory ||
        historyBytes_ > kMaxHistoryBytes - EditBytes(edit))) {
        historyBytes_ -= EditBytes(undo_.front()); undo_.erase(undo_.begin());
    }
    historyBytes_ += EditBytes(edit);
    undo_.push_back(std::move(edit));
    dirty_ = true;
    BumpRevision();
    return true;
}

bool ByteDocument::ReplaceRange(std::size_t offset, std::span<const std::uint8_t> replacement) {
    // A zero-length replacement is a no-op. Subtraction after the offset check
    // keeps the bounds test correct even near SIZE_MAX.
    const std::size_t currentSize = Size();
    if (replacement.empty() || offset > currentSize || replacement.size() > currentSize - offset ||
        replacement.size() > kMaxHistoryBytes / 2) return false;

    Edit edit;
    edit.offset = offset;
    try {
        edit.before.resize(replacement.size());
        if (!CopyRange(offset, edit.before)) return false;
        edit.after.assign(replacement.begin(), replacement.end());
        ReserveHistorySlot(undo_);
    } catch (...) { return false; }
    if (edit.before == edit.after) return false;
    if (!TrackOverwrite(offset, replacement.size())) return false;
    if (!SpliceStorage(offset, replacement.size(), edit.after)) {
        forceFullSave_ = true;
        dirtyRanges_.clear();
        return false;
    }
    for (const auto& redo : redo_) historyBytes_ -= EditBytes(redo);
    redo_.clear();
    while (!undo_.empty() && (undo_.size() >= kMaxHistory ||
        historyBytes_ > kMaxHistoryBytes - EditBytes(edit))) {
        historyBytes_ -= EditBytes(undo_.front()); undo_.erase(undo_.begin());
    }
    historyBytes_ += EditBytes(edit);
    undo_.push_back(std::move(edit));
    dirty_ = true;
    BumpRevision();
    return true;
}

bool ByteDocument::EraseRange(std::size_t offset, std::size_t count) {
    const std::size_t currentSize = Size();
    if (count == 0 || offset >= currentSize) return false;
    count = std::min(count, currentSize - offset);
    if (count > kMaxHistoryBytes) return false;
    Edit edit;
    edit.offset = offset;
    try {
        edit.before.resize(count);
        if (!CopyRange(offset, edit.before)) return false;
        ReserveHistorySlot(undo_);
    } catch (...) { return false; }
    TrackStructuralChange(offset);
    if (!SpliceStorage(offset, count, {})) return false;
    for (const auto& redo : redo_) historyBytes_ -= EditBytes(redo);
    redo_.clear();
    while (!undo_.empty() && (undo_.size() >= kMaxHistory ||
        historyBytes_ > kMaxHistoryBytes - EditBytes(edit))) {
        historyBytes_ -= EditBytes(undo_.front()); undo_.erase(undo_.begin());
    }
    historyBytes_ += EditBytes(edit);
    undo_.push_back(std::move(edit));
    dirty_ = true;
    BumpRevision();
    return true;
}

bool ByteDocument::Undo(std::size_t& changedOffset, std::size_t* changedCount) {
    if (undo_.empty()) return false;
    const auto& pending = undo_.back();
    const std::size_t currentSize = Size();
    if (pending.offset > currentSize || pending.after.size() > currentSize - pending.offset) return false;
    try {
        ReserveHistorySlot(redo_);
    } catch (...) { return false; }
    if (pending.before.size() == pending.after.size()) {
        if (!TrackOverwrite(pending.offset, pending.before.size())) return false;
    } else {
        TrackStructuralChange(pending.offset);
    }
    if (!SpliceStorage(pending.offset, pending.after.size(), pending.before)) return false;
    auto edit = std::move(undo_.back()); undo_.pop_back();
    changedOffset = edit.offset;
    if (changedCount) *changedCount = std::max(edit.before.size(), edit.after.size());
    redo_.push_back(std::move(edit)); dirty_ = true;
    BumpRevision();
    return true;
}

bool ByteDocument::Redo(std::size_t& changedOffset, std::size_t* changedCount) {
    if (redo_.empty()) return false;
    const auto& pending = redo_.back();
    const std::size_t currentSize = Size();
    if (pending.offset > currentSize || pending.before.size() > currentSize - pending.offset) return false;
    try {
        ReserveHistorySlot(undo_);
    } catch (...) { return false; }
    if (pending.before.size() == pending.after.size()) {
        if (!TrackOverwrite(pending.offset, pending.after.size())) return false;
    } else {
        TrackStructuralChange(pending.offset);
    }
    if (!SpliceStorage(pending.offset, pending.before.size(), pending.after)) return false;
    auto edit = std::move(redo_.back()); redo_.pop_back();
    changedOffset = edit.offset;
    if (changedCount) *changedCount = std::max(edit.before.size(), edit.after.size());
    undo_.push_back(std::move(edit)); dirty_ = true;
    BumpRevision();
    return true;
}

std::optional<SearchPattern> ParseBinaryPattern(const std::wstring& text, SearchPatternError& error) {
    error = SearchPatternError::None;
    if (text.size() > kMaxSearchCharacters) {
        error = SearchPatternError::TooLarge;
        return std::nullopt;
    }
    try {
        SearchPattern result; result.displayText = text;
        // At most one PatternByte is produced per two input characters. The
        // reserve is intentionally conservative and never exceeds the input cap.
        result.bytes.reserve((text.size() + 1) / 2);
        std::size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && (iswspace(text[i]) || text[i] == L',' || text[i] == L'-')) ++i;
            if (i >= text.size()) break;
            // Optional 0x prefixes are accepted per byte, but compact unseparated
            // strings are rejected to make wildcard boundaries unambiguous.
            if (i + 1 < text.size() && text[i] == L'0' && (text[i + 1] == L'x' || text[i + 1] == L'X')) i += 2;
            if (i + 1 < text.size() && text[i] == L'?' && text[i + 1] == L'?') {
                result.bytes.push_back({0, true}); i += 2; continue;
            }
            if (i + 1 >= text.size() || HexDigit(text[i]) < 0 || HexDigit(text[i + 1]) < 0) {
                error = SearchPatternError::InvalidHex; return std::nullopt;
            }
            result.bytes.push_back({static_cast<std::uint8_t>((HexDigit(text[i]) << 4) | HexDigit(text[i + 1])), false});
            i += 2;
            if (i < text.size() && !iswspace(text[i]) && text[i] != L',' && text[i] != L'-') {
                error = SearchPatternError::MissingSeparator; return std::nullopt;
            }
        }
        if (result.bytes.empty()) { error = SearchPatternError::Empty; return std::nullopt; }
        return result;
    } catch (const std::bad_alloc&) {
        error = SearchPatternError::AllocationFailed;
        return std::nullopt;
    } catch (const std::length_error&) {
        error = SearchPatternError::AllocationFailed;
        return std::nullopt;
    }
}

std::optional<SearchPattern> EncodeTextPattern(const std::wstring& text, TextEncoding encoding, SearchPatternError& error) {
    error = SearchPatternError::None;
    if (text.empty()) { error = SearchPatternError::Empty; return std::nullopt; }
    if (text.size() > kMaxSearchCharacters || text.size() > static_cast<std::size_t>(INT_MAX)) {
        error = SearchPatternError::TooLarge;
        return std::nullopt;
    }
    try {
        SearchPattern result; result.displayText = text;
        // wchar_t is UTF-16 on Windows. Emit explicit low/high bytes so host
        // endianness is never an implicit file-format assumption.
        if (encoding == TextEncoding::Utf16Le) {
            result.bytes.reserve(text.size() * 2);
            for (wchar_t ch : text) {
                result.bytes.push_back({static_cast<std::uint8_t>(ch & 0xff), false});
                result.bytes.push_back({static_cast<std::uint8_t>((ch >> 8) & 0xff), false});
            }
            return result;
        }
        const int sourceLength = static_cast<int>(text.size());
        std::vector<char> encoded;
        if (encoding == TextEncoding::Utf8) {
            // Windows requires both default-character parameters to be null for
            // CP_UTF8. WC_ERR_INVALID_CHARS rejects malformed UTF-16 instead.
            const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                text.data(), sourceLength, nullptr, 0, nullptr, nullptr);
            if (needed <= 0) {
                error = SearchPatternError::Unrepresentable;
                return std::nullopt;
            }
            encoded.resize(static_cast<std::size_t>(needed));
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                sourceLength, encoded.data(), needed, nullptr, nullptr)) {
                error = SearchPatternError::EncodingFailed;
                return std::nullopt;
            }
        } else {
            const UINT codePage = encoding == TextEncoding::ShiftJis ? 932u : 20127u;
            // Legacy code pages support lpUsedDefaultChar. Rejecting any best-fit
            // substitution keeps searched bytes identical to the visible query.
            BOOL usedDefault = FALSE;
            const int needed = WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS,
                text.data(), sourceLength, nullptr, 0, nullptr, &usedDefault);
            if (needed <= 0 || usedDefault) {
                error = SearchPatternError::Unrepresentable;
                return std::nullopt;
            }
            encoded.resize(static_cast<std::size_t>(needed));
            usedDefault = FALSE;
            if (!WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS, text.data(),
                sourceLength, encoded.data(), needed, nullptr, &usedDefault) || usedDefault) {
                error = SearchPatternError::EncodingFailed;
                return std::nullopt;
            }
        }
        result.bytes.reserve(encoded.size());
        for (unsigned char ch : encoded) result.bytes.push_back({ch, false});
        return result;
    } catch (const std::bad_alloc&) {
        error = SearchPatternError::AllocationFailed;
        return std::nullopt;
    } catch (const std::length_error&) {
        error = SearchPatternError::AllocationFailed;
        return std::nullopt;
    }
}

std::optional<std::size_t> FindPattern(std::span<const std::uint8_t> bytes, const SearchPattern& pattern, std::size_t start) {
    if (pattern.bytes.empty() || pattern.bytes.size() > bytes.size() || start > bytes.size() - pattern.bytes.size()) return std::nullopt;
    WildcardAutomaton automaton(pattern);
    // A pattern made only of wildcards matches every legal candidate, so the
    // requested start is already the first match.
    if (automaton.Empty()) return start;
    const std::size_t candidateCount = bytes.size() - pattern.bytes.size() + 1;
    // A match discovered anywhere in a window may still be preceded by a
    // smaller candidate from a later fragment hit, so only the window itself is
    // abandoned once a match exists. A small window keeps that redundant scan
    // and the dedupe bitmap cache-resident close to Find Next call sites.
    constexpr std::size_t candidatesPerWindow = 4096u;
    std::vector<std::uint8_t> verified((candidatesPerWindow + 7u) / 8u);
    for (std::size_t windowBegin = start; windowBegin < candidateCount;) {
        const std::size_t windowCandidates = std::min(candidatesPerWindow, candidateCount - windowBegin);
        const std::size_t windowBytes = windowCandidates + pattern.bytes.size() - 1;
        const std::size_t verifiedBytes = (windowCandidates + 7u) / 8u;
        std::fill_n(verified.begin(), verifiedBytes, std::uint8_t{0});
        std::optional<std::size_t> best;
        const auto buffer = bytes.subspan(windowBegin, windowBytes);
        static_cast<void>(ScanCandidateWindow(buffer, windowBegin, pattern, automaton,
            windowBegin, windowBegin + windowCandidates,
            std::span<std::uint8_t>(verified).first(verifiedBytes), {},
            [&](std::size_t candidate) {
                if (!best || candidate < *best) best = candidate;
            }));
        if (best) return best;
        windowBegin += windowCandidates;
    }
    return std::nullopt;
}

std::vector<std::size_t> FindAllPatterns(std::span<const std::uint8_t> bytes, const SearchPattern& pattern, std::size_t limit) {
    // Overlapping matches remain eligible; the bounded collector returns them
    // in ascending order even though fragment hits arrive by fragment end.
    std::vector<std::size_t> found;
    if (pattern.bytes.empty() || pattern.bytes.size() > bytes.size() || limit == 0) return found;
    const std::size_t candidateCount = bytes.size() - pattern.bytes.size() + 1;
    WildcardAutomaton automaton(pattern);
    if (automaton.Empty()) {
        const std::size_t count = std::min(candidateCount, limit);
        found.resize(count);
        for (std::size_t index = 0; index < count; ++index) found[index] = index;
        return found;
    }
    constexpr std::size_t candidatesPerWindow = 1024u * 1024u;
    std::vector<std::uint8_t> verified((candidatesPerWindow + 7u) / 8u);
    SmallestOffsets smallest(limit);
    for (std::size_t windowBegin = 0; windowBegin < candidateCount;) {
        const std::size_t windowCandidates = std::min(candidatesPerWindow, candidateCount - windowBegin);
        const std::size_t windowBytes = windowCandidates + pattern.bytes.size() - 1;
        const std::size_t verifiedBytes = (windowCandidates + 7u) / 8u;
        std::fill_n(verified.begin(), verifiedBytes, std::uint8_t{0});
        const auto buffer = bytes.subspan(windowBegin, windowBytes);
        static_cast<void>(ScanCandidateWindow(buffer, windowBegin, pattern, automaton,
            windowBegin, windowBegin + windowCandidates,
            std::span<std::uint8_t>(verified).first(verifiedBytes), {},
            [&](std::size_t candidate) { smallest.Add(candidate); }));
        windowBegin += windowCandidates;
    }
    return smallest.TakeSorted();
}

ParallelSearchResult FindPatternsParallel(ImmutableByteSnapshot snapshot, const SearchPattern& pattern,
    std::size_t start, std::size_t limit, std::stop_token stopToken, unsigned workerHint) {
    ParallelSearchResult result;
    if (!snapshot) {
        result.failed = true;
        return result;
    }
    const std::size_t byteCount = snapshot->Size();
    if (pattern.bytes.empty() || pattern.bytes.size() > byteCount) return result;

    std::optional<WildcardAutomaton> automaton;
    try {
        automaton.emplace(pattern);
    } catch (...) {
        result.failed = true;
        return result;
    }

    const std::size_t candidateCount = byteCount - pattern.bytes.size() + 1;
    constexpr std::size_t candidatesPerWorker = 256u * 1024u;
    constexpr unsigned maximumWorkers = 16;
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t usefulWorkers = std::max<std::size_t>(1,
        (candidateCount + candidatesPerWorker - 1) / candidatesPerWorker);
    const unsigned requested = workerHint == 0 ?
        static_cast<unsigned>(std::min<std::size_t>(usefulWorkers, maximumWorkers)) : workerHint;
    result.workerCount = std::max(1u, std::min({requested, hardware, maximumWorkers,
        static_cast<unsigned>(std::min<std::size_t>(candidateCount, maximumWorkers))}));

    struct WorkerResult {
        std::vector<std::size_t> offsets;
        std::optional<std::size_t> first;
        std::optional<std::size_t> forward;
        bool canceled{};
    };
    std::vector<WorkerResult> partial;
    try { partial.resize(result.workerCount); }
    catch (...) { result.failed = true; return result; }
    std::atomic_bool failed{};
    const bool allWildcards = automaton->Empty();

    auto scanPartition = [&](unsigned worker) {
        try {
            const std::size_t base = candidateCount / result.workerCount;
            const std::size_t remainder = candidateCount % result.workerCount;
            const std::size_t begin = worker * base + std::min<std::size_t>(worker, remainder);
            const std::size_t end = begin + base + (worker < remainder ? 1u : 0u);
            WorkerResult& local = partial[worker];
            SmallestOffsets smallest(limit);
            const auto record = [&](std::size_t at) {
                // Fragment order is not candidate order, so both Find Next
                // markers keep their exact minimum semantics.
                if (!local.first || at < *local.first) local.first = at;
                if (at >= start && (!local.forward || at < *local.forward)) local.forward = at;
                smallest.Add(at);
            };
            if (allWildcards) {
                // No literal fragment exists to anchor the automaton; every
                // candidate matches and only the bounded highlight set matters.
                for (std::size_t at = begin; at < end; ++at) {
                    if ((at & 0x3ffu) == 0 && (stopToken.stop_requested() ||
                        failed.load(std::memory_order_relaxed))) {
                        local.canceled = true;
                        return;
                    }
                    record(at);
                }
                local.offsets = smallest.TakeSorted();
                return;
            }
            constexpr std::size_t candidatesPerChunk = 1024u * 1024u;
            std::vector<std::uint8_t> chunk;
            std::vector<std::uint8_t> verified;
            const std::size_t maximumCandidates = std::min(candidatesPerChunk, end - begin);
            chunk.resize(maximumCandidates + pattern.bytes.size() - 1);
            verified.resize((maximumCandidates + 7u) / 8u);
            for (std::size_t chunkBegin = begin; chunkBegin < end;) {
                if (stopToken.stop_requested() || failed.load(std::memory_order_relaxed)) {
                    local.canceled = true;
                    return;
                }
                const std::size_t chunkCandidates = std::min(candidatesPerChunk, end - chunkBegin);
                const std::size_t chunkBytes = chunkCandidates + pattern.bytes.size() - 1;
                if (!snapshot->CopyRange(chunkBegin,
                    std::span<std::uint8_t>(chunk).first(chunkBytes))) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                const std::size_t verifiedBytes = (chunkCandidates + 7u) / 8u;
                std::fill_n(verified.begin(), verifiedBytes, std::uint8_t{0});
                if (!ScanCandidateWindow(std::span<const std::uint8_t>(chunk).first(chunkBytes),
                    chunkBegin, pattern, *automaton, chunkBegin, chunkBegin + chunkCandidates,
                    std::span<std::uint8_t>(verified).first(verifiedBytes), stopToken, record)) {
                    local.canceled = true;
                    return;
                }
                chunkBegin += chunkCandidates;
            }
            local.offsets = smallest.TakeSorted();
        } catch (...) {
            // Allocation failure in any worker makes the complete result
            // unusable. Other partitions observe failed and exit promptly.
            failed.store(true, std::memory_order_relaxed);
        }
    };

    if (result.workerCount == 1) {
        scanPartition(0);
    } else {
        std::vector<std::jthread> workers;
        try {
            workers.reserve(result.workerCount);
            for (unsigned worker = 0; worker < result.workerCount; ++worker) {
                workers.emplace_back([&, worker] {
                    SetThreadDescription(GetCurrentThread(), L"BinEdit parallel search");
                    scanPartition(worker);
                });
            }
        } catch (...) {
            // Set failure before the partially constructed vector joins. Every
            // live partition then observes the flag and exits promptly.
            failed.store(true, std::memory_order_relaxed);
        }
        // jthread destruction joins every partition before partial is read.
    }

    if (failed.load(std::memory_order_relaxed)) {
        result.failed = true;
        result.offsets.clear();
        return result;
    }
    if (stopToken.stop_requested() || std::ranges::any_of(partial,
        [](const WorkerResult& worker) { return worker.canceled; })) {
        result.canceled = true;
        return result;
    }

    try { result.offsets.reserve(std::min(limit, candidateCount)); }
    catch (...) { result.failed = true; return result; }
    std::optional<std::size_t> first;
    std::optional<std::size_t> forward;
    for (const WorkerResult& worker : partial) {
        // Visit metadata from every partition even after the highlight limit is
        // full, because Find Next may reside beyond the materialized highlights.
        if (worker.first && (!first || *worker.first < *first)) first = worker.first;
        if (worker.forward && (!forward || *worker.forward < *forward)) forward = worker.forward;
        const std::size_t remaining = limit - result.offsets.size();
        const std::size_t take = std::min(remaining, worker.offsets.size());
        try {
            result.offsets.insert(result.offsets.end(), worker.offsets.begin(),
                worker.offsets.begin() + static_cast<std::ptrdiff_t>(take));
        } catch (...) {
            result.offsets.clear();
            result.failed = true;
            return result;
        }
    }
    result.next = forward ? forward : first;
    return result;
}

std::wstring EncodingName(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Ascii: return L"ASCII";
    case TextEncoding::Utf8: return L"UTF-8";
    case TextEncoding::Utf16Le: return L"UTF-16 LE";
    case TextEncoding::ShiftJis: return L"Shift-JIS";
    }
    return L"ASCII";
}
