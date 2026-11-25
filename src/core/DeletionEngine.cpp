#include "DeletionEngine.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

#include "AuditLogger.hpp"
#include "DiskAnalyzer.hpp"
#include "FileUtil.hpp"
#include "PathUtil.hpp"
#include "RemnantScanner.hpp"
#include "StringUtil.hpp"

namespace vortex {

namespace {

constexpr int kMaxRemovalDepth = 4096;

std::int64_t nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
// Recursive removal
// ---------------------------------------------------------------------------

struct RemovalState {
    dev_t device = 0;
    uid_t user = 0;
    int firstError = 0;
    std::string firstFailedPath;

    void fail(int error, const std::string& where) {
        if (firstError == 0) {
            firstError = error;
            firstFailedPath = where;
        }
    }
};

/// Clears the user "locked" flag on a descriptor we own, so an explicit permanent
/// delete behaves like emptying the Trash does for locked items. System flags are
/// never touched.
void clearUserImmutable(int fd, const struct stat& st, uid_t user) {
    if ((st.st_flags & UF_IMMUTABLE) != 0 && st.st_uid == user) {
        ::fchflags(fd, st.st_flags & ~static_cast<unsigned>(UF_IMMUTABLE));
    }
}

void removeEntry(int dirFd, const std::string& name, const std::string& displayPath, RemovalState& state,
                 int depth) {
    struct stat st {};
    if (::fstatat(dirFd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT) state.fail(errno, displayPath);
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        // Files, symlinks (the link itself), sockets, FIFOs.
        if (::unlinkat(dirFd, name.c_str(), 0) == 0) return;
        int error = errno;
        if (error == EPERM && S_ISREG(st.st_mode) && (st.st_flags & UF_IMMUTABLE) != 0 && st.st_uid == state.user) {
            const int fd = ::openat(dirFd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                clearUserImmutable(fd, st, state.user);
                ::close(fd);
                if (::unlinkat(dirFd, name.c_str(), 0) == 0) return;
                error = errno;
            }
        }
        if (error != ENOENT) state.fail(error, displayPath);
        return;
    }

    if (depth == 0) {
        state.device = st.st_dev;
    } else if (st.st_dev != state.device) {
        state.fail(EXDEV, displayPath);  // a mounted volume inside the tree: leave it alone
        return;
    }
    if (depth > kMaxRemovalDepth) {
        state.fail(ENAMETOOLONG, displayPath);
        return;
    }

    const int fd = ::openat(dirFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        state.fail(errno, displayPath);
        return;
    }
    // The directory that was opened must be the very one that was examined:
    // otherwise somebody swapped the entry between the two calls.
    struct stat opened {};
    if (::fstat(fd, &opened) != 0 || opened.st_dev != st.st_dev || opened.st_ino != st.st_ino) {
        ::close(fd);
        state.fail(EAGAIN, displayPath);
        return;
    }
    clearUserImmutable(fd, opened, state.user);

    DIR* const dir = ::fdopendir(fd);  // takes ownership of fd
    if (dir == nullptr) {
        const int error = errno;
        ::close(fd);
        state.fail(error, displayPath);
        return;
    }
    // Collect the names first: removing entries while iterating a directory is unspecified.
    std::vector<std::string> names;
    for (;;) {
        errno = 0;
        const struct dirent* entry = ::readdir(dir);
        if (entry == nullptr) {
            if (errno != 0) state.fail(errno, displayPath);
            break;
        }
        const std::string_view entryName(entry->d_name);
        if (entryName == "." || entryName == "..") continue;
        names.emplace_back(entryName);
    }
    for (const std::string& child : names) {
        removeEntry(::dirfd(dir), child, displayPath + "/" + child, state, depth + 1);
    }
    ::closedir(dir);

    if (::unlinkat(dirFd, name.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT) {
        state.fail(errno, displayPath);
    }
}

// ---------------------------------------------------------------------------
// Outcome helpers
// ---------------------------------------------------------------------------

void appendNote(ItemOutcome& outcome, std::string_view text) {
    if (text.empty()) return;
    if (!outcome.note.empty()) outcome.note += "; ";
    outcome.note.append(text);
}

void setFailure(ItemOutcome& outcome, ItemStatus status, int code, std::string message) {
    outcome.status = status;
    outcome.errorCode = code;
    outcome.errorMessage = std::move(message);
}

bool isRemovedStatus(ItemStatus status) noexcept {
    return status == ItemStatus::Trashed || status == ItemStatus::Deleted || status == ItemStatus::DryRun;
}

constexpr std::array<RemnantCategory, kRemnantCategoryCount> kAllCategories{
    RemnantCategory::AppBundle,       RemnantCategory::ApplicationSupport,  RemnantCategory::Caches,
    RemnantCategory::Preferences,     RemnantCategory::Containers,          RemnantCategory::GroupContainers,
    RemnantCategory::SavedState,      RemnantCategory::Logs,                RemnantCategory::WebKit,
    RemnantCategory::HTTPStorages,    RemnantCategory::Cookies,             RemnantCategory::ApplicationScripts,
    RemnantCategory::LaunchAgents,    RemnantCategory::LaunchDaemons,       RemnantCategory::PrivilegedHelperTools,
    RemnantCategory::Receipts,        RemnantCategory::PackagePayload,      RemnantCategory::QuickLook,
    RemnantCategory::PreferencePanes, RemnantCategory::Frameworks,
};

}  // namespace

// ---------------------------------------------------------------------------
// Enum <-> text
// ---------------------------------------------------------------------------

const char* jsonKey(DeleteMode mode) noexcept {
    return mode == DeleteMode::Trash ? "trash" : "permanent";
}

const char* jsonKey(ItemStatus status) noexcept {
    switch (status) {
        case ItemStatus::Trashed: return "trashed";
        case ItemStatus::Deleted: return "deleted";
        case ItemStatus::SkippedSip: return "skipped_sip";
        case ItemStatus::SkippedPermission: return "skipped_permission";
        case ItemStatus::DryRun: return "dry_run";
        case ItemStatus::Error: return "error";
        case ItemStatus::SkippedMissing: return "skipped_missing";
    }
    return "error";
}

bool parseDeleteMode(std::string_view text, DeleteMode& out) noexcept {
    for (const DeleteMode mode : {DeleteMode::Trash, DeleteMode::Permanent}) {
        if (text == jsonKey(mode)) {
            out = mode;
            return true;
        }
    }
    return false;
}

bool parseItemStatus(std::string_view text, ItemStatus& out) noexcept {
    for (const ItemStatus status :
         {ItemStatus::Trashed, ItemStatus::Deleted, ItemStatus::SkippedSip, ItemStatus::SkippedPermission,
          ItemStatus::DryRun, ItemStatus::Error, ItemStatus::SkippedMissing}) {
        if (text == jsonKey(status)) {
            out = status;
            return true;
        }
    }
    return false;
}

bool parseCategory(std::string_view text, RemnantCategory& out) noexcept {
    for (const RemnantCategory category : kAllCategories) {
        if (text == jsonKey(category)) {
            out = category;
            return true;
        }
    }
    return false;
}

DeletionItem DeletionItem::fromRemnant(const Remnant& remnant) {
    DeletionItem item;
    item.path = remnant.path;
    item.category = remnant.category;
    item.confidence = remnant.confidence;
    item.reason = remnant.reason;
    item.launchdLabel = remnant.launchdLabel;
    return item;
}

std::size_t DeletionReport::countOf(ItemStatus status) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        outcomes.begin(), outcomes.end(), [status](const ItemOutcome& o) { return o.status == status; }));
}

// ---------------------------------------------------------------------------
// DeletionEngine
// ---------------------------------------------------------------------------

std::string DeletionEngine::defaultAuditDirectory(const std::string& home) {
    return path::join(path::join(home, "Library/Logs"), "VortexClean");
}

DeletionEngine::DeletionEngine(Config config, DeletionHooks hooks)
    : config_(std::move(config)), hooks_(std::move(hooks)), policy_(config_.homeDirectory, config_.systemRoot) {
    if (config_.auditDirectory.empty()) config_.auditDirectory = defaultAuditDirectory(config_.homeDirectory);
}

Status DeletionEngine::removeTree(const std::string& target, std::string* failedPath) {
    const std::string normalized = path::normalize(target);
    if (normalized.empty() || normalized.front() != '/' || normalized == "/") {
        if (failedPath != nullptr) *failedPath = target;
        return failure(EINVAL, "refusing to remove '" + target + "': not a removable absolute path");
    }
    const std::string parent = path::parent(normalized);
    const std::string name(path::fileName(normalized));

    // O_NOFOLLOW_ANY: fail instead of walking through a symlink anywhere in the
    // parent path, so the directory being modified is exactly the one named.
    const int parentFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW_ANY);
    if (parentFd < 0) {
        const int error = errno;
        if (failedPath != nullptr) *failedPath = parent;
        const std::string reason =
            error == ELOOP ? std::string("the path contains a symbolic link") : io::errnoMessage(error);
        return failure(error, "cannot open '" + parent + "': " + reason);
    }

    RemovalState state;
    state.user = ::geteuid();
    removeEntry(parentFd, name, normalized, state, 0);
    ::close(parentFd);

    if (state.firstError == 0) return success();
    if (failedPath != nullptr) *failedPath = state.firstFailedPath;
    return failure(state.firstError,
                   "cannot remove '" + state.firstFailedPath + "': " + io::errnoMessage(state.firstError));
}

DeletionReport DeletionEngine::execute(const DeletionRequest& request, const CancellationToken& token) const {
    DeletionReport report;
    report.mode = request.mode;
    report.dryRun = request.dryRun;
    report.startedEpochMs = nowEpochMs();

    const std::size_t total = request.items.size();
    report.outcomes.resize(total);
    std::vector<std::string> paths;
    paths.reserve(total);
    for (std::size_t i = 0; i < total; ++i) {
        report.outcomes[i].item = request.items[i];
        paths.push_back(request.items[i].path);
    }

    std::size_t completed = 0;
    const auto finish = [&](std::size_t index) {
        ++completed;
        if (hooks_.onProgress) hooks_.onProgress(completed, total, report.outcomes[index]);
    };
    const auto cancelItem = [&](std::size_t index) {
        report.cancelled = true;
        setFailure(report.outcomes[index], ItemStatus::Error, ECANCELED,
                   "cancelled before this item was processed");
        finish(index);
    };

    // ---- 1-2. Validate every item on its own merits: policy first, then existence.
    std::vector<std::size_t> valid;
    for (std::size_t index = 0; index < total; ++index) {
        ItemOutcome& outcome = report.outcomes[index];
        const std::string& itemPath = outcome.item.path;
        if (token.isCancelled()) {
            cancelItem(index);
            continue;
        }

        const Protection protection = policy_.classify(itemPath);
        if (protection == Protection::Invalid) {
            setFailure(outcome, ItemStatus::Error, EINVAL, "refused: not an absolute, normalised path");
            finish(index);
            continue;
        }
        if (protection == Protection::ProtectedRoot) {
            setFailure(outcome, ItemStatus::Error, EPERM,
                       "refused: this is a shared system or user directory that must never be removed");
            finish(index);
            continue;
        }
        if (protection == Protection::SipProtected) {
            setFailure(outcome, ItemStatus::SkippedSip, EPERM, "protected by System Integrity Protection");
            finish(index);
            continue;
        }

        struct stat st {};
        if (::lstat(itemPath.c_str(), &st) != 0) {
            const int error = errno;
            if (error == ENOENT || error == ENOTDIR) {
                setFailure(outcome, ItemStatus::SkippedMissing, error, "the item no longer exists");
            } else if (error == EACCES || error == EPERM) {
                setFailure(outcome, ItemStatus::SkippedPermission, error,
                           "cannot examine the item: " + io::errnoMessage(error));
                if (path::isWithinOrEqual(itemPath, policy_.home())) report.privacyDenied = true;
            } else {
                setFailure(outcome, ItemStatus::Error, error, "cannot examine the item: " + io::errnoMessage(error));
            }
            finish(index);
            continue;
        }
        valid.push_back(index);
    }

    // ---- 3. Coalesce: among the valid items, one that lies inside another is
    // handled by that ancestor. Refused items never swallow their descendants.
    std::vector<std::string> validPaths;
    validPaths.reserve(valid.size());
    for (const std::size_t index : valid) validPaths.push_back(paths[index]);
    std::vector<std::size_t> pending;  // the top-most valid items: these are acted upon
    std::vector<std::size_t> nested;   // valid items covered by one of the pending ones
    {
        std::vector<bool> isTopMost(valid.size(), false);
        for (const std::size_t position : topMostPathIndices(validPaths)) isTopMost[position] = true;
        for (std::size_t position = 0; position < valid.size(); ++position) {
            (isTopMost[position] ? pending : nested).push_back(valid[position]);
        }
    }

    // ---- 4. Measure and classify what is about to be acted upon.
    for (const std::size_t index : pending) {
        ItemOutcome& outcome = report.outcomes[index];
        const DiskUsage usage = DiskAnalyzer::measure(outcome.item.path, token);
        outcome.allocatedBytes = usage.allocatedBytes;
        outcome.logicalBytes = usage.logicalBytes;
        outcome.requiresRoot = policy_.requiresRoot(outcome.item.path);
    }

    const auto describeService = [&](ItemOutcome& outcome, bool asRoot) -> std::string {
        const RemnantCategory category = outcome.item.category;
        const bool isAgent = category == RemnantCategory::LaunchAgents;
        const bool isDaemon = category == RemnantCategory::LaunchDaemons;
        if ((!isAgent && !isDaemon) || outcome.item.launchdLabel.empty()) return {};
        if (!LaunchControl::isValidLabel(outcome.item.launchdLabel)) {
            appendNote(outcome, "launchd label has unusual characters; the service was not booted out");
            return {};
        }
        if (isDaemon) {
            if (!asRoot) return {};  // the system domain can only be changed by root
            return LaunchControl::systemServiceTarget(outcome.item.launchdLabel);
        }
        return LaunchControl::userServiceTarget(::getuid(), outcome.item.launchdLabel);
    };

    if (request.dryRun) {
        // ---- 5a. Simulation: everything that passed validation would be removed.
        for (const std::size_t index : pending) {
            ItemOutcome& outcome = report.outcomes[index];
            outcome.status = ItemStatus::DryRun;
            if (outcome.requiresRoot) appendNote(outcome, "would require administrator authorization");
            const std::string target = describeService(outcome, outcome.requiresRoot);
            if (!target.empty()) {
                outcome.launchctlTarget = target;
                appendNote(outcome, "would run launchctl bootout " + target);
            }
            finish(index);
        }
    } else {
        // ---- 5b. What the current user can remove.
        std::vector<std::size_t> rootRequired;
        for (const std::size_t index : pending) {
            ItemOutcome& outcome = report.outcomes[index];
            const std::string& itemPath = outcome.item.path;
            if (token.isCancelled()) {
                cancelItem(index);
                continue;
            }
            if (outcome.requiresRoot) {
                rootRequired.push_back(index);
                continue;
            }

            // Unload the user's launchd agent first so launchd does not keep the job alive.
            if (const std::string target = describeService(outcome, false); !target.empty()) {
                const LaunchctlResult booted = hooks_.bootout ? hooks_.bootout(target) : LaunchControl::bootout(target);
                outcome.launchctlTarget = target;
                outcome.launchctlRan = booted.spawned;
                outcome.launchctlExit = booted.exitStatus;
                appendNote(outcome, booted.detail);
            }

            int error = 0;
            std::string message;
            std::string failedPath = itemPath;
            if (request.mode == DeleteMode::Trash) {
                if (!hooks_.moveToTrash) {
                    setFailure(outcome, ItemStatus::Error, ENOTSUP, "no Trash provider is available");
                    finish(index);
                    continue;
                }
                auto moved = hooks_.moveToTrash(itemPath);
                if (moved) {
                    outcome.status = ItemStatus::Trashed;
                    outcome.trashPath = moved.value();
                    finish(index);
                    continue;
                }
                error = moved.error().code;
                message = moved.error().message;
            } else {
                const Status removed = removeTree(itemPath, &failedPath);
                if (removed) {
                    outcome.status = ItemStatus::Deleted;
                    finish(index);
                    continue;
                }
                error = removed.error().code;
                message = removed.error().message;
                // Part of the tree may be gone: report what was actually freed.
                const DiskUsage remaining = DiskAnalyzer::measure(itemPath, token);
                if (remaining.allocatedBytes < outcome.allocatedBytes) {
                    appendNote(outcome, "partially removed");
                    outcome.allocatedBytes -= remaining.allocatedBytes;
                    outcome.logicalBytes -= std::min(outcome.logicalBytes, remaining.logicalBytes);
                } else {
                    outcome.allocatedBytes = 0;
                    outcome.logicalBytes = 0;
                }
            }

            if (error == ENOENT) {
                setFailure(outcome, ItemStatus::SkippedMissing, error, "the item no longer exists");
            } else if (error == EACCES || error == EPERM || error == EROFS) {
                // Plain UNIX permissions that root could override -> escalate.
                // Anything else that looks like a permission problem is TCC or a
                // file flag, which root cannot override either.
                if (error != EPERM && policy_.requiresRoot(failedPath)) {
                    outcome.requiresRoot = true;
                    rootRequired.push_back(index);
                    continue;
                }
                setFailure(outcome, ItemStatus::SkippedPermission, error, std::move(message));
                if (path::isWithinOrEqual(itemPath, policy_.home())) {
                    report.privacyDenied = true;
                    appendNote(outcome, "macOS privacy protection (Full Disk Access) may be required");
                }
            } else {
                setFailure(outcome, ItemStatus::Error, error, std::move(message));
            }
            finish(index);
        }

        // ---- 6. Items that need root.
        if (!rootRequired.empty()) {
            if (token.isCancelled()) {
                for (const std::size_t index : rootRequired) cancelItem(index);
            } else if (!hooks_.elevate) {
                for (const std::size_t index : rootRequired) {
                    setFailure(report.outcomes[index], ItemStatus::SkippedPermission, EACCES,
                               "requires administrator privileges");
                    finish(index);
                }
            } else {
                report.elevationAttempted = true;
                std::vector<DeletionItem> items;
                items.reserve(rootRequired.size());
                for (const std::size_t index : rootRequired) items.push_back(report.outcomes[index].item);

                PrivilegedBatchResult batch = hooks_.elevate(items, request.mode);
                report.elevationAuthorized = batch.authorized;
                report.elevationError = batch.error;

                std::unordered_map<std::string, const ItemOutcome*> byPath;
                for (const ItemOutcome& elevated : batch.outcomes) byPath.emplace(elevated.item.path, &elevated);

                for (const std::size_t index : rootRequired) {
                    ItemOutcome& outcome = report.outcomes[index];
                    if (!batch.authorized) {
                        setFailure(outcome, ItemStatus::SkippedPermission, EACCES,
                                   batch.error.empty() ? std::string("administrator authorization was not granted")
                                                       : batch.error);
                        finish(index);
                        continue;
                    }
                    const auto found = byPath.find(outcome.item.path);
                    if (found == byPath.end()) {
                        setFailure(outcome, ItemStatus::Error, EIO, "the privileged helper returned no result");
                        finish(index);
                        continue;
                    }
                    const ItemOutcome& elevated = *found->second;
                    outcome.status = elevated.status;
                    outcome.elevated = true;
                    outcome.trashPath = elevated.trashPath;
                    outcome.errorCode = elevated.errorCode;
                    outcome.errorMessage = elevated.errorMessage;
                    outcome.launchctlTarget = elevated.launchctlTarget;
                    outcome.launchctlRan = elevated.launchctlRan;
                    outcome.launchctlExit = elevated.launchctlExit;
                    appendNote(outcome, elevated.note);
                    if (!isRemovedStatus(outcome.status)) {
                        outcome.allocatedBytes = 0;
                        outcome.logicalBytes = 0;
                    }
                    finish(index);
                }
            }
        }
    }

    // ---- Nested selections mirror the fate of the item that contains them.
    for (const std::size_t index : nested) {
        ItemOutcome& outcome = report.outcomes[index];
        const std::string normalized = path::normalize(outcome.item.path);
        const ItemOutcome* owner = nullptr;
        for (const std::size_t candidate : pending) {
            if (path::isWithinOrEqual(normalized, path::normalize(paths[candidate]))) {
                owner = &report.outcomes[candidate];
                break;
            }
        }
        if (owner == nullptr) {
            setFailure(outcome, ItemStatus::Error, EINVAL, "could not be related to any processed item");
        } else {
            outcome.status = owner->status;
            outcome.errorCode = owner->errorCode;
            outcome.errorMessage = owner->errorMessage;
            outcome.requiresRoot = owner->requiresRoot;
            outcome.elevated = owner->elevated;
            // Sizes stay at zero: the bytes are accounted for by the containing item.
            appendNote(outcome, "handled together with " + owner->item.path);
        }
        finish(index);
    }

    for (const ItemOutcome& outcome : report.outcomes) {
        if (isRemovedStatus(outcome.status)) report.reclaimedBytes += outcome.allocatedBytes;
    }
    report.cancelled = report.cancelled || token.isCancelled();
    report.finishedEpochMs = nowEpochMs();

    // ---- 7. Audit log: written for every run, simulated or real.
    auto written = AuditLogger::write(request, report, config_.auditDirectory);
    if (written) {
        report.auditLogPath = written.value();
    } else {
        report.auditLogError = written.error().message;
    }
    return report;
}

}  // namespace vortex
