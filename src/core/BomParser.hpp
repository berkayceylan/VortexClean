// VortexClean core — native parser for Apple's binary "BOMStore" (Bill of
// Materials) container, the format of the /var/db/receipts/*.bom files that
// Installer.app writes for every .pkg it installs.
//
// On-disk layout (every integer is big-endian):
//
//   Header (32 bytes)
//     char[8]  magic            "BOMStore"
//     u32      version          1
//     u32      numberOfBlocks   live blocks in the block table (informational)
//     u32      indexOffset      file offset of the block table
//     u32      indexLength      byte length of block table + free list
//     u32      varsOffset       file offset of the variable table
//     u32      varsLength       byte length of the variable table
//
//   Block table  @indexOffset
//     u32      count
//     { u32 address; u32 length; } [count]     entry 0 is always the null block
//     (a free list follows; it is not needed for reading)
//
//   Variable table  @varsOffset
//     u32      count
//     { u32 blockIndex; u8 nameLength; char name[nameLength]; } [count]
//     Well-known names: "BomInfo", "Paths", "HLIndex", "VIndex", "Size64".
//
//   Tree header  (the block named by a variable such as "Paths", 21 bytes)
//     char[4]  magic            "tree"
//     u32      version          1
//     u32      child            block index of the root page
//     u32      blockSize        page size (4096)
//     u32      pathCount        number of records in all leaves together
//     u8       unknown
//
//   Tree page
//     u16      isLeaf
//     u16      count
//     u32      forward          block index of the next leaf (0 = last)
//     u32      backward         block index of the previous leaf
//     leaf  : { u32 infoIndex;  u32 fileIndex; } [count]
//     branch: { u32 childIndex; u32 keyIndex;  } [count]  +  u32 lastChildIndex
//             (count keys, count + 1 children — Apple's writer always emits the
//              trailing child; the leftmost child is therefore the u32 at +12)
//
//   Path record, reached from a leaf entry
//     infoIndex -> BOMPathInfo1 { u32 id; u32 info2Index; }
//     info2Index-> BOMPathInfo2 { u8 type; u8; u16 arch; u16 mode; u32 uid;
//                                 u32 gid; u32 mtime; u32 size; u8;
//                                 u32 checksumOrDevType;            (31 bytes with the next field)
//                                 u32 linkNameLength;   -- symlinks only
//                                 char linkName[linkNameLength]; }
//                   For Mach-O files the bytes from offset 27 on hold a
//                   per-architecture table instead; they are not a link name.
//     fileIndex -> BOMFile      { u32 parentId; char name[]; /* NUL-terminated */ }
//
// A path is rebuilt by following parentId links up to a root record (parent 0,
// conventionally named "."), giving "./Applications/Foo.app/Contents/...".
//
// The parser never trusts a single length or index read from the file: every
// access is bounds-checked against the buffer, page chains are protected by a
// visited set (so cyclic `forward` or child links cannot loop), and parent
// chains are resolved iteratively with cycle detection.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "Cancellation.hpp"
#include "Result.hpp"

namespace vortex {

enum class BomEntryType : std::uint8_t { Unknown = 0, File = 1, Directory = 2, Symlink = 3, Device = 4 };

struct BomEntry {
    std::string path;  ///< relative, rooted at the BOM root: "." or "./usr/local/bin/tool"
    BomEntryType type = BomEntryType::Unknown;
    std::uint16_t mode = 0;
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::uint32_t modificationTime = 0;
    std::uint32_t size = 0;  ///< 32-bit size as stored in the record
    std::uint32_t checksum = 0;
    std::string linkTarget;  ///< symlink destination, when recorded
};

struct BomParseOptions {
    std::size_t maxEntries = 8'000'000;        ///< hard cap on records accepted from one file
    std::size_t maxPathLength = 8192;          ///< longest reconstructed path accepted
    std::size_t maxFileBytes = std::size_t{1} << 30;  ///< parseFile() refuses anything larger
};

struct BomContents {
    std::vector<BomEntry> entries;           ///< in B-tree (lexicographic key) order
    std::uint32_t declaredPathCount = 0;     ///< the tree header's own record count
    std::uint32_t skippedEntries = 0;        ///< records dropped: bad name, duplicate id, orphan, cycle, stray root
    std::vector<std::string> variableNames;  ///< names present in the variable table
};

class BomParser {
public:
    /// Parses an in-memory BOM image. Structural damage yields an Error; nothing
    /// is ever read outside `data`.
    [[nodiscard]] static Result<BomContents> parse(std::span<const std::uint8_t> data,
                                                   const BomParseOptions& options = {},
                                                   const CancellationToken& token = {});

    /// Reads `path` into memory (regular files only) and parses it.
    [[nodiscard]] static Result<BomContents> parseFile(const std::string& path,
                                                       const BomParseOptions& options = {},
                                                       const CancellationToken& token = {});
};

}  // namespace vortex
