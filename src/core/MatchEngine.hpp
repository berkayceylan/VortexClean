// VortexClean core — attribution rules: does a file-system entry belong to the
// target application, and how sure are we?
//
// This class is pure logic (no disk access), which is what lets the test-suite
// prove the anti-false-positive guarantees directly:
//
//   1. Identifiers beat names. An entry is matched by bundle identifier (exact,
//      or as a dotted child "com.vendor.app.*") before any name rule is tried.
//   2. Longest owner wins. "com.vendor.app.pro.plist" belongs to an installed
//      app whose identifier is "com.vendor.app.pro", never to "com.vendor.app".
//   3. Names must be distinctive. Fuzzy name matching is refused for stop-words
//      ("Helper", "Update", "Agent", ...), for anything shorter than four
//      characters, and it is always whole-name equality, never a substring.
//   4. Apple is off limits. Nothing named com.apple.* is ever attributed by a
//      heuristic.
//   5. Shared means unchecked. Anything that another installed app could own —
//      same team, same vendor folder, same name, same embedded helper — is
//      reported as shared together with the names of those apps.
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "BundleInspector.hpp"
#include "Remnant.hpp"

namespace vortex {

/// Identity of an installed application, reduced to what cross-checks need.
struct InstalledApp {
    std::string bundlePath;
    std::string displayName;
    std::string bundleIdLower;
    std::string teamId;
    std::string vendorToken;
    std::vector<std::string> nameKeys;          ///< normalised distinctive names
    std::vector<std::string> embeddedIdsLower;
    std::vector<std::string> appGroupsLower;
};

/// Everything installed on the machine, used to recognise siblings of the target.
class InstalledAppIndex {
public:
    InstalledAppIndex() = default;
    explicit InstalledAppIndex(const std::vector<AppInfo>& apps);

    void add(const AppInfo& app);
    [[nodiscard]] const std::vector<InstalledApp>& apps() const noexcept { return apps_; }

private:
    std::vector<InstalledApp> apps_;
};

struct MatchVerdict {
    bool matched = false;
    Confidence confidence = Confidence::Medium;
    MatchReason reason = MatchReason::ExactBundleId;
    bool shared = false;
    std::vector<std::string> sharedWith;  ///< display names of the other apps involved

    explicit operator bool() const noexcept { return matched; }
};

class MatchEngine {
public:
    /// Answers whether an identifier is registered to an application that lives
    /// outside the target's bundle (LaunchServices in production). It extends
    /// the "longest owner wins" rule to applications that are installed
    /// somewhere discovery does not look, e.g. inside a tool's cache folder.
    using ForeignOwnerLookup = std::function<bool(std::string_view lowerIdentifier)>;

    /// `installed` must outlive the engine. The target itself may be part of the
    /// index; it is recognised by its bundle path and ignored.
    MatchEngine(const AppInfo& target, const InstalledAppIndex& installed, ForeignOwnerLookup foreignOwner = {});

    // ---- vocabulary rules (static, directly unit-tested) -------------------

    /// Generic words that say nothing about which application owns a file.
    [[nodiscard]] static bool isStopWord(std::string_view lowerToken) noexcept;
    /// True when a human-facing name is specific enough for name matching: at
    /// least four significant characters and one token that is neither short nor
    /// a stop-word.
    [[nodiscard]] static bool isDistinctiveName(std::string_view rawName);
    /// The vendor component of a reverse-DNS identifier ("com.google.Chrome" ->
    /// "google"), or "" when there is none or it is too generic to trust.
    [[nodiscard]] static std::string vendorToken(std::string_view bundleId);
    /// com.apple.* in any of its spellings (plain, "group." prefixed, team prefixed).
    [[nodiscard]] static bool isAppleIdentifier(std::string_view identifier) noexcept;
    /// The names worth testing for one directory entry: the name itself, the name
    /// minus known extensions (.plist, .savedState, ...), and each of those minus a
    /// trailing ByHost host identifier.
    [[nodiscard]] static std::vector<std::string> candidateStems(std::string_view entryName);

    // ---- attribution rules -------------------------------------------------

    /// A bundle-identifier-like string: exact id, dotted child, declared helper.
    [[nodiscard]] MatchVerdict matchIdentifier(std::string_view identifier) const;
    /// A Group Containers / Application Scripts style name: entitled group,
    /// "group.<id>", "<TEAMID>.<id>" or a bare "<TEAMID>.*".
    [[nodiscard]] MatchVerdict matchGroupIdentifier(std::string_view name) const;
    /// Whole-name equality against the app's distinctive names.
    [[nodiscard]] MatchVerdict matchName(std::string_view rawName) const;
    /// Like matchName, but inside the vendor's own folder, where the app name
    /// without its vendor prefix ("Chrome" for "Google Chrome") is also accepted.
    [[nodiscard]] MatchVerdict matchVendorChild(std::string_view rawName) const;
    /// Identifier rules over every candidate stem, optionally followed by name rules.
    [[nodiscard]] MatchVerdict matchEntry(std::string_view entryName, bool allowNameRules) const;
    /// "<Executable>-2026-01-02-030405.ips" style crash and hang reports.
    [[nodiscard]] MatchVerdict matchDiagnosticReport(std::string_view fileName) const;
    /// "<Name>_<UUID>.plist" records kept by CrashReporter.
    [[nodiscard]] MatchVerdict matchCrashReporterRecord(std::string_view fileName) const;

    /// True when `entryName` is this app's vendor folder ("Google", "zoom.us").
    [[nodiscard]] bool isVendorDirectoryName(std::string_view entryName) const;
    /// Display names of other installed apps by the same vendor or signing team.
    [[nodiscard]] std::vector<std::string> vendorSiblings() const;
    /// Other installed copies of the very same application (same bundle id, different path).
    [[nodiscard]] const std::vector<std::string>& otherCopies() const noexcept { return otherCopyPaths_; }
    /// True when another installed app's bundle is at, or contains, `absolutePath`.
    [[nodiscard]] const InstalledApp* owningInstalledApp(std::string_view absolutePath) const noexcept;
    /// True when `directory` holds an application bundle — the target's or any
    /// other installed one. Such a directory is never removed as a whole.
    [[nodiscard]] bool containsInstalledBundle(std::string_view directory) const noexcept;

    [[nodiscard]] bool targetIsApple() const noexcept { return isApple_; }
    [[nodiscard]] const std::string& bundleIdLower() const noexcept { return bundleId_; }
    [[nodiscard]] const std::string& bundlePath() const noexcept { return bundlePath_; }

private:
    struct NameKey {
        std::string key;
        MatchReason reason;
    };
    struct OwnedId {
        std::string id;
        std::vector<std::string> sharedWith;
    };

    [[nodiscard]] bool ownedByLongerSibling(std::string_view lowerId, std::size_t ownLength) const;
    [[nodiscard]] bool isForeignIdentifier(std::string_view lowerId) const;
    [[nodiscard]] std::vector<std::string> siblingsDeclaringGroup(std::string_view lowerGroup) const;
    [[nodiscard]] std::vector<std::string> siblingsNamed(std::string_view nameKey) const;

    std::string bundleId_;      // lower case
    std::string bundlePath_;
    std::string teamIdLower_;
    std::string vendor_;        // normalised vendor token, "" when unusable
    std::string vendorDomain_;  // "zoom.us" for "us.zoom.xos"
    bool isApple_ = false;
    bool prefixRuleAllowed_ = false;  // identifier has at least three components
    bool exactRuleAllowed_ = false;
    std::vector<NameKey> nameKeys_;
    std::vector<std::string> vendorChildKeys_;
    std::vector<std::string> processNames_;  // raw, case preserved
    std::vector<OwnedId> ownEmbedded_;
    std::vector<std::string> helperLabels_;  // lower case
    std::vector<std::string> ownGroups_;     // lower case
    std::vector<const InstalledApp*> others_;
    std::vector<std::string> otherCopyPaths_;
    ForeignOwnerLookup foreignOwner_;
    mutable std::mutex foreignMutex_;  // the rules are evaluated from several scan threads
    mutable std::unordered_map<std::string, bool> foreignCache_;
};

}  // namespace vortex
