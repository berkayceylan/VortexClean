// VortexClean core — installer package receipts (/private/var/db/receipts).
//
// Every installed .pkg leaves "<package-id>.plist" (metadata, including the
// InstallPrefixPath its payload was installed under) and "<package-id>.bom"
// (the bill of materials). This class reads the metadata with CoreFoundation
// and summarises each BOM with the native BomParser; lsbom/pkgutil are never run.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Cancellation.hpp"

namespace vortex {

struct Receipt {
    std::string packageId;
    std::string plistPath;
    std::string bomPath;  ///< "" when the receipt has no .bom next to it
    std::string installPrefix;
    std::string version;
    std::string installProcess;
    std::string packageFileName;
    std::int64_t installEpoch = 0;
};

/// A compact summary of one receipt's BOM, cheap enough to keep for every
/// receipt on the machine.
struct ReceiptDigest {
    /// The BOM parsed without a single rejected record. Receipts that are not
    /// valid are never used as evidence.
    bool valid = false;
    std::size_t entryCount = 0;
    /// Absolute location of the BOM root (".") after applying the install prefix.
    std::string rootPath;
    /// Absolute paths of the outermost *.app directories the package installs.
    std::vector<std::string> appBundles;
    /// Fingerprints of every non-directory entry outside those app bundles.
    /// Lets the scanner notice that two packages installed the same file.
    std::unordered_set<std::uint64_t> payload;
};

class ReceiptStore {
public:
    /// `installRoot` is the volume root that receipt paths are relative to — "/"
    /// in production, a scratch directory in tests.
    explicit ReceiptStore(std::string installRoot = "/");

    /// Reads the receipt property lists in the given directories. Directories
    /// that resolve to the same location (/var/db/receipts and
    /// /private/var/db/receipts) are only read once.
    [[nodiscard]] std::vector<Receipt> load(const std::vector<std::string>& directories,
                                            const CancellationToken& token) const;

    /// The digest of a receipt's BOM. Results are cached for the lifetime of the
    /// store and recomputed when the .bom file changes. Never null.
    [[nodiscard]] std::shared_ptr<const ReceiptDigest> digest(const Receipt& receipt,
                                                              const CancellationToken& token) const;

    /// Maps a BOM-relative path ("./usr/local/bin/tool") to an absolute path under
    /// the install root. Returns "" for anything that would escape the root.
    [[nodiscard]] std::string resolve(std::string_view installPrefix, std::string_view bomRelativePath) const;

    /// Case-insensitive fingerprint of an absolute path.
    [[nodiscard]] static std::uint64_t fingerprint(std::string_view absolutePath);

    [[nodiscard]] const std::string& installRoot() const noexcept { return installRoot_; }

private:
    struct CacheSlot {
        std::int64_t modifiedSeconds = 0;
        std::int64_t modifiedNanoseconds = 0;
        std::int64_t size = 0;
        std::shared_ptr<const ReceiptDigest> digest;
    };

    std::string installRoot_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, CacheSlot> cache_;
};

}  // namespace vortex
