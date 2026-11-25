// VortexClean core — deep remnant discovery.
//
// One scan = (1) package receipts whose BOM installed the app, (2) a parallel
// sweep of every user and system Library location, (3) policy annotation
// (SIP, root, shared), (4) parallel disk-usage measurement. All stages observe a
// CancellationToken and return within milliseconds once it is cancelled.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "Cancellation.hpp"
#include "MatchEngine.hpp"
#include "ReceiptStore.hpp"
#include "Remnant.hpp"
#include "Result.hpp"
#include "ThreadPool.hpp"

namespace vortex {

/// The directories a scan looks at. Injectable so the test-suite can point the
/// scanner at a synthetic home and system tree.
struct ScanRoots {
    std::string homeDir;
    std::string userLibrary;                ///< ~/Library
    std::string systemLibrary;              ///< /Library
    std::vector<std::string> receiptDirs;   ///< /private/var/db/receipts (and its /var alias)
    std::string legacyReceiptsDir;          ///< /Library/Receipts
    std::string userCacheDir;               ///< per-user cache dir under /private/var/folders
    std::string userTempDir;                ///< per-user temp dir under /private/var/folders
    std::string installRoot = "/";          ///< what receipt paths are relative to
    /// Ask LaunchServices who owns an identifier before attributing a dotted
    /// child of the target's identifier. On for the real system, off for
    /// synthetic trees, whose identifiers LaunchServices knows nothing about.
    bool consultLaunchServices = false;

    /// The real locations for the current user.
    [[nodiscard]] static ScanRoots systemDefault();

    /// A synthetic disk: `directory` plays the role of "/" and `directory/Users/demo`
    /// is the home folder. Used by `--root` to try the application, and to take its
    /// screenshots, without looking at the real system.
    [[nodiscard]] static Result<ScanRoots> sandbox(const std::string& directory);
    [[nodiscard]] bool isSandbox() const noexcept { return installRoot != "/"; }

    /// The path as it is shown to people: "~" stands for the home folder, and the
    /// directory of a sandbox is left out.
    [[nodiscard]] std::string displayPath(std::string_view absolutePath) const;
};

struct ScanOptions {
    bool computeSizes = true;
    /// Upper bound on individually listed package-payload items (a note is added when it is hit).
    std::size_t maxPackagePayloadItems = 20000;
};

struct ScanCallbacks {
    /// Called once, as soon as every remnant is known. Sizes may still be pending
    /// (usageKnown == false). Runs on the thread that called scan().
    std::function<void(const std::vector<Remnant>&)> onDiscovered;
    /// Called once per remnant when its disk usage has been measured. Runs on a
    /// worker thread; calls for different indices may overlap.
    std::function<void(std::size_t index, const Remnant& remnant)> onSized;
};

class RemnantScanner {
public:
    RemnantScanner(ScanRoots roots, ThreadPool& pool);

    /// Blocks until the scan is complete or cancelled. Safe to call from several
    /// threads at once; must not be called from one of the pool's own workers.
    [[nodiscard]] ScanResult scan(const AppInfo& target, const InstalledAppIndex& installed,
                                  const CancellationToken& token, const ScanOptions& options = {},
                                  const ScanCallbacks& callbacks = {}) const;

    [[nodiscard]] const ScanRoots& roots() const noexcept { return roots_; }

private:
    ScanRoots roots_;
    ThreadPool& pool_;
    ReceiptStore receipts_;
};

/// Indices of the paths that are not located inside another path of the same
/// list ("top-most" selection). Used wherever nested selections must not be
/// counted or deleted twice.
[[nodiscard]] std::vector<std::size_t> topMostPathIndices(const std::vector<std::string>& paths);

}  // namespace vortex
