// VortexClean core — disk footprint measurement.
//
// Walks a tree with fts(3) in physical mode (symlinks are recorded, never
// followed, so link loops and links that point out of the tree are harmless),
// stays on the starting device, and counts every inode once even when it has
// several hard-linked names.
#pragma once

#include <cstdint>
#include <string>

#include "Cancellation.hpp"

namespace vortex {

struct DiskUsage {
    /// Sum of st_size over regular files: what the data would occupy uncompressed and fully allocated.
    std::uint64_t logicalBytes = 0;
    /// Sum of st_blocks * 512 over every inode: what is actually allocated on disk.
    /// Smaller than logicalBytes for sparse and transparently-compressed files.
    /// (APFS clones report their full extent here, exactly like du(1).)
    std::uint64_t allocatedBytes = 0;
    std::uint64_t fileCount = 0;
    std::uint64_t directoryCount = 0;
    std::uint64_t symlinkCount = 0;
    /// Extra names of inodes that were already counted (hard links).
    std::uint64_t hardLinkDuplicates = 0;
    std::uint64_t errorCount = 0;
    /// True when some part of the tree could not be read because of EPERM / EACCES
    /// (typically TCC: the app lacks Full Disk Access).
    bool permissionDenied = false;
    bool cancelled = false;
    std::string firstDeniedPath;

    DiskUsage& operator+=(const DiskUsage& other);
};

class DiskAnalyzer {
public:
    /// Measures `path`, which may be a file, a symlink or a directory tree.
    /// A missing path yields an all-zero result with errorCount == 1.
    [[nodiscard]] static DiskUsage measure(const std::string& path, const CancellationToken& token = {});
};

}  // namespace vortex
