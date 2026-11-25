#include "BomParser.hpp"

#include <cerrno>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "FileUtil.hpp"

namespace vortex {

namespace {

constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kTreeHeaderSize = 21;
constexpr std::size_t kPageHeaderSize = 12;
constexpr std::size_t kPageEntrySize = 8;
constexpr std::size_t kPathInfo1Size = 8;
constexpr std::size_t kPathInfo2Size = 31;
constexpr std::size_t kMaxTreeDepth = 64;
constexpr std::size_t kCancellationStride = 4096;
constexpr char kMagic[] = {'B', 'O', 'M', 'S', 't', 'o', 'r', 'e'};
constexpr char kTreeMagic[] = {'t', 'r', 'e', 'e'};

/// Bounds-checked big-endian window onto the file image. Every accessor reports
/// failure instead of reading out of range; the arithmetic is written so that it
/// cannot overflow for any offset/length combination.
class Reader {
public:
    Reader() = default;
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }

    [[nodiscard]] bool u8(std::size_t offset, std::uint8_t& out) const noexcept {
        if (offset >= bytes_.size()) return false;
        out = bytes_[offset];
        return true;
    }

    [[nodiscard]] bool u16(std::size_t offset, std::uint16_t& out) const noexcept {
        if (bytes_.size() < 2 || offset > bytes_.size() - 2) return false;
        out = static_cast<std::uint16_t>((static_cast<unsigned>(bytes_[offset]) << 8) |
                                         static_cast<unsigned>(bytes_[offset + 1]));
        return true;
    }

    [[nodiscard]] bool u32(std::size_t offset, std::uint32_t& out) const noexcept {
        if (bytes_.size() < 4 || offset > bytes_.size() - 4) return false;
        out = (static_cast<std::uint32_t>(bytes_[offset]) << 24) |
              (static_cast<std::uint32_t>(bytes_[offset + 1]) << 16) |
              (static_cast<std::uint32_t>(bytes_[offset + 2]) << 8) |
              static_cast<std::uint32_t>(bytes_[offset + 3]);
        return true;
    }

    [[nodiscard]] bool slice(std::uint64_t offset, std::uint64_t length, Reader& out) const noexcept {
        const std::uint64_t total = bytes_.size();
        if (offset > total || length > total - offset) return false;
        out = Reader(bytes_.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length)));
        return true;
    }

private:
    std::span<const std::uint8_t> bytes_;
};

/// Resolves block indices to byte ranges through the block table.
class BlockTable {
public:
    BlockTable(Reader file, Reader pointers, std::uint32_t count) noexcept
        : file_(file), pointers_(pointers), count_(count) {}

    [[nodiscard]] bool block(std::uint32_t index, Reader& out) const noexcept {
        if (index == 0 || index >= count_) return false;
        std::uint32_t address = 0;
        std::uint32_t length = 0;
        const std::size_t offset = static_cast<std::size_t>(index) * 8;
        if (!pointers_.u32(offset, address) || !pointers_.u32(offset + 4, length)) return false;
        if (address == 0 && length == 0) return false;  // unallocated slot
        return file_.slice(address, length, out);
    }

private:
    Reader file_;
    Reader pointers_;
    std::uint32_t count_;
};

/// A leaf record before its path has been reconstructed.
struct RawRecord {
    std::uint32_t id = 0;
    std::uint32_t parent = 0;
    std::string_view name;  // points into the file image
    BomEntry entry;         // metadata; path filled in later
};

Error malformed(const char* what) {
    return Error{EINVAL, std::string("malformed BOM: ") + what};
}

bool validComponentName(std::string_view name, bool isRoot) noexcept {
    if (name.empty()) return false;
    if (name.find('/') != std::string_view::npos) return false;
    if (name == "..") return false;
    if (name == "." && !isRoot) return false;
    return true;
}

}  // namespace

Result<BomContents> BomParser::parse(std::span<const std::uint8_t> data, const BomParseOptions& options,
                                     const CancellationToken& token) {
    const Reader file(data);
    if (file.size() < kHeaderSize) return malformed("file is shorter than the header");
    if (std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) return malformed("missing 'BOMStore' magic");

    std::uint32_t version = 0;
    std::uint32_t numberOfBlocks = 0;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexLength = 0;
    std::uint32_t varsOffset = 0;
    std::uint32_t varsLength = 0;
    if (!file.u32(8, version) || !file.u32(12, numberOfBlocks) || !file.u32(16, indexOffset) ||
        !file.u32(20, indexLength) || !file.u32(24, varsOffset) || !file.u32(28, varsLength)) {
        return malformed("truncated header");
    }
    if (version != 1) return malformed("unsupported BOMStore version");

    // ---- Block table -------------------------------------------------------
    Reader index;
    if (!file.slice(indexOffset, indexLength, index)) return malformed("block table lies outside the file");
    std::uint32_t pointerCount = 0;
    if (!index.u32(0, pointerCount)) return malformed("truncated block table");
    Reader pointers;
    if (!index.slice(4, static_cast<std::uint64_t>(pointerCount) * 8, pointers)) {
        return malformed("block table is larger than its declared region");
    }
    const BlockTable blocks(file, pointers, pointerCount);

    // ---- Variable table ----------------------------------------------------
    Reader vars;
    if (!file.slice(varsOffset, varsLength, vars)) return malformed("variable table lies outside the file");
    std::uint32_t varCount = 0;
    if (!vars.u32(0, varCount)) return malformed("truncated variable table");

    BomContents contents;
    std::uint32_t pathsBlockIndex = 0;
    bool havePaths = false;
    {
        std::size_t cursor = 4;
        for (std::uint32_t i = 0; i < varCount; ++i) {
            std::uint32_t blockIndex = 0;
            std::uint8_t nameLength = 0;
            if (!vars.u32(cursor, blockIndex) || !vars.u8(cursor + 4, nameLength)) {
                return malformed("variable table entry is truncated");
            }
            Reader nameBytes;
            if (!vars.slice(cursor + 5, nameLength, nameBytes)) {
                return malformed("variable name runs past the table");
            }
            const std::string_view name(reinterpret_cast<const char*>(nameBytes.bytes().data()), nameLength);
            contents.variableNames.emplace_back(name);
            if (name == "Paths" && !havePaths) {
                pathsBlockIndex = blockIndex;
                havePaths = true;
            }
            cursor += 5 + static_cast<std::size_t>(nameLength);
        }
    }
    if (!havePaths) return malformed("no 'Paths' variable");

    // ---- Tree header -------------------------------------------------------
    Reader tree;
    if (!blocks.block(pathsBlockIndex, tree)) return malformed("'Paths' points at an invalid block");
    if (tree.size() < kTreeHeaderSize) return malformed("'Paths' tree header is truncated");
    if (std::memcmp(tree.bytes().data(), kTreeMagic, sizeof(kTreeMagic)) != 0) {
        return malformed("'Paths' block is not a tree");
    }
    std::uint32_t treeVersion = 0;
    std::uint32_t rootIndex = 0;
    std::uint32_t blockSize = 0;
    std::uint32_t declaredCount = 0;
    if (!tree.u32(4, treeVersion) || !tree.u32(8, rootIndex) || !tree.u32(12, blockSize) ||
        !tree.u32(16, declaredCount)) {
        return malformed("'Paths' tree header is truncated");
    }
    if (treeVersion != 1) return malformed("unsupported tree version");
    contents.declaredPathCount = declaredCount;

    // ---- Descend to the leftmost leaf --------------------------------------
    std::unordered_set<std::uint32_t> visitedPages;
    std::uint32_t pageIndex = rootIndex;
    Reader page;
    for (std::size_t depth = 0;; ++depth) {
        if (depth > kMaxTreeDepth) return malformed("tree is deeper than any valid BOM");
        if (!visitedPages.insert(pageIndex).second) return malformed("cycle between tree pages");
        if (!blocks.block(pageIndex, page)) return malformed("tree page lies outside the file");
        std::uint16_t isLeaf = 0;
        if (!page.u16(0, isLeaf)) return malformed("tree page header is truncated");
        if (isLeaf != 0) break;
        std::uint32_t leftmostChild = 0;
        if (!page.u32(kPageHeaderSize, leftmostChild)) return malformed("branch page has no child pointer");
        pageIndex = leftmostChild;
    }

    // ---- Walk the leaf chain -----------------------------------------------
    std::vector<RawRecord> records;
    // A record costs at least one 8-byte leaf entry, so the file size bounds the
    // number of records independently of any count stored in the file.
    const std::size_t structuralLimit = file.size() / kPageEntrySize;
    for (;;) {
        std::uint16_t isLeaf = 0;
        std::uint16_t count = 0;
        std::uint32_t forward = 0;
        if (!page.u16(0, isLeaf) || !page.u16(2, count) || !page.u32(4, forward)) {
            return malformed("leaf page header is truncated");
        }
        if (isLeaf == 0) return malformed("leaf chain runs into a branch page");
        if (kPageHeaderSize + static_cast<std::size_t>(count) * kPageEntrySize > page.size()) {
            return malformed("leaf page declares more entries than it holds");
        }
        for (std::size_t i = 0; i < count; ++i) {
            if ((records.size() % kCancellationStride) == 0 && token.isCancelled()) {
                return Error{ECANCELED, "BOM parsing cancelled"};
            }
            if (records.size() >= options.maxEntries || records.size() >= structuralLimit) {
                return malformed("too many path records");
            }
            const std::size_t entryOffset = kPageHeaderSize + i * kPageEntrySize;
            std::uint32_t infoIndex = 0;
            std::uint32_t fileIndex = 0;
            if (!page.u32(entryOffset, infoIndex) || !page.u32(entryOffset + 4, fileIndex)) {
                return malformed("leaf entry is truncated");
            }

            Reader info1;
            if (!blocks.block(infoIndex, info1) || info1.size() < kPathInfo1Size) {
                return malformed("leaf entry references an invalid path-info block");
            }
            RawRecord record;
            std::uint32_t info2Index = 0;
            if (!info1.u32(0, record.id) || !info1.u32(4, info2Index)) {
                return malformed("path-info block is truncated");
            }

            Reader info2;
            if (!blocks.block(info2Index, info2)) return malformed("path-info references an invalid detail block");
            if (info2.size() >= kPathInfo2Size) {
                std::uint8_t type = 0;
                std::uint32_t linkLength = 0;
                if (!info2.u8(0, type) || !info2.u16(4, record.entry.mode) || !info2.u32(6, record.entry.uid) ||
                    !info2.u32(10, record.entry.gid) || !info2.u32(14, record.entry.modificationTime) ||
                    !info2.u32(18, record.entry.size) || !info2.u32(23, record.entry.checksum) ||
                    !info2.u32(27, linkLength)) {
                    return malformed("path detail block is truncated");
                }
                record.entry.type = (type >= 1 && type <= 4) ? static_cast<BomEntryType>(type) : BomEntryType::Unknown;
                // Only symlink records carry a link name here. In records of Mach-O
                // files the same bytes start a per-architecture table instead, so
                // the field must not be read as a length for any other type.
                if (record.entry.type == BomEntryType::Symlink && linkLength != 0) {
                    Reader link;
                    if (!info2.slice(kPathInfo2Size, linkLength, link)) {
                        return malformed("symlink target runs past its block");
                    }
                    std::string_view target(reinterpret_cast<const char*>(link.bytes().data()), link.size());
                    if (const std::size_t nul = target.find('\0'); nul != std::string_view::npos) {
                        target = target.substr(0, nul);
                    }
                    record.entry.linkTarget.assign(target);
                }
            }

            Reader fileBlock;
            if (!blocks.block(fileIndex, fileBlock) || fileBlock.size() < 5) {
                return malformed("leaf entry references an invalid file-name block");
            }
            if (!fileBlock.u32(0, record.parent)) return malformed("file-name block is truncated");
            std::string_view name(reinterpret_cast<const char*>(fileBlock.bytes().data()) + 4, fileBlock.size() - 4);
            if (const std::size_t nul = name.find('\0'); nul != std::string_view::npos) {
                name = name.substr(0, nul);
            }
            record.name = name;
            records.push_back(std::move(record));
        }
        if (forward == 0) break;
        if (!visitedPages.insert(forward).second) return malformed("cycle in the leaf chain");
        if (!blocks.block(forward, page)) return malformed("leaf chain points outside the file");
    }

    // ---- Reconstruct full paths from parent links --------------------------
    enum : std::uint8_t { kUnresolved = 0, kInProgress = 1, kResolved = 2, kInvalid = 3 };
    const std::size_t total = records.size();
    std::vector<std::uint8_t> state(total, kUnresolved);
    std::vector<std::string> paths(total);
    std::unordered_map<std::uint32_t, std::uint32_t> indexById;
    indexById.reserve(total);

    for (std::size_t i = 0; i < total; ++i) {
        const RawRecord& record = records[i];
        const bool nameOk = validComponentName(record.name, record.parent == 0);
        if (!nameOk || record.id == 0 || !indexById.emplace(record.id, static_cast<std::uint32_t>(i)).second) {
            state[i] = kInvalid;  // unusable name, reserved id, or a second record with the same id
        }
    }

    const std::size_t maxChain = options.maxPathLength / 2 + 2;
    std::vector<std::uint32_t> chain;
    for (std::size_t i = 0; i < total; ++i) {
        if ((i % kCancellationStride) == 0 && token.isCancelled()) {
            return Error{ECANCELED, "BOM parsing cancelled"};
        }
        if (state[i] != kUnresolved) continue;

        chain.clear();
        bool valid = true;
        bool reachedRoot = false;
        std::uint32_t resolvedAncestor = 0;
        auto current = static_cast<std::uint32_t>(i);
        for (;;) {
            if (state[current] == kResolved) {
                resolvedAncestor = current;
                break;
            }
            if (state[current] != kUnresolved) {  // kInvalid, or kInProgress == a parent cycle
                valid = false;
                break;
            }
            state[current] = kInProgress;
            chain.push_back(current);
            if (chain.size() > maxChain) {
                valid = false;
                break;
            }
            const std::uint32_t parentId = records[current].parent;
            if (parentId == 0) {
                reachedRoot = true;
                break;
            }
            const auto found = indexById.find(parentId);
            if (found == indexById.end()) {
                valid = false;  // orphan: the parent record does not exist
                break;
            }
            current = found->second;
        }

        // Unwind from the outermost ancestor down to the record we started from.
        for (std::size_t k = chain.size(); k-- > 0;) {
            const std::uint32_t idx = chain[k];
            if (!valid) {
                state[idx] = kInvalid;
                continue;
            }
            std::string full;
            const bool isOutermost = (k + 1 == chain.size());
            if (isOutermost && reachedRoot) {
                full.assign(records[idx].name);
            } else {
                const std::string& base = isOutermost ? paths[resolvedAncestor] : paths[chain[k + 1]];
                full.reserve(base.size() + 1 + records[idx].name.size());
                full.append(base);
                full.push_back('/');
                full.append(records[idx].name);
            }
            if (full.size() > options.maxPathLength) {
                valid = false;  // this record and everything below it is rejected
                state[idx] = kInvalid;
                continue;
            }
            paths[idx] = std::move(full);
            state[idx] = kResolved;
        }
    }

    // A payload hangs off a single root called ".". Real receipts occasionally carry
    // a stray second root (a file such as ".TemporaryItems" with no parent); lsbom
    // never reaches it, and neither must we: it names nothing the package installs.
    bool hasDotRoot = false;
    for (std::size_t i = 0; i < total && !hasDotRoot; ++i) {
        hasDotRoot = state[i] == kResolved && records[i].parent == 0 && paths[i] == ".";
    }
    const auto belongsToPayload = [&](const std::string& path) {
        return !hasDotRoot || path == "." || path.starts_with("./");
    };

    contents.entries.reserve(total);
    for (std::size_t i = 0; i < total; ++i) {
        if (state[i] != kResolved || !belongsToPayload(paths[i])) {
            ++contents.skippedEntries;
            continue;
        }
        BomEntry entry = std::move(records[i].entry);
        entry.path = std::move(paths[i]);
        contents.entries.push_back(std::move(entry));
    }
    return contents;
}

Result<BomContents> BomParser::parseFile(const std::string& path, const BomParseOptions& options,
                                         const CancellationToken& token) {
    auto bytes = io::readBinaryFile(path, options.maxFileBytes);
    if (!bytes) return bytes.error();
    return parse(std::span<const std::uint8_t>(bytes.value()), options, token);
}

}  // namespace vortex
