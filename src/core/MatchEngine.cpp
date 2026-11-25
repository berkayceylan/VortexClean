#include "MatchEngine.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "PathUtil.hpp"
#include "StringUtil.hpp"

namespace vortex {

namespace {

// Words that appear in the names of countless unrelated products. A name made
// only of these (or of tokens shorter than four characters) is never used for
// fuzzy matching.
constexpr auto kStopWords = std::to_array<std::string_view>({
    "agent",       "agents",     "alpha",      "and",         "app",          "apple",     "application",
    "applications", "apps",      "audio",      "backup",      "base",         "beta",      "browser",
    "cache",       "caches",     "client",     "cloud",       "com",          "common",    "config",
    "control",     "core",       "crash",      "daemon",      "data",         "default",   "demo",
    "desktop",     "docs",       "document",   "documents",   "download",     "downloads", "driver",
    "drivers",     "editor",     "electron",   "extension",   "extensions",   "for",       "framework",
    "frameworks",  "free",       "game",       "games",       "helper",       "helpers",   "home",
    "installer",   "java",       "launcher",   "library",     "lite",         "log",       "login",
    "logs",        "mac",        "macos",      "main",        "manager",      "max",       "media",
    "menu",        "mini",       "monitor",    "music",       "net",          "new",       "node",
    "old",         "one",        "org",        "osx",         "photo",        "photos",    "player",
    "plugin",      "plugins",    "plus",       "preferences", "pro",          "python",    "remote",
    "reporter",    "server",     "service",    "services",    "settings",     "setup",     "shared",
    "software",    "status",     "store",      "support",     "sync",         "system",    "temp",
    "test",        "the",        "tmp",        "tool",        "tools",        "uninstaller", "update",
    "updater",     "updates",    "user",       "users",       "util",         "utilities", "utility",
    "utils",       "video",      "viewer",     "web",         "work",
});

// Second-level identifier components that are hosting platforms or placeholders
// rather than a vendor, so they never identify a vendor folder.
constexpr auto kGenericVendorTokens = std::to_array<std::string_view>({
    "apple",     "bitbucket", "blogspot",  "company",   "developer",   "domain",    "example",
    "github",    "gitlab",    "gmail",     "googlecode", "hotmail",    "icloud",    "localhost",
    "mycompany", "outlook",   "personal",  "private",   "sample",      "sourceforge", "unknown",
    "wordpress", "yahoo",     "yourcompany",
});

// Extensions that are decoration around an identifier or name in Library folders.
constexpr auto kKnownExtensions = std::to_array<std::string_view>({
    "plist",  "savedstate", "binarycookies", "sfl",    "sfl2",   "sfl3",   "sfl4",  "log",
    "bom",    "pkg",        "qlgenerator",   "prefpane", "framework", "bundle", "plugin", "kext",
    "app",    "appex",      "lockfile",      "db",     "sqlite", "sqlite3", "json",
});

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& set, std::string_view value) noexcept {
    return std::find(set.begin(), set.end(), value) != set.end();
}

/// True when `id` is `prefix` followed by a dot and at least one more character.
bool isDottedChild(std::string_view id, std::string_view prefix) noexcept {
    return !prefix.empty() && id.size() > prefix.size() + 1 && id[prefix.size()] == '.' &&
           id.substr(0, prefix.size()) == prefix;
}

std::size_t componentCount(std::string_view id) noexcept {
    if (id.empty()) return 0;
    return static_cast<std::size_t>(std::count(id.begin(), id.end(), '.')) + 1;
}

/// First two dot-separated components ("com.vendor"), or "" when there are fewer than three.
std::string_view vendorPrefix(std::string_view lowerId) noexcept {
    const std::size_t first = lowerId.find('.');
    if (first == std::string_view::npos) return {};
    const std::size_t second = lowerId.find('.', first + 1);
    if (second == std::string_view::npos) return {};
    return lowerId.substr(0, second);
}

bool containsString(const std::vector<std::string>& list, std::string_view value) noexcept {
    return std::find(list.begin(), list.end(), value) != list.end();
}

void addUnique(std::vector<std::string>& list, std::string value) {
    if (!value.empty() && !containsString(list, value)) list.push_back(std::move(value));
}

/// Removes a trailing ".<UUID>" or ".<12 hex digits>" host identifier (Preferences/ByHost).
std::string_view stripHostSuffix(std::string_view stem) noexcept {
    const std::size_t dot = stem.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return stem;
    const std::string_view tail = stem.substr(dot + 1);
    if (str::isUuid(tail) || (tail.size() == 12 && str::isHex(tail))) return stem.substr(0, dot);
    return stem;
}

MatchVerdict better(MatchVerdict a, MatchVerdict b) {
    if (!a.matched) return b;
    if (!b.matched) return a;
    return static_cast<int>(b.confidence) < static_cast<int>(a.confidence) ? b : a;
}

MatchVerdict verdict(Confidence confidence, MatchReason reason, std::vector<std::string> sharedWith = {}) {
    MatchVerdict v;
    v.matched = true;
    v.confidence = confidence;
    v.reason = reason;
    v.shared = !sharedWith.empty();
    v.sharedWith = std::move(sharedWith);
    return v;
}

/// Distinctive names of an application with the reason each one would carry.
std::vector<std::pair<std::string, MatchReason>> distinctiveNames(const AppInfo& app) {
    std::vector<std::pair<std::string, MatchReason>> out;
    const auto add = [&out](std::string_view raw, MatchReason reason) {
        if (!MatchEngine::isDistinctiveName(raw)) return;
        std::string key = str::normalizeName(raw);
        for (const auto& existing : out) {
            if (existing.first == key) return;
        }
        out.emplace_back(std::move(key), reason);
    };
    add(app.displayName, MatchReason::AppNameMatch);
    add(app.bundleDisplayName, MatchReason::AppNameMatch);
    add(app.bundleName, MatchReason::AppNameMatch);
    add(app.executableName, MatchReason::ExecutableNameHeuristic);
    if (const std::size_t dot = app.bundleId.rfind('.'); dot != std::string::npos) {
        add(std::string_view(app.bundleId).substr(dot + 1), MatchReason::BundleIdNameHeuristic);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// InstalledAppIndex
// ---------------------------------------------------------------------------

InstalledAppIndex::InstalledAppIndex(const std::vector<AppInfo>& apps) {
    apps_.reserve(apps.size());
    for (const AppInfo& app : apps) add(app);
}

void InstalledAppIndex::add(const AppInfo& app) {
    InstalledApp entry;
    entry.bundlePath = app.bundlePath;
    entry.displayName = app.displayName;
    entry.bundleIdLower = str::toLower(app.bundleId);
    entry.teamId = app.signing.teamIdentifier;
    entry.vendorToken = MatchEngine::vendorToken(app.bundleId);
    for (auto& name : distinctiveNames(app)) entry.nameKeys.push_back(std::move(name.first));
    for (const std::string& id : app.embeddedBundleIds) addUnique(entry.embeddedIdsLower, str::toLower(id));
    for (const std::string& group : app.signing.applicationGroups) {
        addUnique(entry.appGroupsLower, str::toLower(group));
    }
    apps_.push_back(std::move(entry));
}

// ---------------------------------------------------------------------------
// Vocabulary rules
// ---------------------------------------------------------------------------

bool MatchEngine::isStopWord(std::string_view lowerToken) noexcept {
    return contains(kStopWords, lowerToken);
}

bool MatchEngine::isDistinctiveName(std::string_view rawName) {
    const std::string normalized = str::normalizeName(rawName);
    if (normalized.size() < 4) return false;
    if (str::isAllDigits(normalized)) return false;
    if (isAppleIdentifier(rawName)) return false;
    for (const std::string& token : str::nameTokens(rawName)) {
        if (token.size() >= 4 && !isStopWord(token) && !str::isAllDigits(token)) return true;
    }
    return false;
}

bool MatchEngine::isAppleIdentifier(std::string_view identifier) noexcept {
    const std::string lower = str::toLower(identifier);
    const std::string_view id(lower);
    return id == "com.apple" || id.starts_with("com.apple.") || id.starts_with("group.com.apple.") ||
           id == "group.com.apple" || id.find(".com.apple.") != std::string_view::npos ||
           id.ends_with(".com.apple");
}

std::string MatchEngine::vendorToken(std::string_view bundleId) {
    const std::vector<std::string> parts = str::split(str::toLower(bundleId), '.');
    if (parts.size() < 3) return {};
    const std::string& topLevel = parts[0];
    if (topLevel.size() < 2 || topLevel.size() > 12) return {};
    for (const char ch : topLevel) {
        if (ch < 'a' || ch > 'z') return {};
    }
    std::string vendor = str::normalizeName(parts[1]);
    if (vendor.size() < 4 || isStopWord(vendor) || contains(kGenericVendorTokens, std::string_view(vendor))) {
        return {};
    }
    return vendor;
}

std::vector<std::string> MatchEngine::candidateStems(std::string_view entryName) {
    std::vector<std::string> out;
    const auto add = [&out](std::string_view candidate) {
        if (candidate.empty()) return;
        if (std::find(out.begin(), out.end(), candidate) == out.end()) out.emplace_back(candidate);
    };
    std::string_view current = entryName;
    for (int round = 0; round < 4; ++round) {
        add(current);
        add(stripHostSuffix(current));
        const std::string_view extension = path::extension(current);
        if (extension.empty() || !contains(kKnownExtensions, std::string_view(str::toLower(extension)))) break;
        current = path::stem(current);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MatchEngine::MatchEngine(const AppInfo& target, const InstalledAppIndex& installed,
                         ForeignOwnerLookup foreignOwner)
    : bundleId_(str::toLower(target.bundleId)),
      bundlePath_(target.bundlePath),
      teamIdLower_(str::toLower(target.signing.teamIdentifier)),
      isApple_(target.isAppleApp || isAppleIdentifier(target.bundleId)),
      foreignOwner_(std::move(foreignOwner)) {
    const std::size_t components = componentCount(bundleId_);
    prefixRuleAllowed_ = components >= 3;
    // A single-word identifier ("MyApp") is only trusted when it is a distinctive word.
    exactRuleAllowed_ = !bundleId_.empty() && (components >= 2 || isDistinctiveName(bundleId_));

    if (!isApple_) {
        vendor_ = vendorToken(bundleId_);
        if (!vendor_.empty()) {
            const std::vector<std::string> parts = str::split(bundleId_, '.');
            vendorDomain_ = parts[1] + "." + parts[0];
        }
    }

    for (const InstalledApp& other : installed.apps()) {
        if (str::iequals(other.bundlePath, bundlePath_)) continue;
        if (!bundleId_.empty() && other.bundleIdLower == bundleId_) {
            otherCopyPaths_.push_back(other.bundlePath);
            continue;
        }
        others_.push_back(&other);
    }

    for (auto& name : distinctiveNames(target)) nameKeys_.push_back(NameKey{std::move(name.first), name.second});

    // Inside the vendor's folder the product name is usually written without the
    // vendor prefix: "Google Chrome" lives in "Google/Chrome".
    if (!vendor_.empty()) {
        for (const std::string_view raw :
             {std::string_view(target.displayName), std::string_view(target.bundleDisplayName),
              std::string_view(target.bundleName)}) {
            const std::vector<std::string> tokens = str::nameTokens(raw);
            if (tokens.size() < 2 || tokens.front() != vendor_) continue;
            const std::size_t firstSpace = raw.find_first_of(" -_");
            if (firstSpace == std::string_view::npos) continue;
            const std::string_view remainder = str::trim(raw.substr(firstSpace + 1));
            if (isDistinctiveName(remainder)) addUnique(vendorChildKeys_, str::normalizeName(remainder));
        }
    }

    for (const std::string_view raw :
         {std::string_view(target.executableName), std::string_view(target.displayName)}) {
        if (isDistinctiveName(raw)) addUnique(processNames_, std::string(raw));
    }

    // Helpers shipped inside the bundle count as "ours" only when they share the
    // vendor prefix; everything else (Sparkle, Squirrel, crash reporters) is a
    // third-party component that many unrelated apps embed.
    const std::string_view ownPrefix = vendorPrefix(bundleId_);
    for (const std::string& raw : target.embeddedBundleIds) {
        std::string id = str::toLower(raw);
        if (id.empty() || id == bundleId_ || isAppleIdentifier(id)) continue;
        if (isDottedChild(id, bundleId_)) continue;  // already covered by the prefix rule
        if (ownPrefix.empty() || vendorPrefix(id) != ownPrefix) continue;
        OwnedId owned;
        owned.id = std::move(id);
        for (const InstalledApp* other : others_) {
            if (other->bundleIdLower == owned.id || containsString(other->embeddedIdsLower, owned.id)) {
                addUnique(owned.sharedWith, other->displayName);
            }
        }
        ownEmbedded_.push_back(std::move(owned));
    }

    for (const std::string& raw : target.helperLabels) {
        std::string label = str::toLower(raw);
        if (!label.empty() && !isAppleIdentifier(label)) addUnique(helperLabels_, std::move(label));
    }
    for (const std::string& raw : target.signing.applicationGroups) {
        std::string group = str::toLower(raw);
        if (!group.empty() && !isAppleIdentifier(group)) addUnique(ownGroups_, std::move(group));
    }
}

// ---------------------------------------------------------------------------
// Sibling helpers
// ---------------------------------------------------------------------------

bool MatchEngine::ownedByLongerSibling(std::string_view lowerId, std::size_t ownLength) const {
    const auto claims = [&](const std::string& siblingId) noexcept {
        return siblingId.size() > ownLength && (lowerId == siblingId || isDottedChild(lowerId, siblingId));
    };
    for (const InstalledApp* other : others_) {
        if (claims(other->bundleIdLower)) return true;
        for (const std::string& embedded : other->embeddedIdsLower) {
            if (claims(embedded)) return true;
        }
    }
    if (!foreignOwner_) return false;
    // The identifier itself, then each dotted ancestor that is still longer than
    // our own: "com.vendor.app.other.cache" -> "com.vendor.app.other".
    for (std::string_view candidate = lowerId; candidate.size() > ownLength;) {
        if (isForeignIdentifier(candidate)) return true;
        const std::size_t dot = candidate.rfind('.');
        if (dot == std::string_view::npos) break;
        candidate = candidate.substr(0, dot);
    }
    return false;
}

bool MatchEngine::isForeignIdentifier(std::string_view lowerId) const {
    std::string key(lowerId);
    {
        const std::lock_guard<std::mutex> lock(foreignMutex_);
        const auto cached = foreignCache_.find(key);
        if (cached != foreignCache_.end()) return cached->second;
    }
    const bool foreign = foreignOwner_(lowerId);  // may be slow: asked outside the lock
    const std::lock_guard<std::mutex> lock(foreignMutex_);
    foreignCache_.emplace(std::move(key), foreign);
    return foreign;
}

std::vector<std::string> MatchEngine::siblingsDeclaringGroup(std::string_view lowerGroup) const {
    std::vector<std::string> names;
    for (const InstalledApp* other : others_) {
        if (containsString(other->appGroupsLower, lowerGroup)) addUnique(names, other->displayName);
    }
    return names;
}

std::vector<std::string> MatchEngine::siblingsNamed(std::string_view nameKey) const {
    std::vector<std::string> names;
    for (const InstalledApp* other : others_) {
        if (containsString(other->nameKeys, nameKey)) addUnique(names, other->displayName);
    }
    return names;
}

std::vector<std::string> MatchEngine::vendorSiblings() const {
    std::vector<std::string> names;
    for (const InstalledApp* other : others_) {
        const bool sameVendor = !vendor_.empty() && other->vendorToken == vendor_;
        const bool sameTeam = !teamIdLower_.empty() && str::iequals(other->teamId, teamIdLower_);
        if (sameVendor || sameTeam) addUnique(names, other->displayName);
    }
    return names;
}

const InstalledApp* MatchEngine::owningInstalledApp(std::string_view absolutePath) const noexcept {
    for (const InstalledApp* other : others_) {
        if (path::iIsWithinOrEqual(absolutePath, other->bundlePath)) return other;
    }
    return nullptr;
}

bool MatchEngine::containsInstalledBundle(std::string_view directory) const noexcept {
    if (path::iIsStrictlyWithin(bundlePath_, directory)) return true;
    for (const InstalledApp* other : others_) {
        if (path::iIsStrictlyWithin(other->bundlePath, directory)) return true;
    }
    for (const std::string& copy : otherCopyPaths_) {
        if (path::iIsStrictlyWithin(copy, directory)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Attribution rules
// ---------------------------------------------------------------------------

MatchVerdict MatchEngine::matchIdentifier(std::string_view identifier) const {
    const std::string id = str::toLower(str::trim(identifier));
    if (id.empty() || isAppleIdentifier(id)) return {};

    if (exactRuleAllowed_ && id == bundleId_) return verdict(Confidence::Exact, MatchReason::ExactBundleId);

    for (const std::string& label : helperLabels_) {
        if (id == label) return verdict(Confidence::Exact, MatchReason::HelperLabel);
    }

    if (prefixRuleAllowed_ && isDottedChild(id, bundleId_)) {
        if (ownedByLongerSibling(id, bundleId_.size())) return {};
        return verdict(Confidence::High, MatchReason::BundleIdPrefix);
    }

    for (const std::string& label : helperLabels_) {
        if (componentCount(label) >= 3 && isDottedChild(id, label)) {
            if (ownedByLongerSibling(id, label.size())) return {};
            return verdict(Confidence::High, MatchReason::HelperLabel);
        }
    }

    for (const OwnedId& owned : ownEmbedded_) {
        if (id == owned.id) return verdict(Confidence::High, MatchReason::EmbeddedHelperId, owned.sharedWith);
        if (componentCount(owned.id) >= 3 && isDottedChild(id, owned.id)) {
            if (ownedByLongerSibling(id, owned.id.size())) return {};
            return verdict(Confidence::High, MatchReason::EmbeddedHelperId, owned.sharedWith);
        }
    }
    return {};
}

MatchVerdict MatchEngine::matchGroupIdentifier(std::string_view name) const {
    const std::string lower = str::toLower(str::trim(name));
    if (lower.empty() || isAppleIdentifier(lower)) return {};

    // 1. A group the application is entitled to: the strongest possible evidence.
    if (containsString(ownGroups_, lower)) {
        return verdict(Confidence::Exact, MatchReason::EntitledAppGroup, siblingsDeclaringGroup(lower));
    }

    const std::string_view view(lower);
    constexpr std::string_view groupPrefix = "group.";

    // 2. "group.<bundle id>" and friends.
    if (view.starts_with(groupPrefix)) {
        MatchVerdict v = matchIdentifier(view.substr(groupPrefix.size()));
        if (v) {
            v.sharedWith = siblingsDeclaringGroup(lower);
            v.shared = !v.sharedWith.empty();
            return v;
        }
    }

    // 3. Team-prefixed names: "<TEAMID>.<anything>".
    if (!teamIdLower_.empty() && isDottedChild(view, teamIdLower_)) {
        std::string_view rest = view.substr(teamIdLower_.size() + 1);
        MatchVerdict v = matchIdentifier(rest);
        if (!v && rest.starts_with(groupPrefix)) v = matchIdentifier(rest.substr(groupPrefix.size()));
        const std::vector<std::string> declaredBy = siblingsDeclaringGroup(lower);
        if (v) {
            v.sharedWith = declaredBy;
            v.shared = !declaredBy.empty();
            return v;
        }
        // A sibling is entitled to this group and the target is not (it would have
        // matched above): the data is the sibling's, whatever else the team shares.
        if (!declaredBy.empty()) return {};

        std::vector<std::string> sameTeam;
        for (const InstalledApp* other : others_) {
            if (str::iequals(other->teamId, teamIdLower_)) addUnique(sameTeam, other->displayName);
        }
        if (sameTeam.empty()) return verdict(Confidence::High, MatchReason::TeamIdGroupContainer);
        return verdict(Confidence::Medium, MatchReason::TeamIdGroupContainer, std::move(sameTeam));
    }
    return {};
}

MatchVerdict MatchEngine::matchName(std::string_view rawName) const {
    if (isAppleIdentifier(rawName)) return {};
    const std::string key = str::normalizeName(rawName);
    if (key.empty()) return {};
    for (const NameKey& name : nameKeys_) {
        if (name.key == key) return verdict(Confidence::Medium, name.reason, siblingsNamed(key));
    }
    return {};
}

MatchVerdict MatchEngine::matchVendorChild(std::string_view rawName) const {
    if (MatchVerdict v = matchName(rawName)) return v;
    if (isAppleIdentifier(rawName)) return {};
    const std::string key = str::normalizeName(rawName);
    if (key.empty()) return {};
    if (containsString(vendorChildKeys_, key)) {
        // Another product of the same vendor whose shortened name is identical would be ambiguous.
        std::vector<std::string> clash = siblingsNamed(key);
        return verdict(Confidence::Medium, MatchReason::AppNameMatch, std::move(clash));
    }
    return {};
}

MatchVerdict MatchEngine::matchEntry(std::string_view entryName, bool allowNameRules) const {
    const std::vector<std::string> stems = candidateStems(entryName);
    MatchVerdict best;
    for (const std::string& stem : stems) best = better(std::move(best), matchIdentifier(stem));
    if (!best && allowNameRules) {
        for (const std::string& stem : stems) best = better(std::move(best), matchName(stem));
    }
    return best;
}

MatchVerdict MatchEngine::matchDiagnosticReport(std::string_view fileName) const {
    static constexpr auto kReportExtensions =
        std::to_array<std::string_view>({"ips", "crash", "diag", "hang", "spin"});
    const std::string extension = str::toLower(path::extension(fileName));
    if (!contains(kReportExtensions, std::string_view(extension))) return {};

    for (const std::string& process : processNames_) {
        if (fileName.size() <= process.size() + 1 || !fileName.starts_with(process)) continue;
        const std::string_view rest = fileName.substr(process.size());
        // "<Process>-2026-01-02-...", "<Process>_2026-...", or a helper of the process.
        const bool dated = (rest[0] == '-' || rest[0] == '_') && rest[1] >= '0' && rest[1] <= '9';
        const bool helper = rest.starts_with(" Helper");
        if (dated || helper) return verdict(Confidence::Medium, MatchReason::ExecutableNameHeuristic);
    }
    return {};
}

MatchVerdict MatchEngine::matchCrashReporterRecord(std::string_view fileName) const {
    if (!str::iendsWith(fileName, ".plist")) return {};
    const std::string_view stem = fileName.substr(0, fileName.size() - 6);
    const std::size_t underscore = stem.rfind('_');
    if (underscore == std::string_view::npos || underscore == 0) return {};
    if (!str::isUuid(stem.substr(underscore + 1))) return {};
    MatchVerdict v = matchName(stem.substr(0, underscore));
    if (v) v.reason = MatchReason::ExecutableNameHeuristic;
    return v;
}

bool MatchEngine::isVendorDirectoryName(std::string_view entryName) const {
    if (isApple_ || vendor_.empty()) return false;
    const std::string key = str::normalizeName(entryName);
    const bool isVendorWord = key == vendor_;
    const bool isVendorDomain = !vendorDomain_.empty() && str::iequals(entryName, vendorDomain_);
    if (!isVendorWord && !isVendorDomain) return false;
    // When the vendor and the product share a name ("Spotify"), the folder is the
    // product's own and is handled by the name rule instead.
    for (const NameKey& name : nameKeys_) {
        if (name.key == key) return false;
    }
    return true;
}

}  // namespace vortex
