#include "DiskAnalyzer.hpp"

#include <cerrno>
#include <cstddef>
#include <fts.h>
#include <functional>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_set>

namespace vortex {

namespace {

struct InodeKey {
    dev_t device;
    ino_t inode;
    bool operator==(const InodeKey& other) const noexcept {
        return device == other.device && inode == other.inode;
    }
};

struct InodeKeyHash {
    std::size_t operator()(const InodeKey& key) const noexcept {
        const std::size_t a = std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(key.inode));
        const std::size_t b = std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(key.device));
        return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
    }
};

std::uint64_t allocatedBytesOf(const struct stat& st) noexcept {
    // st_blocks is always expressed in 512-byte units, independent of the volume block size.
    return st.st_blocks > 0 ? static_cast<std::uint64_t>(st.st_blocks) * 512ULL : 0ULL;
}

void noteError(DiskUsage& usage, int error, const char* path) {
    ++usage.errorCount;
    if (error == EPERM || error == EACCES) {
        if (!usage.permissionDenied && path != nullptr) usage.firstDeniedPath = path;
        usage.permissionDenied = true;
    }
}

}  // namespace

DiskUsage& DiskUsage::operator+=(const DiskUsage& other) {
    logicalBytes += other.logicalBytes;
    allocatedBytes += other.allocatedBytes;
    fileCount += other.fileCount;
    directoryCount += other.directoryCount;
    symlinkCount += other.symlinkCount;
    hardLinkDuplicates += other.hardLinkDuplicates;
    errorCount += other.errorCount;
    if (other.permissionDenied && !permissionDenied) firstDeniedPath = other.firstDeniedPath;
    permissionDenied = permissionDenied || other.permissionDenied;
    cancelled = cancelled || other.cancelled;
    return *this;
}

DiskUsage DiskAnalyzer::measure(const std::string& path, const CancellationToken& token) {
    DiskUsage usage;
    if (path.empty()) {
        ++usage.errorCount;
        return usage;
    }

    std::string mutablePath = path;
    char* const roots[] = {mutablePath.data(), nullptr};
    // FTS_PHYSICAL: lstat semantics, symlinks are reported and not traversed.
    // FTS_NOCHDIR : fts must not chdir(); the working directory is process-wide
    //               state and this function runs on many threads at once.
    // FTS_XDEV    : never descend into another mounted volume.
    FTS* const fts = ::fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr);
    if (fts == nullptr) {
        noteError(usage, errno, path.c_str());
        return usage;
    }

    std::unordered_set<InodeKey, InodeKeyHash> linkedInodes;
    for (;;) {
        if (token.isCancelled()) {
            usage.cancelled = true;
            break;
        }
        errno = 0;
        FTSENT* const entry = ::fts_read(fts);
        if (entry == nullptr) {
            if (errno != 0) noteError(usage, errno, path.c_str());
            break;
        }
        switch (entry->fts_info) {
            case FTS_D:  // directory, pre-order visit
                ++usage.directoryCount;
                usage.allocatedBytes += allocatedBytesOf(*entry->fts_statp);
                break;
            case FTS_DP:  // directory, post-order visit: already counted
                break;
            case FTS_F: {
                const struct stat& st = *entry->fts_statp;
                if (st.st_nlink > 1 && !linkedInodes.insert(InodeKey{st.st_dev, st.st_ino}).second) {
                    ++usage.hardLinkDuplicates;
                    break;
                }
                ++usage.fileCount;
                if (st.st_size > 0) usage.logicalBytes += static_cast<std::uint64_t>(st.st_size);
                usage.allocatedBytes += allocatedBytesOf(st);
                break;
            }
            case FTS_SL:
            case FTS_SLNONE:
                ++usage.symlinkCount;
                usage.allocatedBytes += allocatedBytesOf(*entry->fts_statp);
                break;
            case FTS_DEFAULT:  // socket, FIFO, device node
                usage.allocatedBytes += allocatedBytesOf(*entry->fts_statp);
                break;
            case FTS_DNR:  // directory that cannot be read
            case FTS_ERR:
            case FTS_NS:   // stat failed
                noteError(usage, entry->fts_errno, entry->fts_path);
                break;
            case FTS_DC:   // cycle; cannot occur without following links, counted defensively
                ++usage.errorCount;
                break;
            default:
                break;
        }
    }
    ::fts_close(fts);
    return usage;
}

}  // namespace vortex
