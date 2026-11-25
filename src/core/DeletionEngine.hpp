// VortexClean core — the safe deletion pipeline.
//
// Every request, real or simulated, runs the same sequence:
//
//   1. validate   PathPolicy: refuse invalid paths, shared roots and SIP items
//   2. coalesce   among the valid items, nested ones collapse onto their ancestor
//   3. measure    disk usage is taken before anything is touched
//   4. classify   split what the user can remove from what needs root
//   5. act        dry-run: report only | trash: recoverable | permanent: unlinkat
//   6. escalate   root-required items go to the elevation handler, if any
//   7. audit      a JSON report is always written
//
// The engine itself is pure C++/POSIX. Moving to the Trash (NSFileManager) and
// privilege elevation (Authorization Services) are injected as hooks by the
// Objective-C++ bridge, which keeps this layer free of Cocoa and fully testable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "BundleInspector.hpp"
#include "Cancellation.hpp"
#include "LaunchControl.hpp"
#include "PathPolicy.hpp"
#include "Remnant.hpp"
#include "Result.hpp"

namespace vortex {

enum class DeleteMode : std::uint8_t { Trash, Permanent };

enum class ItemStatus : std::uint8_t {
    Trashed,            ///< moved to the Trash; recoverable
    Deleted,            ///< unlinked permanently
    SkippedSip,         ///< protected by System Integrity Protection
    SkippedPermission,  ///< needs root that was not granted, or blocked by TCC
    DryRun,             ///< simulation: validated and measured, nothing touched
    Error,              ///< refused by policy or failed for another reason
    SkippedMissing,     ///< no longer exists
};

[[nodiscard]] const char* jsonKey(DeleteMode mode) noexcept;
[[nodiscard]] const char* jsonKey(ItemStatus status) noexcept;
[[nodiscard]] bool parseDeleteMode(std::string_view text, DeleteMode& out) noexcept;
[[nodiscard]] bool parseItemStatus(std::string_view text, ItemStatus& out) noexcept;
[[nodiscard]] bool parseCategory(std::string_view text, RemnantCategory& out) noexcept;

struct DeletionItem {
    std::string path;
    RemnantCategory category = RemnantCategory::ApplicationSupport;
    Confidence confidence = Confidence::Exact;
    MatchReason reason = MatchReason::ExactBundleId;
    std::string launchdLabel;

    [[nodiscard]] static DeletionItem fromRemnant(const Remnant& remnant);
};

struct ItemOutcome {
    DeletionItem item;
    ItemStatus status = ItemStatus::Error;
    std::uint64_t allocatedBytes = 0;  ///< measured before the operation
    std::uint64_t logicalBytes = 0;
    bool requiresRoot = false;
    bool elevated = false;             ///< handled by the privileged helper
    std::string trashPath;             ///< where the item now lives, for Trashed
    int errorCode = 0;                 ///< errno-style code; 0 when there is none
    std::string errorMessage;
    std::string note;
    std::string launchctlTarget;       ///< service target that was booted out, if any
    bool launchctlRan = false;
    int launchctlExit = -1;
};

struct DeletionRequest {
    AppInfo target;
    std::vector<DeletionItem> items;
    DeleteMode mode = DeleteMode::Trash;
    bool dryRun = false;
};

/// What the elevation handler returns for a batch of root-required items.
struct PrivilegedBatchResult {
    bool authorized = false;            ///< false: the user declined or elevation is unavailable
    std::string error;
    std::vector<ItemOutcome> outcomes;  ///< matched back to the request by path
};

struct DeletionHooks {
    /// Moves one item to the Trash and returns its new location. Required for DeleteMode::Trash.
    std::function<Result<std::string>(const std::string& path)> moveToTrash;
    /// Unloads a launchd service. Defaults to LaunchControl::bootout when unset.
    std::function<LaunchctlResult(const std::string& serviceTarget)> bootout;
    /// Removes root-required items with administrator privileges. Optional: when
    /// unset those items are reported as skipped_permission.
    std::function<PrivilegedBatchResult(const std::vector<DeletionItem>& items, DeleteMode mode)> elevate;
    /// Called after each item is finished. Runs on the thread that called execute().
    std::function<void(std::size_t completed, std::size_t total, const ItemOutcome& outcome)> onProgress;
};

struct DeletionReport {
    DeleteMode mode = DeleteMode::Trash;
    bool dryRun = false;
    std::vector<ItemOutcome> outcomes;  ///< one per requested item, in request order
    /// Allocated bytes of everything trashed or deleted (for a dry run: what would be).
    std::uint64_t reclaimedBytes = 0;
    bool privacyDenied = false;         ///< a TCC-style denial occurred: suggest Full Disk Access
    bool elevationAttempted = false;
    bool elevationAuthorized = false;
    std::string elevationError;
    bool cancelled = false;
    std::int64_t startedEpochMs = 0;
    std::int64_t finishedEpochMs = 0;
    std::string auditLogPath;           ///< the JSON report written for this run
    std::string auditLogError;          ///< why it could not be written ("" on success)

    [[nodiscard]] std::size_t countOf(ItemStatus status) const noexcept;
};

class DeletionEngine {
public:
    struct Config {
        std::string homeDirectory;
        std::string systemRoot = "/";   ///< see PathPolicy; "/" in production
        std::string auditDirectory;     ///< "" selects defaultAuditDirectory(homeDirectory)
    };

    DeletionEngine(Config config, DeletionHooks hooks);

    /// Runs the whole pipeline and always writes an audit log.
    [[nodiscard]] DeletionReport execute(const DeletionRequest& request, const CancellationToken& token = {}) const;

    /// Race-safe recursive removal: anchored on directory descriptors
    /// (openat/unlinkat), refuses any symlink in the path leading to the item,
    /// never follows a symlink inside it, and never crosses onto another volume.
    /// On failure `failedPath` (when given) receives the first entry that could
    /// not be removed.
    [[nodiscard]] static Status removeTree(const std::string& path, std::string* failedPath = nullptr);

    /// ~/Library/Logs/VortexClean
    [[nodiscard]] static std::string defaultAuditDirectory(const std::string& home);

    [[nodiscard]] const PathPolicy& policy() const noexcept { return policy_; }

private:
    Config config_;
    DeletionHooks hooks_;
    PathPolicy policy_;
};

}  // namespace vortex
