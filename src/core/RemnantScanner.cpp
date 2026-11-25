#include "RemnantScanner.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <dirent.h>
#include <fts.h>
#include <memory>
#include <mutex>
#include <numeric>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "BomParser.hpp"
#include "CFUtil.hpp"
#include "DiskAnalyzer.hpp"
#include "FileUtil.hpp"
#include "PathPolicy.hpp"
#include "PathUtil.hpp"
#include "StringUtil.hpp"

namespace vortex {

namespace {

constexpr std::size_t kMaxLaunchdPlistBytes = std::size_t{1} << 20;
constexpr std::size_t kMaxContainerMetadataBytes = std::size_t{1} << 20;

// ---------------------------------------------------------------------------
// Candidates and their thread-safe collection
// ---------------------------------------------------------------------------

struct Candidate {
    std::string path;
    RemnantCategory category = RemnantCategory::ApplicationSupport;
    Domain domain = Domain::User;
    MatchVerdict verdict;
    std::string launchdLabel;
    std::string receiptPackageId;
    std::string warning;
    bool forceUnchecked = false;
};

class Collector {
public:
    void add(Candidate candidate) {
        const std::lock_guard<std::mutex> lock(mutex_);
        candidates_.push_back(std::move(candidate));
    }
    void noteDenied(std::string path) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(denied_.begin(), denied_.end(), path) == denied_.end()) denied_.push_back(std::move(path));
    }
    void note(std::string text) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(notes_.begin(), notes_.end(), text) == notes_.end()) notes_.push_back(std::move(text));
    }
    [[nodiscard]] std::vector<Candidate> takeCandidates() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(candidates_, {});
    }
    [[nodiscard]] std::vector<std::string> takeDenied() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(denied_, {});
    }
    [[nodiscard]] std::vector<std::string> takeNotes() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(notes_, {});
    }

private:
    std::mutex mutex_;
    std::vector<Candidate> candidates_;
    std::vector<std::string> denied_;
    std::vector<std::string> notes_;
};

// ---------------------------------------------------------------------------
// Scan locations
// ---------------------------------------------------------------------------

enum class LocationKind {
    Generic,            ///< identifier rules, then name rules
    IdentifierOnly,     ///< identifier rules only
    Groups,             ///< group / team prefixed names, then identifier rules
    Containers,         ///< identifier rules, plus container metadata for UUID-named folders
    Launchd,            ///< launchd job property lists
    BundlePlugins,      ///< plug-in bundles: file name, then the bundle's own CFBundleIdentifier
    DiagnosticReports,  ///< "<Process>-<date>.ips" reports
    CrashReporter,      ///< "<Name>_<UUID>.plist" records
};

struct Location {
    std::string dir;
    RemnantCategory category;
    Domain domain;
    LocationKind kind;
    bool vendorFolders;
};

std::vector<Location> buildLocations(const ScanRoots& roots) {
    std::vector<Location> list;
    const auto add = [&list](const std::string& base, std::string_view sub, RemnantCategory category,
                             Domain domain, LocationKind kind, bool vendorFolders = false) {
        if (base.empty()) return;
        list.push_back(Location{sub.empty() ? base : path::join(base, sub), category, domain, kind, vendorFolders});
    };
    using C = RemnantCategory;
    using K = LocationKind;

    const std::string& user = roots.userLibrary;
    add(user, "Application Support", C::ApplicationSupport, Domain::User, K::Generic, true);
    add(user, "Application Support/CrashReporter", C::Logs, Domain::User, K::CrashReporter);
    add(user, "Application Support/com.apple.sharedfilelist/com.apple.LSSharedFileList.ApplicationRecentDocuments",
        C::SavedState, Domain::User, K::IdentifierOnly);
    add(user, "Caches", C::Caches, Domain::User, K::Generic, true);
    add(user, "Preferences", C::Preferences, Domain::User, K::Generic, true);
    add(user, "Preferences/ByHost", C::Preferences, Domain::User, K::IdentifierOnly);
    add(user, "Containers", C::Containers, Domain::User, K::Containers);
    add(user, "Group Containers", C::GroupContainers, Domain::User, K::Groups);
    add(user, "Saved Application State", C::SavedState, Domain::User, K::IdentifierOnly);
    add(user, "Logs", C::Logs, Domain::User, K::Generic, true);
    add(user, "Logs/DiagnosticReports", C::Logs, Domain::User, K::DiagnosticReports);
    add(user, "WebKit", C::WebKit, Domain::User, K::IdentifierOnly);
    add(user, "HTTPStorages", C::HTTPStorages, Domain::User, K::IdentifierOnly);
    add(user, "Cookies", C::Cookies, Domain::User, K::IdentifierOnly);
    add(user, "Application Scripts", C::ApplicationScripts, Domain::User, K::Groups);
    add(user, "LaunchAgents", C::LaunchAgents, Domain::User, K::Launchd);
    add(user, "QuickLook", C::QuickLook, Domain::User, K::BundlePlugins);
    add(user, "PreferencePanes", C::PreferencePanes, Domain::User, K::BundlePlugins);
    add(user, "Frameworks", C::Frameworks, Domain::User, K::BundlePlugins);
    add(user, "Receipts", C::Receipts, Domain::User, K::IdentifierOnly);

    const std::string& system = roots.systemLibrary;
    add(system, "Application Support", C::ApplicationSupport, Domain::System, K::Generic, true);
    add(system, "Caches", C::Caches, Domain::System, K::Generic, true);
    add(system, "Preferences", C::Preferences, Domain::System, K::Generic, true);
    add(system, "Logs", C::Logs, Domain::System, K::Generic, true);
    add(system, "Logs/DiagnosticReports", C::Logs, Domain::System, K::DiagnosticReports);
    add(system, "LaunchAgents", C::LaunchAgents, Domain::System, K::Launchd);
    add(system, "LaunchDaemons", C::LaunchDaemons, Domain::System, K::Launchd);
    add(system, "PrivilegedHelperTools", C::PrivilegedHelperTools, Domain::System, K::IdentifierOnly);
    add(system, "QuickLook", C::QuickLook, Domain::System, K::BundlePlugins);
    add(system, "PreferencePanes", C::PreferencePanes, Domain::System, K::BundlePlugins);
    add(system, "Frameworks", C::Frameworks, Domain::System, K::BundlePlugins);

    add(roots.legacyReceiptsDir, "", C::Receipts, Domain::System, K::IdentifierOnly);
    add(roots.userCacheDir, "", C::Caches, Domain::System, K::IdentifierOnly);
    add(roots.userTempDir, "", C::Caches, Domain::System, K::IdentifierOnly);
    return list;
}

/// Maps an arbitrary installed path onto the Library category it lives in.
RemnantCategory categoryForPath(std::string_view p, const ScanRoots& roots) {
    struct Rule {
        std::string_view sub;
        RemnantCategory category;
    };
    static constexpr std::array<Rule, 10> kRules{{
        {"LaunchDaemons", RemnantCategory::LaunchDaemons},
        {"LaunchAgents", RemnantCategory::LaunchAgents},
        {"PrivilegedHelperTools", RemnantCategory::PrivilegedHelperTools},
        {"Application Support", RemnantCategory::ApplicationSupport},
        {"Caches", RemnantCategory::Caches},
        {"Preferences", RemnantCategory::Preferences},
        {"Logs", RemnantCategory::Logs},
        {"Frameworks", RemnantCategory::Frameworks},
        {"PreferencePanes", RemnantCategory::PreferencePanes},
        {"QuickLook", RemnantCategory::QuickLook},
    }};
    for (const std::string* library : {&roots.systemLibrary, &roots.userLibrary}) {
        if (library->empty() || !path::iIsStrictlyWithin(p, *library)) continue;
        for (const Rule& rule : kRules) {
            if (path::iIsStrictlyWithin(p, path::join(*library, rule.sub))) return rule.category;
        }
    }
    return RemnantCategory::PackagePayload;
}

// ---------------------------------------------------------------------------
// Small readers
// ---------------------------------------------------------------------------

struct LaunchdJob {
    std::string label;
    std::string program;
    std::vector<std::string> associatedBundleIds;
};

LaunchdJob readLaunchdJob(const std::string& plistPath, const std::string& installRoot) {
    LaunchdJob job;
    auto plist = cf::readPropertyList(plistPath, kMaxLaunchdPlistBytes);
    if (!plist) return job;
    const CFDictionaryRef dict = cf::asDictionary(plist.value().get());
    if (dict == nullptr) return job;

    job.label = cf::dictString(dict, "Label");
    job.program = cf::dictString(dict, "Program");
    if (job.program.empty()) {
        const std::vector<std::string> arguments = cf::dictStringArray(dict, "ProgramArguments");
        if (!arguments.empty()) job.program = arguments.front();
    }
    if (!job.program.empty() && job.program.front() == '/') {
        job.program = path::normalize(job.program);
        // Job definitions name programs on the disk they are installed on.
        if (installRoot != "/" && !path::isStrictlyWithin(job.program, installRoot)) {
            job.program = path::normalize(installRoot + job.program);
        }
    } else {
        job.program.clear();  // relative programs cannot be attributed to a location
    }

    const CFTypeRef associated = cf::dictValue(dict, "AssociatedBundleIdentifiers");
    if (const CFStringRef single = cf::asString(associated)) {
        job.associatedBundleIds.push_back(cf::toString(single));
    } else {
        job.associatedBundleIds = cf::arrayStrings(cf::asArray(associated));
    }
    return job;
}

/// Identifier recorded by containermanagerd for a (possibly UUID-named) container.
std::string readContainerIdentifier(const std::string& containerPath) {
    auto plist = cf::readPropertyList(path::join(containerPath, ".com.apple.containermanagerd.metadata.plist"),
                                      kMaxContainerMetadataBytes);
    if (!plist) return {};
    return cf::dictString(cf::asDictionary(plist.value().get()), "MCMMetadataIdentifier");
}

/// EPERM is what TCC returns; EACCES inside the user's own home has the same
/// practical meaning (plain UNIX permissions would not normally stand in the way).
bool isPrivacyDenial(int error, std::string_view p, std::string_view home) noexcept {
    if (error == EPERM) return true;
    return error == EACCES && !home.empty() && path::isWithinOrEqual(p, home);
}

bool isIgnorableName(std::string_view name) noexcept {
    return name == ".DS_Store" || name == ".localized" || name.starts_with("._");
}

MatchVerdict makeVerdict(Confidence confidence, MatchReason reason, std::vector<std::string> sharedWith = {}) {
    MatchVerdict v;
    v.matched = true;
    v.confidence = confidence;
    v.reason = reason;
    v.shared = !sharedWith.empty();
    v.sharedWith = std::move(sharedWith);
    return v;
}

MatchVerdict betterOf(MatchVerdict a, MatchVerdict b) {
    if (!a.matched) return b;
    if (!b.matched) return a;
    return static_cast<int>(b.confidence) < static_cast<int>(a.confidence) ? b : a;
}

// ---------------------------------------------------------------------------
// Scan context
// ---------------------------------------------------------------------------

struct Context {
    const ScanRoots& roots;
    const MatchEngine& engine;
    const PathPolicy& policy;
    const CancellationToken& token;
    Collector& out;
    /// Lower-cased absolute paths of every file installed by a matched receipt.
    /// Written by the receipt stage only, read-only afterwards.
    std::unordered_set<std::string> payloadPaths;
};

// ---------------------------------------------------------------------------
// Library location sweep
// ---------------------------------------------------------------------------

void emit(Context& ctx, std::string full, const Location& location, MatchVerdict verdict,
          std::string launchdLabel = {}) {
    Candidate candidate;
    candidate.path = std::move(full);
    candidate.category = location.category;
    candidate.domain = location.domain;
    candidate.verdict = std::move(verdict);
    candidate.launchdLabel = std::move(launchdLabel);
    ctx.out.add(std::move(candidate));
}

void scanVendorFolder(Context& ctx, const std::string& vendorDir, const Location& location) {
    auto listing = io::listDirectory(vendorDir);
    if (!listing) {
        if (isPrivacyDenial(listing.error().code, vendorDir, ctx.roots.homeDir)) ctx.out.noteDenied(vendorDir);
        return;
    }
    std::vector<std::pair<std::string, MatchVerdict>> matched;
    std::size_t unmatched = 0;
    for (const io::DirEntry& child : listing.value()) {
        if (isIgnorableName(child.name)) continue;
        MatchVerdict v = ctx.engine.matchEntry(child.name, false);
        if (!v) {
            for (const std::string& stem : MatchEngine::candidateStems(child.name)) {
                v = betterOf(std::move(v), ctx.engine.matchVendorChild(stem));
            }
        }
        if (v) {
            matched.emplace_back(path::join(vendorDir, child.name), std::move(v));
        } else {
            ++unmatched;
        }
    }

    const std::vector<std::string> siblings = ctx.engine.vendorSiblings();
    if (siblings.empty() && unmatched == 0) {
        // Everything in the vendor folder is this app's: offer the folder itself.
        emit(ctx, vendorDir, location, makeVerdict(Confidence::Medium, MatchReason::VendorDirectory));
        return;
    }
    for (auto& [childPath, v] : matched) emit(ctx, std::move(childPath), location, std::move(v));

    Candidate folder;
    folder.path = vendorDir;
    folder.category = location.category;
    folder.domain = location.domain;
    if (!siblings.empty()) {
        folder.verdict = makeVerdict(Confidence::Medium, MatchReason::VendorDirectory, siblings);
    } else {
        folder.verdict = makeVerdict(Confidence::Medium, MatchReason::VendorDirectory);
        folder.forceUnchecked = true;
        folder.warning = "Vendor folder that also holds data VortexClean cannot attribute to this app.";
    }
    ctx.out.add(std::move(folder));
}

void scanLaunchdEntry(Context& ctx, const std::string& full, const std::string& name, const Location& location) {
    if (!str::iendsWith(name, ".plist")) return;
    const LaunchdJob job = readLaunchdJob(full, ctx.roots.installRoot);
    const std::string_view stem(name.data(), name.size() - 6);
    // Apple's own jobs are never attributed to an application, whatever they launch.
    if (MatchEngine::isAppleIdentifier(job.label) || MatchEngine::isAppleIdentifier(stem)) return;

    MatchVerdict v;
    if (!job.program.empty() && path::iIsStrictlyWithin(job.program, ctx.engine.bundlePath())) {
        v = makeVerdict(Confidence::Exact, MatchReason::LaunchdProgramInBundle);
    }
    if (!v && !ctx.engine.bundleIdLower().empty()) {
        for (const std::string& associated : job.associatedBundleIds) {
            if (str::iequals(associated, ctx.engine.bundleIdLower())) {
                v = makeVerdict(Confidence::Exact, MatchReason::LaunchdAssociatedBundleId);
                break;
            }
        }
    }
    if (!v && !job.label.empty()) v = ctx.engine.matchIdentifier(job.label);
    if (!v) v = ctx.engine.matchEntry(name, false);
    if (!v && !job.program.empty() && ctx.payloadPaths.contains(str::toLower(job.program))) {
        v = makeVerdict(Confidence::High, MatchReason::LaunchdProgramFromReceipt);
    }
    if (!v) return;

    // A job that is ours runs a helper that is ours. Only the dedicated helper
    // folder qualifies: a job may just as well launch /bin/sh or a shared runtime.
    if (!job.program.empty()) {
        const std::string helpers = path::join(ctx.roots.systemLibrary, "PrivilegedHelperTools");
        if (path::iIsStrictlyWithin(job.program, helpers)) {
            // The tool itself, or the bundle directly inside the folder that contains it.
            const std::string_view below = std::string_view(job.program).substr(helpers.size() + 1);
            Candidate tool;
            tool.path = path::join(helpers, below.substr(0, below.find('/')));
            tool.category = RemnantCategory::PrivilegedHelperTools;
            tool.domain = Domain::System;
            tool.verdict = makeVerdict(Confidence::High, MatchReason::LaunchdJobProgram, v.sharedWith);
            ctx.out.add(std::move(tool));
        }
    }
    emit(ctx, full, location, std::move(v), job.label.empty() ? std::string(stem) : job.label);
}

void scanLocation(Context& ctx, const Location& location) {
    if (ctx.token.isCancelled()) return;
    // Resolve the location itself so a relocated (symlinked) Library folder is still found.
    const auto resolved = path::canonical(location.dir);
    if (!resolved) return;
    const std::string& dir = *resolved;

    auto listing = io::listDirectory(dir);
    if (!listing) {
        if (isPrivacyDenial(listing.error().code, dir, ctx.roots.homeDir)) ctx.out.noteDenied(dir);
        return;
    }

    for (const io::DirEntry& entry : listing.value()) {
        if (ctx.token.isCancelled()) return;
        const std::string& name = entry.name;
        if (isIgnorableName(name)) continue;
        std::string full = path::join(dir, name);
        const bool isDir = entry.type == DT_DIR || (entry.type == DT_UNKNOWN && io::isDirectoryNoFollow(full));

        MatchVerdict v;
        switch (location.kind) {
            case LocationKind::Generic:
                v = ctx.engine.matchEntry(name, true);
                if (!v && location.vendorFolders && isDir && ctx.engine.isVendorDirectoryName(name)) {
                    scanVendorFolder(ctx, full, location);
                    continue;
                }
                break;
            case LocationKind::IdentifierOnly:
                v = ctx.engine.matchEntry(name, false);
                break;
            case LocationKind::Groups:
                v = betterOf(ctx.engine.matchGroupIdentifier(name), ctx.engine.matchEntry(name, false));
                break;
            case LocationKind::Containers:
                v = ctx.engine.matchEntry(name, false);
                if (!v && isDir && str::isUuid(name)) {
                    const std::string identifier = readContainerIdentifier(full);
                    if (!identifier.empty()) {
                        v = ctx.engine.matchIdentifier(identifier);
                        if (v) v.reason = MatchReason::ContainerMetadata;
                    }
                }
                break;
            case LocationKind::Launchd:
                scanLaunchdEntry(ctx, full, name, location);
                continue;
            case LocationKind::BundlePlugins:
                v = ctx.engine.matchEntry(name, false);
                if (!v && isDir) {
                    const std::string identifier = BundleInspector::bundleIdentifier(full);
                    if (!identifier.empty()) v = ctx.engine.matchIdentifier(identifier);
                }
                if (!v) v = ctx.engine.matchEntry(name, true);
                break;
            case LocationKind::DiagnosticReports:
                v = ctx.engine.matchDiagnosticReport(name);
                break;
            case LocationKind::CrashReporter:
                v = ctx.engine.matchCrashReporterRecord(name);
                break;
        }
        if (v) emit(ctx, std::move(full), location, std::move(v));
    }
}

// ---------------------------------------------------------------------------
// Package receipts
// ---------------------------------------------------------------------------

struct OtherPackage {
    std::string packageId;
    std::shared_ptr<const ReceiptDigest> digest;
};

/// Turns the payload of one matched receipt into deletion candidates.
///
/// A BOM lists every directory it touches, including shared ones such as
/// /Library/Application Support, so a directory is only ever offered as a unit
/// when *everything* found inside it on disk is listed by this BOM and by no
/// other package ("fully covered"). Otherwise the walk descends and offers the
/// listed files individually, leaving foreign content alone.
class PayloadWalker {
public:
    PayloadWalker(Context& ctx, const ReceiptStore& store, const Receipt& receipt, const BomContents& contents,
                  Confidence confidence, std::vector<std::string> suiteSiblings,
                  const std::vector<OtherPackage>& others, std::size_t& budget)
        : ctx_(ctx),
          receipt_(receipt),
          confidence_(confidence),
          suiteSiblings_(std::move(suiteSiblings)),
          others_(others),
          budget_(budget) {
        types_.reserve(contents.entries.size());
        for (const BomEntry& entry : contents.entries) {
            std::string absolute = store.resolve(receipt.installPrefix, entry.path);
            if (absolute.empty()) continue;
            std::string key = str::toLower(absolute);
            if (!types_.emplace(key, entry.type).second) continue;
            if (entry.type == BomEntryType::File || entry.type == BomEntryType::Symlink) {
                ctx_.payloadPaths.insert(key);
            }
            if (absolute != store.installRoot()) {
                children_[str::toLower(path::parent(absolute))].push_back(std::move(absolute));
            }
        }
        root_ = store.resolve(receipt.installPrefix, ".");
    }

    void run() {
        if (!root_.empty()) visit(root_);
    }

    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

private:
    [[nodiscard]] BomEntryType typeOf(const std::string& lowerPath) const {
        const auto found = types_.find(lowerPath);
        return found == types_.end() ? BomEntryType::Unknown : found->second;
    }

    [[nodiscard]] std::vector<std::string> otherOwners(std::string_view absolutePath) const {
        std::vector<std::string> owners;
        const std::uint64_t fp = ReceiptStore::fingerprint(absolutePath);
        for (const OtherPackage& other : others_) {
            if (other.digest->payload.contains(fp)) owners.push_back("package " + other.packageId);
        }
        return owners;
    }

    void visitChildren(const std::string& lowerDir) {
        const auto found = children_.find(lowerDir);
        if (found == children_.end()) return;
        for (const std::string& child : found->second) {
            if (ctx_.token.isCancelled() || truncated_) return;
            visit(child);
        }
    }

    void visit(const std::string& p) {
        if (ctx_.token.isCancelled() || truncated_) return;
        if (path::iIsWithinOrEqual(p, ctx_.engine.bundlePath())) return;  // the app itself is a separate item
        // Shared roots (/, /usr, /Library ...) are only ever descended into. Anything
        // else inside a SIP-guarded location is a shared system resource: never offered.
        // The same goes for any directory that holds an application bundle (the
        // target's own folder in a suite, for instance): removing it as a unit
        // would take those applications with it.
        const bool sharedRoot = ctx_.policy.isProtectedRoot(p) || path::iIsWithinOrEqual(ctx_.roots.homeDir, p) ||
                                ctx_.engine.containsInstalledBundle(p);
        if (!sharedRoot && ctx_.policy.isSystemLocation(p)) return;

        struct stat st {};
        if (::lstat(p.c_str(), &st) != 0) return;  // already gone
        const std::string lowerPath = str::toLower(p);
        const BomEntryType type = typeOf(lowerPath);

        if (S_ISDIR(st.st_mode)) {
            if (type != BomEntryType::Directory) return;  // the receipt and the disk disagree: leave it
            if (const InstalledApp* other = ctx_.engine.owningInstalledApp(p)) {
                if (str::iequals(p, other->bundlePath)) {
                    std::vector<std::string> sharedWith = suiteSiblings_;
                    if (std::find(sharedWith.begin(), sharedWith.end(), other->displayName) == sharedWith.end()) {
                        sharedWith.push_back(other->displayName);
                    }
                    emit(p, true, std::move(sharedWith),
                         "A separate application that was installed by the same package.");
                }
                return;
            }
            if (sharedRoot) {
                visitChildren(lowerPath);  // shared directory: only what is inside can be ours
                return;
            }
            const std::unordered_set<std::string> covered = computeCoverage(p);
            emitByCoverage(p, lowerPath, covered);
            return;
        }
        if (S_ISREG(st.st_mode) && type == BomEntryType::File) {
            emit(p, false, {}, {});
        } else if (S_ISLNK(st.st_mode) && type == BomEntryType::Symlink) {
            emit(p, false, {}, {});
        }
    }

    void emitByCoverage(const std::string& dir, const std::string& lowerDir,
                        const std::unordered_set<std::string>& covered) {
        if (ctx_.token.isCancelled() || truncated_) return;
        if (covered.contains(lowerDir)) {
            emit(dir, true, {}, {});
            return;
        }
        const auto found = children_.find(lowerDir);
        if (found == children_.end()) return;
        for (const std::string& child : found->second) {
            if (ctx_.token.isCancelled() || truncated_) return;
            struct stat st {};
            if (::lstat(child.c_str(), &st) != 0) continue;
            const std::string lowerChild = str::toLower(child);
            const BomEntryType type = typeOf(lowerChild);
            if (S_ISDIR(st.st_mode)) {
                if (type == BomEntryType::Directory) emitByCoverage(child, lowerChild, covered);
            } else if ((S_ISREG(st.st_mode) && type == BomEntryType::File) ||
                       (S_ISLNK(st.st_mode) && type == BomEntryType::Symlink)) {
                emit(child, false, {}, {});
            }
        }
    }

    /// One physical walk of `dir` that returns every directory in it (lower-cased)
    /// whose complete on-disk content is listed by this BOM and by no other package.
    [[nodiscard]] std::unordered_set<std::string> computeCoverage(const std::string& dir) const {
        std::unordered_set<std::string> covered;
        std::string mutablePath = dir;
        char* const roots[] = {mutablePath.data(), nullptr};
        FTS* const fts = ::fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr);
        if (fts == nullptr) return covered;

        std::vector<bool> stack;  // one flag per open directory: "still fully covered"
        const auto spoil = [&stack]() {
            if (!stack.empty()) stack.back() = false;
        };
        for (;;) {
            if (ctx_.token.isCancelled()) {
                covered.clear();
                break;
            }
            FTSENT* const entry = ::fts_read(fts);
            if (entry == nullptr) break;
            const std::string_view entryPath(entry->fts_path, entry->fts_pathlen);
            switch (entry->fts_info) {
                case FTS_D:
                    stack.push_back(typeOf(str::toLower(entryPath)) == BomEntryType::Directory);
                    break;
                case FTS_DP: {
                    const bool full = !stack.empty() && stack.back();
                    if (!stack.empty()) stack.pop_back();
                    if (full) {
                        covered.insert(str::toLower(entryPath));
                    } else {
                        spoil();  // an uncovered child makes the parent uncovered too
                    }
                    break;
                }
                case FTS_F:
                case FTS_SL:
                case FTS_SLNONE:
                case FTS_DEFAULT: {
                    if (isIgnorableName(std::string_view(entry->fts_name, entry->fts_namelen))) break;
                    const BomEntryType type = typeOf(str::toLower(entryPath));
                    const bool listed = type == BomEntryType::File || type == BomEntryType::Symlink;
                    if (!listed || !otherOwners(entryPath).empty()) spoil();
                    break;
                }
                default:  // unreadable directory, stat failure, cycle: content unknown
                    spoil();
                    break;
            }
        }
        ::fts_close(fts);
        return covered;
    }

    void emit(const std::string& p, bool isDirectory, std::vector<std::string> sharedWith, std::string warning) {
        // A com.apple.* named item is only ever accepted on the strength of an
        // exact, verified receipt of a third-party application.
        if (MatchEngine::isAppleIdentifier(path::fileName(p)) && confidence_ != Confidence::Exact) return;
        if (budget_ == 0) {
            truncated_ = true;
            return;
        }
        --budget_;

        for (const std::string& sibling : suiteSiblings_) {
            if (std::find(sharedWith.begin(), sharedWith.end(), sibling) == sharedWith.end()) {
                sharedWith.push_back(sibling);
            }
        }
        if (!isDirectory) {
            for (std::string& owner : otherOwners(p)) sharedWith.push_back(std::move(owner));
        }

        Candidate candidate;
        candidate.path = p;
        candidate.category = categoryForPath(p, ctx_.roots);
        candidate.domain = path::isWithinOrEqual(p, ctx_.roots.homeDir) ? Domain::User : Domain::System;
        candidate.verdict = makeVerdict(confidence_, MatchReason::BomReceipt, std::move(sharedWith));
        candidate.receiptPackageId = receipt_.packageId;
        candidate.warning = std::move(warning);
        if (!isDirectory &&
            (candidate.category == RemnantCategory::LaunchDaemons ||
             candidate.category == RemnantCategory::LaunchAgents) &&
            str::iendsWith(p, ".plist")) {
            const LaunchdJob job = readLaunchdJob(p, ctx_.roots.installRoot);
            candidate.launchdLabel = job.label.empty() ? std::string(path::stem(path::fileName(p))) : job.label;
        }
        ctx_.out.add(std::move(candidate));
    }

    Context& ctx_;
    const Receipt& receipt_;
    Confidence confidence_;
    std::vector<std::string> suiteSiblings_;
    const std::vector<OtherPackage>& others_;
    std::size_t& budget_;
    bool truncated_ = false;
    std::string root_;
    std::unordered_map<std::string, BomEntryType> types_;
    std::unordered_map<std::string, std::vector<std::string>> children_;
};

void scanReceipts(Context& ctx, const ReceiptStore& store, ThreadPool& pool, const ScanOptions& options,
                  std::vector<std::string>& matchedPackageIds) {
    // Apple's own receipts are never evidence, and an Apple application is never
    // matched against receipts at all.
    if (ctx.engine.targetIsApple()) return;
    std::vector<Receipt> receipts = store.load(ctx.roots.receiptDirs, ctx.token);
    std::erase_if(receipts, [](const Receipt& r) { return MatchEngine::isAppleIdentifier(r.packageId); });
    if (receipts.empty() || ctx.token.isCancelled()) return;

    std::vector<std::shared_ptr<const ReceiptDigest>> digests(receipts.size());
    {
        TaskGroup group(pool);
        for (std::size_t i = 0; i < receipts.size(); ++i) {
            group.run([&store, &receipts, &digests, &ctx, i] {
                if (!ctx.token.isCancelled()) digests[i] = store.digest(receipts[i], ctx.token);
            });
        }
        group.wait();
    }
    if (ctx.token.isCancelled()) return;

    struct Matched {
        std::size_t index;
        Confidence confidence;
        MatchReason reason;
        std::vector<std::string> suiteSiblings;
    };
    std::vector<Matched> matched;
    std::vector<bool> isMatched(receipts.size(), false);
    const std::string& bundle = ctx.engine.bundlePath();

    for (std::size_t i = 0; i < receipts.size(); ++i) {
        const ReceiptDigest* digest = digests[i].get();
        const bool usable = digest != nullptr && digest->valid;
        bool containsBundle = false;
        std::vector<std::string> suiteSiblings;
        if (usable) {
            containsBundle = path::iIsWithinOrEqual(digest->rootPath, bundle);
            for (const std::string& installed : digest->appBundles) {
                if (str::iequals(installed, bundle)) {
                    containsBundle = true;
                } else if (const InstalledApp* other = ctx.engine.owningInstalledApp(installed)) {
                    if (std::find(suiteSiblings.begin(), suiteSiblings.end(), other->displayName) ==
                        suiteSiblings.end()) {
                        suiteSiblings.push_back(other->displayName);
                    }
                }
            }
        }
        if (containsBundle) {
            matched.push_back(Matched{i, Confidence::Exact, MatchReason::BomReceipt, std::move(suiteSiblings)});
            isMatched[i] = true;
        } else if (const MatchVerdict idMatch = ctx.engine.matchIdentifier(receipts[i].packageId)) {
            matched.push_back(
                Matched{i, idMatch.confidence, MatchReason::ReceiptPackageId, std::move(suiteSiblings)});
            isMatched[i] = true;
        }
    }
    if (matched.empty()) return;

    std::vector<OtherPackage> others;
    for (std::size_t i = 0; i < receipts.size(); ++i) {
        if (!isMatched[i] && digests[i] && digests[i]->valid && !digests[i]->payload.empty()) {
            others.push_back(OtherPackage{receipts[i].packageId, digests[i]});
        }
    }

    std::size_t budget = options.maxPackagePayloadItems;
    bool truncated = false;
    for (const Matched& match : matched) {
        if (ctx.token.isCancelled()) return;
        const Receipt& receipt = receipts[match.index];
        matchedPackageIds.push_back(receipt.packageId);

        for (const std::string* file : {&receipt.plistPath, &receipt.bomPath}) {
            if (file->empty()) continue;
            Candidate candidate;
            candidate.path = *file;
            candidate.category = RemnantCategory::Receipts;
            candidate.domain = Domain::System;
            candidate.verdict = makeVerdict(match.confidence, match.reason, match.suiteSiblings);
            candidate.receiptPackageId = receipt.packageId;
            ctx.out.add(std::move(candidate));
        }

        const ReceiptDigest* digest = digests[match.index].get();
        if (digest == nullptr || !digest->valid) continue;  // receipt files only; payload is not trusted
        auto parsed = BomParser::parseFile(receipt.bomPath, BomParseOptions{}, ctx.token);
        if (!parsed || parsed.value().skippedEntries != 0) continue;

        PayloadWalker walker(ctx, store, receipt, parsed.value(), match.confidence, match.suiteSiblings, others,
                             budget);
        walker.run();
        truncated = truncated || walker.truncated();
    }
    if (truncated) {
        ctx.out.note("The package payload is very large; only the first " +
                     std::to_string(options.maxPackagePayloadItems) + " installed items are listed.");
    }
}

// ---------------------------------------------------------------------------
// Finalisation
// ---------------------------------------------------------------------------

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) out += ", ";
        if (i == 4 && names.size() > 5) {
            out += std::to_string(names.size() - 4) + " more";
            break;
        }
        out += names[i];
    }
    return out;
}

/// Builds the Remnant for a candidate, or returns false when policy forbids offering it.
bool makeRemnant(const Candidate& candidate, const PathPolicy& policy, Remnant& out) {
    struct stat st {};
    if (::lstat(candidate.path.c_str(), &st) != 0) return false;

    const Protection protection = policy.classify(candidate.path);
    if (protection == Protection::Invalid || protection == Protection::ProtectedRoot) return false;

    out = Remnant{};
    out.path = candidate.path;
    out.category = candidate.category;
    out.confidence = candidate.verdict.confidence;
    out.reason = candidate.verdict.reason;
    out.domain = candidate.domain;
    out.isDirectory = S_ISDIR(st.st_mode);
    out.isSymlink = S_ISLNK(st.st_mode);
    out.ownerUid = st.st_uid;
    out.isShared = candidate.verdict.shared;
    out.sharedWith = candidate.verdict.sharedWith;
    out.launchdLabel = candidate.launchdLabel;
    out.receiptPackageId = candidate.receiptPackageId;
    out.warning = candidate.warning;
    out.sipProtected = protection == Protection::SipProtected;
    out.requiresRoot = !out.sipProtected && policy.requiresRoot(candidate.path);

    if (out.sipProtected) {
        out.warning = "Protected by System Integrity Protection; it cannot be removed.";
    } else if (out.isShared && out.warning.empty()) {
        out.warning = "Shared with " + joinNames(out.sharedWith) + ".";
    }
    out.defaultSelected = !out.isShared && !out.sipProtected && !candidate.forceUnchecked;
    return true;
}

int rank(const Candidate& c) noexcept {
    // Lower is better: confidence first, then unshared before shared.
    return static_cast<int>(c.verdict.confidence) * 2 + (c.verdict.shared ? 1 : 0);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ScanRoots ScanRoots::systemDefault() {
    ScanRoots roots;
    roots.homeDir = path::homeDirectory();
    roots.userLibrary = path::join(roots.homeDir, "Library");
    roots.systemLibrary = "/Library";
    roots.receiptDirs = {"/private/var/db/receipts", "/var/db/receipts"};
    roots.legacyReceiptsDir = "/Library/Receipts";
    roots.installRoot = "/";
    roots.consultLaunchServices = true;

    const auto darwinDir = [](int name) -> std::string {
        std::array<char, PATH_MAX> buffer{};
        const std::size_t length = ::confstr(name, buffer.data(), buffer.size());
        if (length == 0 || length > buffer.size()) return {};
        std::string value = path::normalize(buffer.data());
        if (const auto canonical = path::canonical(value)) value = *canonical;
        return value;
    };
    roots.userCacheDir = darwinDir(_CS_DARWIN_USER_CACHE_DIR);
    roots.userTempDir = darwinDir(_CS_DARWIN_USER_TEMP_DIR);
    return roots;
}

Result<ScanRoots> ScanRoots::sandbox(const std::string& directory) {
    const auto canonical = path::canonical(directory);
    if (!canonical) return Error{ENOENT, "the sandbox directory does not exist: " + directory};
    if (*canonical == "/") return Error{EINVAL, "the sandbox directory must not be the root of the disk"};
    ScanRoots roots;
    roots.installRoot = *canonical;
    roots.homeDir = path::join(*canonical, "Users/demo");
    roots.userLibrary = path::join(roots.homeDir, "Library");
    roots.systemLibrary = path::join(*canonical, "Library");
    roots.receiptDirs = {path::join(*canonical, "private/var/db/receipts")};
    roots.legacyReceiptsDir = path::join(*canonical, "Library/Receipts");
    roots.userCacheDir = path::join(*canonical, "private/var/folders/demo/C");
    roots.userTempDir = path::join(*canonical, "private/var/folders/demo/T");
    roots.consultLaunchServices = false;  // it knows nothing about a synthetic disk
    return roots;
}

std::string ScanRoots::displayPath(std::string_view absolutePath) const {
    if (!isSandbox()) return path::abbreviateHome(absolutePath, homeDir);
    const auto logical = [this](std::string_view p) {
        if (path::isStrictlyWithin(p, installRoot)) p.remove_prefix(installRoot.size());
        return std::string(p);
    };
    return path::abbreviateHome(logical(absolutePath), logical(homeDir));
}

std::vector<std::size_t> topMostPathIndices(const std::vector<std::string>& paths) {
    std::vector<std::size_t> order(paths.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    // Shorter paths first, so every ancestor is decided before its descendants.
    std::stable_sort(order.begin(), order.end(),
                     [&paths](std::size_t a, std::size_t b) { return paths[a].size() < paths[b].size(); });

    std::unordered_set<std::string> kept;
    std::vector<std::size_t> result;
    for (const std::size_t index : order) {
        const std::string normalized = path::normalize(paths[index]);
        bool covered = kept.contains(normalized);
        for (std::string ancestor = path::parent(normalized); !covered && !ancestor.empty();
             ancestor = path::parent(ancestor)) {
            covered = kept.contains(ancestor);
            if (ancestor == "/") break;
        }
        if (covered) continue;
        kept.insert(normalized);
        result.push_back(index);
    }
    std::sort(result.begin(), result.end());
    return result;
}

RemnantScanner::RemnantScanner(ScanRoots roots, ThreadPool& pool)
    : roots_(std::move(roots)), pool_(pool), receipts_(roots_.installRoot) {}

ScanResult RemnantScanner::scan(const AppInfo& target, const InstalledAppIndex& installed,
                                const CancellationToken& token, const ScanOptions& options,
                                const ScanCallbacks& callbacks) const {
    const auto started = std::chrono::steady_clock::now();
    ScanResult result;
    result.app = target;

    const PathPolicy policy(roots_.homeDir, roots_.installRoot);
    MatchEngine::ForeignOwnerLookup foreignOwner;
    if (roots_.consultLaunchServices) {
        foreignOwner = [bundlePath = target.bundlePath](std::string_view identifier) {
            return BundleInspector::isRegisteredToAnotherApplication(identifier, bundlePath);
        };
    }
    const MatchEngine engine(target, installed, std::move(foreignOwner));
    Collector collector;
    Context ctx{roots_, engine, policy, token, collector, {}};

    // ---- The application bundle itself -------------------------------------
    Remnant bundle;
    {
        Candidate self;
        self.path = target.bundlePath;
        self.category = RemnantCategory::AppBundle;
        self.domain = path::isWithinOrEqual(target.bundlePath, roots_.homeDir) ? Domain::User : Domain::System;
        self.verdict = makeVerdict(Confidence::Exact, MatchReason::TargetBundle);
        if (!makeRemnant(self, policy, bundle)) {
            // The bundle vanished or sits at a location that may never be deleted.
            bundle = Remnant{};
            bundle.path = target.bundlePath;
            bundle.category = RemnantCategory::AppBundle;
            bundle.confidence = Confidence::Exact;
            bundle.reason = MatchReason::TargetBundle;
            bundle.domain = self.domain;
            bundle.isDirectory = true;
            bundle.sipProtected = true;
            bundle.defaultSelected = false;
            bundle.warning = "This location is protected and cannot be removed.";
        }
        if (target.isSipProtected) {
            bundle.sipProtected = true;
            bundle.requiresRoot = false;
            bundle.defaultSelected = false;
            bundle.warning = "Protected by System Integrity Protection; it cannot be removed.";
        }
        if (target.usageKnown) {
            bundle.usage = target.usage;
            bundle.usageKnown = true;
        }
    }

    const bool lockedSystemApp = target.isSipProtected;
    if (lockedSystemApp) {
        result.notes.emplace_back(
            "This application is part of macOS and protected by System Integrity Protection. "
            "Nothing belonging to it is offered for removal.");
    } else {
        if (engine.targetIsApple()) {
            result.notes.emplace_back(
                "Apple application: items named com.apple.* and Apple package receipts are never attributed "
                "to it, so only name-based matches are listed.");
        }
        // ---- Stage 1: package receipts (fills ctx.payloadPaths) -------------
        scanReceipts(ctx, receipts_, pool_, options, result.matchedPackageIds);

        // ---- Stage 2: parallel sweep of every Library location --------------
        if (!token.isCancelled()) {
            const std::vector<Location> locations = buildLocations(roots_);
            TaskGroup group(pool_);
            for (const Location& location : locations) {
                group.run([&ctx, &location] { scanLocation(ctx, location); });
            }
            group.wait();
        }
    }

    // ---- Stage 3: merge, apply policy, drop redundant nested items ---------
    std::vector<Candidate> candidates = collector.takeCandidates();
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return rank(a) < rank(b); });

    std::vector<Remnant> remnants;
    std::unordered_set<std::string> seenPaths;
    for (const Candidate& candidate : candidates) {
        if (token.isCancelled()) break;
        if (path::iIsWithinOrEqual(candidate.path, target.bundlePath)) continue;  // part of the bundle item
        if (!seenPaths.insert(str::toLower(candidate.path)).second) continue;     // best-ranked wins
        Remnant remnant;
        if (makeRemnant(candidate, policy, remnant)) remnants.push_back(std::move(remnant));
    }

    // An item inside another item that is selected by default is redundant.
    {
        std::unordered_set<std::string> covering;
        for (const Remnant& remnant : remnants) {
            if (remnant.isDirectory && remnant.defaultSelected) covering.insert(str::toLower(remnant.path));
        }
        std::erase_if(remnants, [&covering](const Remnant& remnant) {
            for (std::string ancestor = path::parent(remnant.path); !ancestor.empty() && ancestor != "/";
                 ancestor = path::parent(ancestor)) {
                if (covering.contains(str::toLower(ancestor))) return true;
            }
            return false;
        });
    }

    // Another installed copy of the same application owns the same data.
    if (!engine.otherCopies().empty()) {
        for (Remnant& remnant : remnants) {
            remnant.isShared = true;
            for (const std::string& copy : engine.otherCopies()) remnant.sharedWith.push_back(copy);
            remnant.defaultSelected = false;
            if (!remnant.sipProtected) {
                remnant.warning = "Another copy of this application is installed at " +
                                  joinNames(engine.otherCopies()) + " and uses the same data.";
            }
        }
        result.notes.emplace_back("Another copy of this application is installed; its data is not selected.");
    }

    std::stable_sort(remnants.begin(), remnants.end(), [](const Remnant& a, const Remnant& b) {
        if (a.category != b.category) return a.category < b.category;
        if (a.confidence != b.confidence) return a.confidence < b.confidence;
        return a.path < b.path;
    });

    result.remnants.reserve(remnants.size() + 1);
    result.remnants.push_back(std::move(bundle));
    for (Remnant& remnant : remnants) result.remnants.push_back(std::move(remnant));

    for (std::string& note : collector.takeNotes()) result.notes.push_back(std::move(note));
    result.deniedPaths = collector.takeDenied();
    result.permissionDenied = !result.deniedPaths.empty();

    if (token.isCancelled()) {
        result.cancelled = true;
        result.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return result;
    }
    if (callbacks.onDiscovered) callbacks.onDiscovered(result.remnants);

    // ---- Stage 4: parallel disk-usage measurement --------------------------
    if (options.computeSizes) {
        TaskGroup group(pool_);
        for (std::size_t i = 0; i < result.remnants.size(); ++i) {
            if (result.remnants[i].usageKnown) {
                if (callbacks.onSized) callbacks.onSized(i, result.remnants[i]);
                continue;
            }
            group.run([&result, &token, &callbacks, i] {
                if (token.isCancelled()) return;
                Remnant& remnant = result.remnants[i];  // each task owns exactly one element
                remnant.usage = DiskAnalyzer::measure(remnant.path, token);
                if (remnant.usage.cancelled) return;
                remnant.usageKnown = true;
                remnant.permissionDenied = remnant.usage.permissionDenied;
                if (callbacks.onSized) callbacks.onSized(i, remnant);
            });
        }
        group.wait();

        for (const Remnant& remnant : result.remnants) {
            if (!remnant.permissionDenied) continue;
            const std::string& denied =
                remnant.usage.firstDeniedPath.empty() ? remnant.path : remnant.usage.firstDeniedPath;
            // Unreadable root-owned system files are expected; only user-domain denials point at TCC.
            if (path::isWithinOrEqual(denied, roots_.homeDir)) {
                result.permissionDenied = true;
                if (std::find(result.deniedPaths.begin(), result.deniedPaths.end(), denied) ==
                    result.deniedPaths.end()) {
                    result.deniedPaths.push_back(denied);
                }
            }
        }
    }

    result.cancelled = token.isCancelled();
    result.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

}  // namespace vortex
