// VortexClean core — value types describing what a scan found.
#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

#include "BundleInspector.hpp"
#include "DiskAnalyzer.hpp"

namespace vortex {

enum class RemnantCategory : std::uint8_t {
    AppBundle,
    ApplicationSupport,
    Caches,
    Preferences,
    Containers,
    GroupContainers,
    SavedState,
    Logs,
    WebKit,
    HTTPStorages,
    Cookies,
    ApplicationScripts,
    LaunchAgents,
    LaunchDaemons,
    PrivilegedHelperTools,
    Receipts,
    PackagePayload,
    QuickLook,
    PreferencePanes,
    Frameworks,
};
inline constexpr std::size_t kRemnantCategoryCount = 20;

/// How sure the engine is that an item belongs to the target application.
enum class Confidence : std::uint8_t {
    Exact,   ///< identified by an exact identifier or a verified package receipt
    High,    ///< derived from the identifier (child identifier, team prefix, declared helper)
    Medium,  ///< name based heuristic that survived every guardrail
};

/// Why an item was attributed to the target application.
enum class MatchReason : std::uint8_t {
    TargetBundle,
    ExactBundleId,
    BundleIdPrefix,
    EmbeddedHelperId,
    HelperLabel,
    EntitledAppGroup,
    TeamIdGroupContainer,
    ContainerMetadata,
    LaunchdProgramInBundle,
    LaunchdAssociatedBundleId,
    LaunchdProgramFromReceipt,
    LaunchdJobProgram,
    BomReceipt,
    ReceiptPackageId,
    AppNameMatch,
    ExecutableNameHeuristic,
    BundleIdNameHeuristic,
    VendorDirectory,
};

enum class Domain : std::uint8_t { User, System };

[[nodiscard]] const char* displayName(RemnantCategory category) noexcept;  ///< "Application Support"
[[nodiscard]] const char* jsonKey(RemnantCategory category) noexcept;      ///< "application_support"
[[nodiscard]] const char* displayName(Confidence confidence) noexcept;     ///< "Exact"
[[nodiscard]] const char* jsonKey(Confidence confidence) noexcept;         ///< "exact"
[[nodiscard]] const char* displayName(MatchReason reason) noexcept;        ///< "BOM Receipt"
[[nodiscard]] const char* jsonKey(MatchReason reason) noexcept;            ///< "bom_receipt"
[[nodiscard]] const char* jsonKey(Domain domain) noexcept;

struct Remnant {
    std::string path;
    RemnantCategory category = RemnantCategory::ApplicationSupport;
    Confidence confidence = Confidence::Medium;
    MatchReason reason = MatchReason::ExactBundleId;
    Domain domain = Domain::User;

    bool isDirectory = false;
    bool isSymlink = false;
    uid_t ownerUid = 0;

    /// Also used by other installed software; never selected by default.
    bool isShared = false;
    std::vector<std::string> sharedWith;
    /// Human readable caution shown as a badge / tooltip ("" when there is none).
    std::string warning;

    bool sipProtected = false;      ///< locked: the engine refuses to delete it
    bool requiresRoot = false;      ///< needs administrator privileges to remove
    bool permissionDenied = false;  ///< could not be fully read (TCC / permissions)
    bool defaultSelected = true;

    DiskUsage usage;
    bool usageKnown = false;

    std::string launchdLabel;      ///< set for LaunchAgents / LaunchDaemons property lists
    std::string receiptPackageId;  ///< set for items that come from a package receipt
};

struct ScanResult {
    AppInfo app;
    std::vector<Remnant> remnants;  ///< the application bundle is always the first element
    std::vector<std::string> matchedPackageIds;
    /// True when a TCC-protected location could not be read: the UI offers Full Disk Access.
    bool permissionDenied = false;
    std::vector<std::string> deniedPaths;
    std::vector<std::string> notes;
    bool cancelled = false;
    double elapsedSeconds = 0.0;
};

}  // namespace vortex
