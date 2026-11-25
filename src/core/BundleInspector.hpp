// VortexClean core — application discovery and bundle metadata extraction.
//
// Info.plist is parsed with CoreFoundation, code-signing metadata comes from
// Security.framework (SecStaticCode), and the last-used date from the Spotlight
// metadata C API. No AppKit, no child processes.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "Cancellation.hpp"
#include "DiskAnalyzer.hpp"
#include "Result.hpp"

namespace vortex {

struct SigningInfo {
    bool isSigned = false;
    bool isAdHoc = false;
    /// Satisfies the requirement "anchor apple": software signed by Apple itself
    /// (not merely Developer ID or Mac App Store distribution).
    bool isApplePlatform = false;
    std::string teamIdentifier;
    std::string signingIdentifier;
    std::string authority;                       ///< leaf certificate summary
    std::vector<std::string> authorityChain;     ///< leaf first, root last
    std::vector<std::string> applicationGroups;  ///< com.apple.security.application-groups entitlement

    /// One-line description for the UI and audit log.
    [[nodiscard]] std::string summary() const;
};

struct AppInfo {
    std::string bundlePath;         ///< canonical absolute path of the .app
    std::string bundleId;           ///< CFBundleIdentifier
    std::string displayName;        ///< what Finder shows: the bundle's file name without ".app"
    std::string bundleDisplayName;  ///< CFBundleDisplayName
    std::string bundleName;         ///< CFBundleName
    std::string executableName;     ///< CFBundleExecutable
    std::string executablePath;
    std::string shortVersion;       ///< CFBundleShortVersionString
    std::string bundleVersion;      ///< CFBundleVersion

    /// Bundle identifiers of helpers shipped inside the bundle (login items,
    /// XPC services, app extensions, nested helper apps).
    std::vector<std::string> embeddedBundleIds;
    /// launchd labels the app declares: SMPrivilegedExecutables keys, tools in
    /// Contents/Library/LaunchServices, and embedded LaunchAgents/LaunchDaemons.
    std::vector<std::string> helperLabels;

    SigningInfo signing;
    bool isAppleApp = false;       ///< com.apple.* identifier or an Apple platform signature
    bool isSipProtected = false;   ///< locked by System Integrity Protection; cannot be removed
    bool isAppStore = false;       ///< carries a Mac App Store receipt
    bool isWrappedIosApp = false;  ///< iOS/iPadOS app running on Apple silicon

    std::int64_t lastUsedEpoch = 0;  ///< seconds since 1970; 0 when unknown
    std::int64_t modifiedEpoch = 0;
    uid_t ownerUid = 0;

    DiskUsage usage;
    bool usageKnown = false;
};

class BundleInspector {
public:
    struct Options {
        bool readSigningInfo = true;
        bool readLastUsedDate = true;
        bool enumerateEmbeddedBundles = true;
    };

    /// Reads everything about one bundle except its disk usage.
    [[nodiscard]] static Result<AppInfo> inspect(const std::string& bundlePath, const Options& options);
    [[nodiscard]] static Result<AppInfo> inspect(const std::string& bundlePath) { return inspect(bundlePath, Options{}); }

    /// Finds *.app bundles below the given roots. Bundles are never descended
    /// into; plain folders (e.g. /Applications/Utilities) are, up to `maxDepth`.
    [[nodiscard]] static std::vector<std::string> discoverBundles(const std::vector<std::string>& roots,
                                                                  const CancellationToken& token = {},
                                                                  int maxDepth = 3);

    /// /Applications and ~/Applications. (`systemRoot`: see PathPolicy.)
    [[nodiscard]] static std::vector<std::string> defaultSearchRoots(const std::string& home,
                                                                     const std::string& systemRoot = "/");

    /// Places that hold application bundles which are never offered for removal
    /// but must be known to the cross-checks: the system's own applications and
    /// helper applications installed next to privileged tools.
    [[nodiscard]] static std::vector<std::string> auxiliarySearchRoots(const std::string& systemRoot = "/");

    /// Bundles in auxiliarySearchRoots(), inspected just deeply enough for the
    /// cross-checks (identifiers and embedded identifiers; no signatures, no sizes).
    [[nodiscard]] static std::vector<AppInfo> inspectAuxiliaryApplications(const CancellationToken& token = {},
                                                                           const std::string& systemRoot = "/");

    /// Asks LaunchServices who owns a bundle identifier. True when the identifier
    /// is registered to at least one application that exists on disk and none of
    /// its registrations lies inside `targetBundlePath` — i.e. it belongs to
    /// another application, wherever that one is installed.
    [[nodiscard]] static bool isRegisteredToAnotherApplication(std::string_view bundleId,
                                                               std::string_view targetBundlePath);

    /// Locates the Info.plist of any bundle flavour: app (Contents/), wrapped iOS
    /// app, flat bundle, or framework (Resources/, Versions/Current/Resources/).
    /// Empty when there is none.
    [[nodiscard]] static std::string infoPlistPath(const std::string& bundlePath);

    /// CFBundleIdentifier of any bundle, or "" when it has no readable Info.plist.
    [[nodiscard]] static std::string bundleIdentifier(const std::string& bundlePath);

    [[nodiscard]] static bool hasAppExtension(std::string_view path) noexcept;
};

}  // namespace vortex
