// Tests for the deletion pipeline: a dry run must leave every fixture file
// intact and emit a valid JSON audit log; real runs must remove exactly the
// selection; root-required items are escalated or reported; the privileged
// helper re-validates everything it is asked to do.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "AuditLogger.hpp"
#include "DeletionEngine.hpp"
#include "DiskAnalyzer.hpp"
#include "FileUtil.hpp"
#include "Json.hpp"
#include "PathUtil.hpp"
#include "PrivilegedHelper.hpp"
#include "TestSupport.hpp"

using namespace vortex;

namespace {

struct Fixture {
    vt::TempDir temp{"vortex-delete"};
    vt::TempDir auditTemp{"vortex-audit"};
    std::string root;
    std::string home;
    std::string library;
    std::string systemLibrary;
    std::string auditDir;
    std::string trashDir;

    std::vector<std::string> trashed;
    std::vector<std::string> bootedOut;
    int elevateCalls = 0;

    Fixture() {
        root = temp.path();
        home = root + "/Users/tester";
        library = home + "/Library";
        systemLibrary = root + "/Library";
        auditDir = auditTemp.sub("Logs/VortexClean");
        trashDir = temp.sub("FakeTrash");
        vt::makeDirs(trashDir);

        vt::writeFileOfSize(library + "/Caches/com.acme.Rocket/Cache.db", 3000);
        vt::writeFileOfSize(library + "/Caches/com.acme.Rocket/blobs/a.bin", 500);
        vt::writeFileOfSize(library + "/Caches/com.acme.Studio/keep.db", 700);
        vt::writeFileOfSize(library + "/Preferences/com.acme.Rocket.plist", 100);
        vt::writeFileOfSize(library + "/Preferences/com.acme.Studio.plist", 100);
        vt::writeFile(library + "/LaunchAgents/com.acme.Rocket.agent.plist",
                      vt::plistXml({{"Label", "com.acme.Rocket.agent"}}));
        vt::writeFile(systemLibrary + "/LaunchDaemons/com.acme.rocketd.plist",
                      vt::plistXml({{"Label", "com.acme.rocketd"}}));
        vt::writeFileOfSize(root + "/Applications/Rocket.app/Contents/MacOS/Rocket", 9000);
        vt::writeFileOfSize(root + "/outside/precious.txt", 4242);
    }

    [[nodiscard]] DeletionEngine::Config config() const {
        DeletionEngine::Config c;
        c.homeDirectory = home;
        c.systemRoot = root;
        c.auditDirectory = auditDir;
        return c;
    }

    [[nodiscard]] DeletionHooks hooks() {
        DeletionHooks h;
        h.moveToTrash = [this](const std::string& path) -> Result<std::string> {
            const std::string destination = trashDir + "/" + std::string(path::fileName(path));
            if (::rename(path.c_str(), destination.c_str()) != 0) {
                return Error{errno, "rename failed"};
            }
            trashed.push_back(path);
            return destination;
        };
        h.bootout = [this](const std::string& target) {
            bootedOut.push_back(target);
            LaunchctlResult result;
            result.spawned = true;
            result.exitStatus = 0;
            result.serviceTarget = target;
            result.detail = "service booted out";
            return result;
        };
        return h;
    }

    [[nodiscard]] AppInfo target() const {
        AppInfo app;
        app.bundlePath = root + "/Applications/Rocket.app";
        app.bundleId = "com.acme.Rocket";
        app.displayName = "Rocket";
        app.executableName = "Rocket";
        app.shortVersion = "1.2.3";
        app.bundleVersion = "42";
        app.signing.teamIdentifier = "ABCDE12345";
        return app;
    }
};

DeletionItem item(const std::string& path, RemnantCategory category = RemnantCategory::Caches,
                  const std::string& label = {}) {
    DeletionItem out;
    out.path = path;
    out.category = category;
    out.confidence = Confidence::Exact;
    out.reason = MatchReason::ExactBundleId;
    out.launchdLabel = label;
    return out;
}

const ItemOutcome& outcomeFor(const DeletionReport& report, const std::string& path) {
    for (const ItemOutcome& outcome : report.outcomes) {
        if (outcome.item.path == path) return outcome;
    }
    static const ItemOutcome missing;
    vt::reportFailure(__FILE__, __LINE__, "no outcome for " + path);
    return missing;
}

/// Reads and parses the audit log of a report, failing the test when it is not valid JSON.
JsonValue loadAudit(const DeletionReport& report) {
    VT_REQUIRE(report.auditLogError.empty());
    VT_REQUIRE(!report.auditLogPath.empty());
    VT_REQUIRE(vt::exists(report.auditLogPath));
    const std::string text = vt::readFile(report.auditLogPath);
    VT_REQUIRE(!text.empty());
    auto parsed = JsonValue::parse(text);
    if (!parsed) {
        vt::reportFailure(__FILE__, __LINE__, "audit log is not valid JSON: " + parsed.error().message);
        throw vt::AbortTest{};
    }
    return parsed.value();
}

const JsonValue* auditItem(const JsonValue& audit, const std::string& path) {
    for (const JsonValue& entry : audit["items"].asArray()) {
        if (entry["path"].asString() == path) return &entry;
    }
    return nullptr;
}

std::string userTarget(const std::string& label) {
    return "gui/" + std::to_string(::getuid()) + "/" + label;
}

}  // namespace

// ---------------------------------------------------------------------------
// Dry run
// ---------------------------------------------------------------------------

VT_TEST(dry_run_leaves_every_file_intact_and_writes_a_valid_audit_log) {
    Fixture f;
    const bool unprivileged = ::geteuid() != 0;
    if (unprivileged) VT_REQUIRE(::chmod((f.systemLibrary + "/LaunchDaemons").c_str(), 0555) == 0);

    const std::string cacheDir = f.library + "/Caches/com.acme.Rocket";
    const std::string nested = cacheDir + "/Cache.db";
    const std::string prefs = f.library + "/Preferences/com.acme.Rocket.plist";
    const std::string agent = f.library + "/LaunchAgents/com.acme.Rocket.agent.plist";
    const std::string daemon = f.systemLibrary + "/LaunchDaemons/com.acme.rocketd.plist";
    const std::string missing = f.library + "/Caches/already-gone";
    const std::string sip = "/System/Library/CoreServices/Finder.app";
    const std::string sharedRoot = f.library + "/Caches";
    const std::string invalid = "relative/path";

    DeletionRequest request;
    request.target = f.target();
    request.mode = DeleteMode::Trash;
    request.dryRun = true;
    request.items = {
        item(cacheDir),
        item(prefs, RemnantCategory::Preferences),
        item(agent, RemnantCategory::LaunchAgents, "com.acme.Rocket.agent"),
        item(nested),
        item(missing),
        item(sip, RemnantCategory::AppBundle),
        item(sharedRoot),
        item(invalid),
        item(daemon, RemnantCategory::LaunchDaemons, "com.acme.rocketd"),
    };

    const std::vector<std::string> before = vt::snapshotTree(f.root);
    std::size_t progressCalls = 0;
    DeletionHooks hooks = f.hooks();
    hooks.onProgress = [&](std::size_t completed, std::size_t total, const ItemOutcome&) {
        ++progressCalls;
        VT_CHECK(completed <= total);
    };
    hooks.elevate = [&](const std::vector<DeletionItem>&, DeleteMode) {
        ++f.elevateCalls;
        return PrivilegedBatchResult{};
    };
    const DeletionEngine engine(f.config(), hooks);
    const DeletionReport report = engine.execute(request);

    // ---- Nothing on disk changed, and nothing with side effects was called.
    VT_CHECK_EQ(vt::snapshotTree(f.root), before);
    VT_CHECK(f.trashed.empty());
    VT_CHECK(f.bootedOut.empty());
    VT_CHECK_EQ(f.elevateCalls, 0);
    VT_CHECK_EQ(progressCalls, request.items.size());

    // ---- The full validation pipeline still ran.
    VT_REQUIRE(report.outcomes.size() == request.items.size());
    VT_CHECK(report.dryRun);
    VT_CHECK(outcomeFor(report, cacheDir).status == ItemStatus::DryRun);
    VT_CHECK(outcomeFor(report, prefs).status == ItemStatus::DryRun);
    VT_CHECK(outcomeFor(report, agent).status == ItemStatus::DryRun);
    VT_CHECK(outcomeFor(report, nested).status == ItemStatus::DryRun);
    VT_CHECK(outcomeFor(report, missing).status == ItemStatus::SkippedMissing);
    VT_CHECK(outcomeFor(report, sip).status == ItemStatus::SkippedSip);
    VT_CHECK(outcomeFor(report, sharedRoot).status == ItemStatus::Error);
    VT_CHECK(outcomeFor(report, invalid).status == ItemStatus::Error);
    VT_CHECK(outcomeFor(report, daemon).status == ItemStatus::DryRun);
    if (unprivileged) {
        VT_CHECK(outcomeFor(report, daemon).requiresRoot);
        VT_CHECK(outcomeFor(report, daemon).note.find("administrator") != std::string::npos);
        VT_CHECK_EQ(outcomeFor(report, daemon).launchctlTarget, std::string("system/com.acme.rocketd"));
    }
    VT_CHECK(!outcomeFor(report, cacheDir).requiresRoot);
    VT_CHECK_EQ(outcomeFor(report, agent).launchctlTarget, userTarget("com.acme.Rocket.agent"));
    VT_CHECK(!outcomeFor(report, agent).launchctlRan);
    VT_CHECK(outcomeFor(report, nested).note.find(cacheDir) != std::string::npos);
    VT_CHECK_EQ(outcomeFor(report, nested).allocatedBytes, std::uint64_t{0});  // counted with its parent

    const std::uint64_t expectedBytes =
        DiskAnalyzer::measure(cacheDir).allocatedBytes + DiskAnalyzer::measure(prefs).allocatedBytes +
        DiskAnalyzer::measure(agent).allocatedBytes + DiskAnalyzer::measure(daemon).allocatedBytes;
    VT_CHECK_EQ(report.reclaimedBytes, expectedBytes);
    VT_CHECK_EQ(outcomeFor(report, cacheDir).logicalBytes, std::uint64_t{3500});

    // ---- The audit log is valid JSON with the documented structure.
    const JsonValue audit = loadAudit(report);
    VT_CHECK(path::isStrictlyWithin(report.auditLogPath, f.auditDir));
    const std::string fileName(path::fileName(report.auditLogPath));
    VT_CHECK(fileName.starts_with("audit-"));
    VT_CHECK(fileName.ends_with(".json"));
    VT_CHECK_EQ(fileName.size(), std::string("audit-20261124-213800-123.json").size());

    VT_CHECK_EQ(audit["schema"].asString(), std::string("vortexclean.audit/1"));
    VT_CHECK_EQ(audit["generator"]["name"].asString(), std::string("VortexClean"));
    VT_CHECK(!audit["generator"]["version"].asString().empty());
    VT_CHECK_EQ(audit["started_at"].asString().size(), std::string("2026-11-24T21:38:00.123Z").size());
    VT_CHECK(audit["started_at"].asString().ends_with("Z"));
    VT_CHECK(audit["duration_ms"].isInteger());
    VT_CHECK(!audit["host"]["os_version"].asString().empty());
    VT_CHECK_EQ(audit["host"]["user_id"].asInteger(-1), static_cast<std::int64_t>(::getuid()));

    VT_CHECK_EQ(audit["operation"]["mode"].asString(), std::string("trash"));
    VT_CHECK(audit["operation"]["dry_run"].asBool(false));
    VT_CHECK(!audit["operation"]["cancelled"].asBool(true));
    VT_CHECK(!audit["operation"]["elevation_attempted"].asBool(true));

    VT_CHECK_EQ(audit["target"]["name"].asString(), std::string("Rocket"));
    VT_CHECK_EQ(audit["target"]["bundle_id"].asString(), std::string("com.acme.Rocket"));
    VT_CHECK_EQ(audit["target"]["bundle_path"].asString(), f.root + "/Applications/Rocket.app");
    VT_CHECK_EQ(audit["target"]["version"].asString(), std::string("1.2.3"));
    VT_CHECK_EQ(audit["target"]["team_id"].asString(), std::string("ABCDE12345"));

    const JsonValue& summary = audit["summary"];
    VT_CHECK_EQ(summary["attempted"].asInteger(), std::int64_t{9});
    VT_CHECK_EQ(summary["dry_run"].asInteger(), std::int64_t{5});
    VT_CHECK_EQ(summary["skipped_missing"].asInteger(), std::int64_t{1});
    VT_CHECK_EQ(summary["skipped_sip"].asInteger(), std::int64_t{1});
    VT_CHECK_EQ(summary["error"].asInteger(), std::int64_t{2});
    VT_CHECK_EQ(summary["trashed"].asInteger(), std::int64_t{0});
    VT_CHECK_EQ(summary["deleted"].asInteger(), std::int64_t{0});
    VT_CHECK_EQ(summary["skipped_permission"].asInteger(), std::int64_t{0});
    VT_CHECK_EQ(summary["bytes_reclaimed"].asInteger(-1), std::int64_t{0});  // a simulation frees nothing
    VT_CHECK_EQ(summary["bytes_reclaimable"].asInteger(-1), static_cast<std::int64_t>(expectedBytes));

    VT_REQUIRE(audit["items"].isArray());
    VT_REQUIRE(audit["items"].size() == request.items.size());
    for (std::size_t i = 0; i < request.items.size(); ++i) {
        const JsonValue& entry = audit["items"].asArray()[i];
        // Every attempted path is listed, in request order, with its outcome.
        VT_CHECK_EQ(entry["path"].asString(), request.items[i].path);
        VT_CHECK_EQ(entry["status"].asString(), std::string(jsonKey(report.outcomes[i].status)));
        VT_CHECK(entry["size_bytes"].isInteger());
        VT_CHECK(entry["logical_bytes"].isInteger());
        VT_CHECK(entry["requires_root"].isBool());
        VT_CHECK(entry["elevated"].isBool());
        VT_CHECK(entry["category"].isString());
        VT_CHECK_EQ(entry["confidence"].asString(), std::string("exact"));
        VT_CHECK_EQ(entry["reason"].asString(), std::string("exact_bundle_id"));
        VT_CHECK(entry.find("error") != nullptr);
    }
    const JsonValue* sipEntry = auditItem(audit, sip);
    VT_REQUIRE(sipEntry != nullptr);
    VT_CHECK_EQ((*sipEntry)["status"].asString(), std::string("skipped_sip"));
    VT_CHECK((*sipEntry)["error"].isObject());
    VT_CHECK((*sipEntry)["error"]["message"].asString().find("System Integrity Protection") != std::string::npos);
    const JsonValue* cacheEntry = auditItem(audit, cacheDir);
    VT_REQUIRE(cacheEntry != nullptr);
    VT_CHECK((*cacheEntry)["error"].isNull());
    VT_CHECK_EQ((*cacheEntry)["category"].asString(), std::string("caches"));
    VT_CHECK_EQ((*cacheEntry)["size_bytes"].asInteger(), static_cast<std::int64_t>(DiskAnalyzer::measure(cacheDir).allocatedBytes));
    const JsonValue* agentEntry = auditItem(audit, agent);
    VT_REQUIRE(agentEntry != nullptr);
    VT_CHECK_EQ((*agentEntry)["launchd"]["label"].asString(), std::string("com.acme.Rocket.agent"));
    VT_CHECK(!(*agentEntry)["launchd"]["bootout_ran"].asBool(true));
    const JsonValue* invalidEntry = auditItem(audit, invalid);
    VT_REQUIRE(invalidEntry != nullptr);
    VT_CHECK_EQ((*invalidEntry)["error"]["code"].asInteger(), static_cast<std::int64_t>(EINVAL));

    if (unprivileged) ::chmod((f.systemLibrary + "/LaunchDaemons").c_str(), 0755);
}

// ---------------------------------------------------------------------------
// Real runs
// ---------------------------------------------------------------------------

VT_TEST(permanent_delete_removes_exactly_the_selection) {
    Fixture f;
    const std::string cacheDir = f.library + "/Caches/com.acme.Rocket";
    const std::string prefs = f.library + "/Preferences/com.acme.Rocket.plist";
    const std::string agent = f.library + "/LaunchAgents/com.acme.Rocket.agent.plist";
    const std::string link = f.library + "/Caches/com.acme.Rocket.link";
    const std::string bundle = f.root + "/Applications/Rocket.app";

    // A symlink that leaves the selection, a symlink inside it, and a locked file.
    VT_REQUIRE(::symlink((f.root + "/outside").c_str(), link.c_str()) == 0);
    VT_REQUIRE(::symlink((f.root + "/outside").c_str(), (cacheDir + "/escape").c_str()) == 0);
    VT_REQUIRE(::chflags((cacheDir + "/Cache.db").c_str(), UF_IMMUTABLE) == 0);

    const std::uint64_t expectedBytes =
        DiskAnalyzer::measure(cacheDir).allocatedBytes + DiskAnalyzer::measure(prefs).allocatedBytes +
        DiskAnalyzer::measure(agent).allocatedBytes + DiskAnalyzer::measure(link).allocatedBytes +
        DiskAnalyzer::measure(bundle).allocatedBytes;

    DeletionRequest request;
    request.target = f.target();
    request.mode = DeleteMode::Permanent;
    request.items = {
        item(bundle, RemnantCategory::AppBundle),
        item(cacheDir),
        item(prefs, RemnantCategory::Preferences),
        item(agent, RemnantCategory::LaunchAgents, "com.acme.Rocket.agent"),
        item(link),
    };
    const DeletionEngine engine(f.config(), f.hooks());
    const DeletionReport report = engine.execute(request);

    for (const DeletionItem& requested : request.items) {
        VT_CHECK(outcomeFor(report, requested.path).status == ItemStatus::Deleted);
        VT_CHECK(!vt::exists(requested.path));
    }
    VT_CHECK_EQ(report.countOf(ItemStatus::Deleted), std::size_t{5});
    VT_CHECK_EQ(report.reclaimedBytes, expectedBytes);
    VT_CHECK(!report.privacyDenied);
    VT_CHECK(f.trashed.empty());  // permanent mode never goes through the Trash provider

    // Neighbours and the targets of both symlinks are untouched.
    VT_CHECK(vt::exists(f.library + "/Caches/com.acme.Studio/keep.db"));
    VT_CHECK(vt::exists(f.library + "/Preferences/com.acme.Studio.plist"));
    VT_CHECK(vt::exists(f.root + "/outside/precious.txt"));
    VT_CHECK_EQ(vt::readFile(f.root + "/outside/precious.txt").size(), std::size_t{4242});
    VT_CHECK(vt::isDir(f.library + "/Caches"));
    VT_CHECK(vt::isDir(f.root + "/Applications"));

    // The agent was unloaded from the user's launchd domain before its plist went away.
    VT_CHECK_EQ(f.bootedOut, std::vector<std::string>{userTarget("com.acme.Rocket.agent")});
    VT_CHECK(outcomeFor(report, agent).launchctlRan);
    VT_CHECK_EQ(outcomeFor(report, agent).launchctlExit, 0);

    const JsonValue audit = loadAudit(report);
    VT_CHECK_EQ(audit["operation"]["mode"].asString(), std::string("permanent"));
    VT_CHECK(!audit["operation"]["dry_run"].asBool(true));
    VT_CHECK_EQ(audit["summary"]["deleted"].asInteger(), std::int64_t{5});
    VT_CHECK_EQ(audit["summary"]["bytes_reclaimed"].asInteger(), static_cast<std::int64_t>(expectedBytes));
    VT_CHECK_EQ(audit["summary"]["bytes_reclaimable"].asInteger(-1), std::int64_t{0});
    const JsonValue* agentEntry = auditItem(audit, agent);
    VT_REQUIRE(agentEntry != nullptr);
    VT_CHECK_EQ((*agentEntry)["status"].asString(), std::string("deleted"));
    VT_CHECK_EQ((*agentEntry)["launchd"]["service_target"].asString(), userTarget("com.acme.Rocket.agent"));
    VT_CHECK((*agentEntry)["launchd"]["bootout_ran"].asBool(false));
    VT_CHECK_EQ((*agentEntry)["launchd"]["bootout_exit_status"].asInteger(-1), std::int64_t{0});
}

VT_TEST(trash_mode_goes_through_the_provider_and_records_the_new_location) {
    Fixture f;
    const std::string cacheDir = f.library + "/Caches/com.acme.Rocket";
    const std::string prefs = f.library + "/Preferences/com.acme.Rocket.plist";
    const std::string studioPrefs = f.library + "/Preferences/com.acme.Studio.plist";
    const std::string studioCache = f.library + "/Caches/com.acme.Studio";

    DeletionHooks hooks = f.hooks();
    const auto realTrash = hooks.moveToTrash;
    hooks.moveToTrash = [&](const std::string& path) -> Result<std::string> {
        if (path == studioPrefs) return Error{EPERM, "Operation not permitted"};  // what TCC answers
        if (path == studioCache) return Error{EIO, "Input/output error"};
        return realTrash(path);
    };

    DeletionRequest request;
    request.target = f.target();
    request.mode = DeleteMode::Trash;
    request.items = {item(cacheDir), item(prefs, RemnantCategory::Preferences),
                     item(studioPrefs, RemnantCategory::Preferences), item(studioCache)};
    const DeletionEngine engine(f.config(), hooks);
    const DeletionReport report = engine.execute(request);

    VT_CHECK(outcomeFor(report, cacheDir).status == ItemStatus::Trashed);
    VT_CHECK_EQ(outcomeFor(report, cacheDir).trashPath, f.trashDir + "/com.acme.Rocket");
    VT_CHECK(vt::exists(f.trashDir + "/com.acme.Rocket/Cache.db"));  // recoverable
    VT_CHECK(!vt::exists(cacheDir));
    VT_CHECK(outcomeFor(report, prefs).status == ItemStatus::Trashed);

    // A TCC-style refusal is a permission skip that points the user at Full Disk Access.
    VT_CHECK(outcomeFor(report, studioPrefs).status == ItemStatus::SkippedPermission);
    VT_CHECK_EQ(outcomeFor(report, studioPrefs).errorCode, EPERM);
    VT_CHECK(report.privacyDenied);
    VT_CHECK(vt::exists(studioPrefs));
    // Any other failure is an error with its diagnostics preserved.
    VT_CHECK(outcomeFor(report, studioCache).status == ItemStatus::Error);
    VT_CHECK_EQ(outcomeFor(report, studioCache).errorCode, EIO);
    VT_CHECK(vt::exists(studioCache));

    const std::uint64_t trashedBytes =
        outcomeFor(report, cacheDir).allocatedBytes + outcomeFor(report, prefs).allocatedBytes;
    VT_CHECK(trashedBytes > 0);
    VT_CHECK_EQ(report.reclaimedBytes, trashedBytes);

    const JsonValue audit = loadAudit(report);
    VT_CHECK_EQ(audit["summary"]["trashed"].asInteger(), std::int64_t{2});
    VT_CHECK_EQ(audit["summary"]["skipped_permission"].asInteger(), std::int64_t{1});
    VT_CHECK_EQ(audit["summary"]["error"].asInteger(), std::int64_t{1});
    VT_CHECK(audit["operation"]["privacy_denied"].asBool(false));
    const JsonValue* cacheEntry = auditItem(audit, cacheDir);
    VT_REQUIRE(cacheEntry != nullptr);
    VT_CHECK_EQ((*cacheEntry)["trash_path"].asString(), f.trashDir + "/com.acme.Rocket");
    const JsonValue* failed = auditItem(audit, studioCache);
    VT_REQUIRE(failed != nullptr);
    VT_CHECK_EQ((*failed)["error"]["code"].asInteger(), static_cast<std::int64_t>(EIO));
    VT_CHECK_EQ((*failed)["error"]["message"].asString(), std::string("Input/output error"));

    // Without a provider the engine refuses instead of deleting permanently.
    Fixture g;
    DeletionHooks none;
    DeletionRequest again;
    again.target = g.target();
    again.items = {item(g.library + "/Preferences/com.acme.Rocket.plist", RemnantCategory::Preferences)};
    const DeletionReport refused = DeletionEngine(g.config(), none).execute(again);
    VT_CHECK(refused.outcomes.front().status == ItemStatus::Error);
    VT_CHECK(vt::exists(g.library + "/Preferences/com.acme.Rocket.plist"));
}

VT_TEST(root_required_items_are_escalated_or_reported) {
    if (::geteuid() == 0) {
        vt::note("running as root: nothing requires elevation");
        return;
    }
    const auto prepare = [](Fixture& f) {
        VT_REQUIRE(::chmod((f.systemLibrary + "/LaunchDaemons").c_str(), 0555) == 0);
        DeletionRequest request;
        request.target = f.target();
        request.mode = DeleteMode::Permanent;
        request.items = {
            item(f.library + "/Preferences/com.acme.Rocket.plist", RemnantCategory::Preferences),
            item(f.systemLibrary + "/LaunchDaemons/com.acme.rocketd.plist", RemnantCategory::LaunchDaemons,
                 "com.acme.rocketd"),
        };
        return request;
    };

    {
        // (a) No elevation handler: the root item is reported, the user item is still removed.
        Fixture f;
        const DeletionRequest request = prepare(f);
        const DeletionReport report = DeletionEngine(f.config(), f.hooks()).execute(request);
        const ItemOutcome& daemon = outcomeFor(report, request.items[1].path);
        VT_CHECK(daemon.status == ItemStatus::SkippedPermission);
        VT_CHECK(daemon.requiresRoot);
        VT_CHECK(!daemon.elevated);
        VT_CHECK(daemon.errorMessage.find("administrator") != std::string::npos);
        VT_CHECK(vt::exists(request.items[1].path));
        VT_CHECK(outcomeFor(report, request.items[0].path).status == ItemStatus::Deleted);
        VT_CHECK(!report.elevationAttempted);
        VT_CHECK(f.bootedOut.empty());  // the system domain is never touched without root
        VT_CHECK_EQ(loadAudit(report)["summary"]["skipped_permission"].asInteger(), std::int64_t{1});
    }
    {
        // (b) The user dismisses the password dialog.
        Fixture f;
        const DeletionRequest request = prepare(f);
        DeletionHooks hooks = f.hooks();
        hooks.elevate = [&](const std::vector<DeletionItem>& items, DeleteMode) {
            ++f.elevateCalls;
            VT_CHECK_EQ(items.size(), std::size_t{1});  // only what really needs root is escalated
            PrivilegedBatchResult result;
            result.authorized = false;
            result.error = "The administrator authorization was cancelled.";
            return result;
        };
        const DeletionReport report = DeletionEngine(f.config(), hooks).execute(request);
        const ItemOutcome& daemon = outcomeFor(report, request.items[1].path);
        VT_CHECK(daemon.status == ItemStatus::SkippedPermission);
        VT_CHECK_EQ(daemon.errorMessage, std::string("The administrator authorization was cancelled."));
        VT_CHECK(vt::exists(request.items[1].path));
        VT_CHECK(report.elevationAttempted);
        VT_CHECK(!report.elevationAuthorized);
        VT_CHECK_EQ(f.elevateCalls, 1);
        const JsonValue audit = loadAudit(report);
        VT_CHECK(audit["operation"]["elevation_attempted"].asBool(false));
        VT_CHECK(!audit["operation"]["elevation_authorized"].asBool(true));
    }
    {
        // (c) Authorised: the helper's results are merged into the report.
        Fixture f;
        const DeletionRequest request = prepare(f);
        DeletionHooks hooks = f.hooks();
        hooks.elevate = [&](const std::vector<DeletionItem>& items, DeleteMode mode) {
            ++f.elevateCalls;
            VT_CHECK(mode == DeleteMode::Permanent);
            PrivilegedBatchResult result;
            result.authorized = true;
            for (const DeletionItem& requested : items) {
                ::chmod(std::string(path::parent(requested.path)).c_str(), 0755);
                ItemOutcome outcome;
                outcome.item = requested;
                outcome.status = ::unlink(requested.path.c_str()) == 0 ? ItemStatus::Deleted : ItemStatus::Error;
                outcome.launchctlTarget = "system/" + requested.launchdLabel;
                outcome.launchctlRan = true;
                outcome.launchctlExit = 0;
                result.outcomes.push_back(std::move(outcome));
            }
            return result;
        };
        const DeletionReport report = DeletionEngine(f.config(), hooks).execute(request);
        const ItemOutcome& daemon = outcomeFor(report, request.items[1].path);
        VT_CHECK(daemon.status == ItemStatus::Deleted);
        VT_CHECK(daemon.elevated);
        VT_CHECK(daemon.requiresRoot);
        VT_CHECK_EQ(daemon.launchctlTarget, std::string("system/com.acme.rocketd"));
        VT_CHECK(daemon.allocatedBytes > 0);
        VT_CHECK(!vt::exists(request.items[1].path));
        VT_CHECK(report.elevationAuthorized);
        const JsonValue audit = loadAudit(report);
        const JsonValue* entry = auditItem(audit, request.items[1].path);
        VT_REQUIRE(entry != nullptr);
        VT_CHECK((*entry)["elevated"].asBool(false));
        VT_CHECK((*entry)["requires_root"].asBool(false));
        VT_CHECK_EQ((*entry)["status"].asString(), std::string("deleted"));
    }
    {
        // (d) A helper that says nothing about an item must not be mistaken for success.
        Fixture f;
        const DeletionRequest request = prepare(f);
        DeletionHooks hooks = f.hooks();
        hooks.elevate = [](const std::vector<DeletionItem>&, DeleteMode) {
            PrivilegedBatchResult result;
            result.authorized = true;
            return result;
        };
        const DeletionReport report = DeletionEngine(f.config(), hooks).execute(request);
        VT_CHECK(outcomeFor(report, request.items[1].path).status == ItemStatus::Error);
        VT_CHECK(vt::exists(request.items[1].path));
        ::chmod((f.systemLibrary + "/LaunchDaemons").c_str(), 0755);
    }
}

VT_TEST(cancellation_is_reported_and_nothing_is_removed) {
    Fixture f;
    DeletionRequest request;
    request.target = f.target();
    request.mode = DeleteMode::Permanent;
    request.items = {item(f.library + "/Caches/com.acme.Rocket"),
                     item(f.library + "/Preferences/com.acme.Rocket.plist", RemnantCategory::Preferences)};
    const std::vector<std::string> before = vt::snapshotTree(f.root);

    const CancellationToken token;
    token.cancel();
    const DeletionReport report = DeletionEngine(f.config(), f.hooks()).execute(request, token);
    VT_CHECK(report.cancelled);
    for (const ItemOutcome& outcome : report.outcomes) {
        VT_CHECK(outcome.status == ItemStatus::Error);
        VT_CHECK_EQ(outcome.errorCode, ECANCELED);
    }
    VT_CHECK_EQ(vt::snapshotTree(f.root), before);
    VT_CHECK(loadAudit(report)["operation"]["cancelled"].asBool(false));
}

VT_TEST(audit_logs_never_overwrite_each_other) {
    Fixture f;
    DeletionRequest request;
    request.target = f.target();
    request.dryRun = true;
    request.items = {item(f.library + "/Preferences/com.acme.Rocket.plist", RemnantCategory::Preferences)};
    const DeletionEngine engine(f.config(), f.hooks());
    const DeletionReport first = engine.execute(request);
    const DeletionReport second = engine.execute(request);
    VT_CHECK(first.auditLogPath != second.auditLogPath);
    VT_CHECK(vt::exists(first.auditLogPath));
    VT_CHECK(vt::exists(second.auditLogPath));

    // Two reports stamped with the very same millisecond still get distinct files.
    DeletionReport fixed = first;
    fixed.startedEpochMs = 1795556280123;
    const auto a = AuditLogger::write(request, fixed, f.auditDir);
    const auto b = AuditLogger::write(request, fixed, f.auditDir);
    VT_REQUIRE(a.ok() && b.ok());
    VT_CHECK(a.value() != b.value());
    VT_CHECK(b.value().ends_with("-2.json"));
    VT_CHECK(JsonValue::parse(vt::readFile(b.value())).ok());

    VT_CHECK(!AuditLogger::write(request, fixed, "relative/dir").ok());
    VT_CHECK_EQ(AuditLogger::iso8601(0), std::string("1970-01-01T00:00:00.000Z"));
    VT_CHECK_EQ(AuditLogger::iso8601(1795556280123), std::string("2026-11-24T21:38:00.123Z"));
    VT_CHECK_EQ(DeletionEngine::defaultAuditDirectory("/Users/tester"),
                std::string("/Users/tester/Library/Logs/VortexClean"));
}

// ---------------------------------------------------------------------------
// Recursive removal
// ---------------------------------------------------------------------------

VT_TEST(remove_tree_never_follows_symlinks) {
    const vt::TempDir temp("vortex-rm");
    vt::writeFileOfSize(temp.sub("real/dir/file.bin"), 10);
    vt::writeFileOfSize(temp.sub("outside/precious.txt"), 10);

    // (1) A symlink in the path that leads to the item: refused outright.
    VT_REQUIRE(::symlink(temp.sub("real").c_str(), temp.sub("alias").c_str()) == 0);
    std::string failed;
    const Status viaLink = DeletionEngine::removeTree(temp.sub("alias/dir"), &failed);
    VT_CHECK(!viaLink.ok());
    VT_CHECK_EQ(viaLink.error().code, ELOOP);
    VT_CHECK(vt::exists(temp.sub("real/dir/file.bin")));

    // (2) A symlink given as the item: the link goes, its target stays.
    VT_CHECK(DeletionEngine::removeTree(temp.sub("alias")).ok());
    VT_CHECK(!vt::exists(temp.sub("alias")));
    VT_CHECK(vt::exists(temp.sub("real/dir/file.bin")));

    // (3) Symlinks inside the tree (to a directory, to a file, dangling, looping).
    VT_REQUIRE(::symlink(temp.sub("outside").c_str(), temp.sub("real/dir/to-dir").c_str()) == 0);
    VT_REQUIRE(::symlink(temp.sub("outside/precious.txt").c_str(), temp.sub("real/dir/to-file").c_str()) == 0);
    VT_REQUIRE(::symlink("/nonexistent", temp.sub("real/dir/dangling").c_str()) == 0);
    VT_REQUIRE(::symlink(".", temp.sub("real/dir/loop").c_str()) == 0);
    VT_CHECK(DeletionEngine::removeTree(temp.sub("real")).ok());
    VT_CHECK(!vt::exists(temp.sub("real")));
    VT_CHECK(vt::exists(temp.sub("outside/precious.txt")));

    // (4) Nonsense is refused; something already gone is not an error.
    VT_CHECK(!DeletionEngine::removeTree("/").ok());
    VT_CHECK(!DeletionEngine::removeTree("relative/path").ok());
    VT_CHECK(!DeletionEngine::removeTree("").ok());
    VT_CHECK(DeletionEngine::removeTree(temp.sub("never-existed")).ok());
}

VT_TEST(remove_tree_handles_deep_trees_and_locked_files) {
    const vt::TempDir temp("vortex-rm");
    std::string deep = temp.sub("deep");
    for (int level = 0; level < 150; ++level) deep += "/d" + std::to_string(level);
    vt::writeFileOfSize(deep + "/leaf.bin", 5);
    for (int i = 0; i < 300; ++i) vt::writeFileOfSize(temp.sub("deep/wide/f" + std::to_string(i)), 3);
    VT_CHECK(DeletionEngine::removeTree(temp.sub("deep")).ok());
    VT_CHECK(!vt::exists(temp.sub("deep")));

    // Items carrying the user "locked" flag are removed by an explicit permanent delete.
    vt::writeFileOfSize(temp.sub("locked/dir/file.bin"), 5);
    VT_REQUIRE(::chflags(temp.sub("locked/dir/file.bin").c_str(), UF_IMMUTABLE) == 0);
    VT_REQUIRE(::chflags(temp.sub("locked/dir").c_str(), UF_IMMUTABLE) == 0);
    VT_CHECK(DeletionEngine::removeTree(temp.sub("locked/dir")).ok());
    VT_CHECK(!vt::exists(temp.sub("locked/dir")));

    if (::geteuid() != 0) {
        // A directory we cannot enter: reported with the exact path, the rest still removed.
        vt::writeFileOfSize(temp.sub("mixed/open/a.bin"), 5);
        vt::writeFileOfSize(temp.sub("mixed/sealed/b.bin"), 5);
        VT_REQUIRE(::chmod(temp.sub("mixed/sealed").c_str(), 0000) == 0);
        std::string failed;
        const Status partial = DeletionEngine::removeTree(temp.sub("mixed"), &failed);
        VT_CHECK(!partial.ok());
        VT_CHECK_EQ(partial.error().code, EACCES);
        VT_CHECK_EQ(failed, temp.sub("mixed/sealed"));
        VT_CHECK(!vt::exists(temp.sub("mixed/open")));
        VT_CHECK(vt::exists(temp.sub("mixed/sealed")));
        ::chmod(temp.sub("mixed/sealed").c_str(), 0700);
    }
}

// ---------------------------------------------------------------------------
// Privileged helper
// ---------------------------------------------------------------------------

VT_TEST(privileged_protocol_round_trips) {
    PrivilegedRequest request;
    request.mode = DeleteMode::Permanent;
    request.invokerUid = 501;
    request.items = {item("/Library/LaunchDaemons/com.acme.rocketd.plist", RemnantCategory::LaunchDaemons, "com.acme.rocketd"),
                     item("/Applications/Rocket \"Pro\" \xC3\xA9.app", RemnantCategory::AppBundle)};

    const std::string wire = privileged::encodeRequest(request).dump(-1);
    VT_CHECK(wire.find('\n') == std::string::npos);  // compact: a single line
    auto parsed = JsonValue::parse(wire);
    VT_REQUIRE(parsed.ok());
    auto decoded = privileged::decodeRequest(parsed.value());
    VT_REQUIRE(decoded.ok());
    VT_CHECK(decoded.value().mode == DeleteMode::Permanent);
    VT_CHECK_EQ(decoded.value().invokerUid, static_cast<uid_t>(501));
    VT_REQUIRE(decoded.value().items.size() == 2);
    VT_CHECK_EQ(decoded.value().items[0].path, request.items[0].path);
    VT_CHECK(decoded.value().items[0].category == RemnantCategory::LaunchDaemons);
    VT_CHECK_EQ(decoded.value().items[0].launchdLabel, std::string("com.acme.rocketd"));
    VT_CHECK_EQ(decoded.value().items[1].path, request.items[1].path);

    PrivilegedResponse response;
    response.helperPid = 4242;
    ItemOutcome outcome;
    outcome.item = request.items[0];
    outcome.status = ItemStatus::Trashed;
    outcome.trashPath = "/Users/tester/.Trash/com.acme.rocketd.plist";
    outcome.launchctlTarget = "system/com.acme.rocketd";
    outcome.launchctlRan = true;
    outcome.launchctlExit = 3;
    outcome.note = "service was not loaded";
    response.outcomes.push_back(outcome);
    auto responseJson = JsonValue::parse(privileged::encodeResponse(response).dump(-1));
    VT_REQUIRE(responseJson.ok());
    auto decodedResponse = privileged::decodeResponse(responseJson.value());
    VT_REQUIRE(decodedResponse.ok());
    VT_CHECK_EQ(decodedResponse.value().helperPid, static_cast<pid_t>(4242));
    VT_REQUIRE(decodedResponse.value().outcomes.size() == 1);
    const ItemOutcome& back = decodedResponse.value().outcomes.front();
    VT_CHECK(back.status == ItemStatus::Trashed);
    VT_CHECK_EQ(back.trashPath, outcome.trashPath);
    VT_CHECK_EQ(back.launchctlExit, 3);
    VT_CHECK(back.launchctlRan);
    VT_CHECK(back.elevated);

    // Malformed requests are rejected, never guessed at.
    for (const char* bad : {"[]", "{}", R"({"protocol":2,"mode":"trash","invoker_uid":1,"items":[]})",
                            R"({"protocol":1,"mode":"shred","invoker_uid":1,"items":[]})",
                            R"({"protocol":1,"mode":"trash","invoker_uid":-4,"items":[]})",
                            R"({"protocol":1,"mode":"trash","invoker_uid":1,"items":{}})",
                            R"({"protocol":1,"mode":"trash","invoker_uid":1,"items":[{"path":7}]})",
                            R"({"protocol":1,"mode":"trash","invoker_uid":1,"items":[{"path":"/x","category":"nope"}]})"}) {
        auto json = JsonValue::parse(bad);
        VT_REQUIRE(json.ok());
        if (privileged::decodeRequest(json.value()).ok()) {
            vt::reportFailure(__FILE__, __LINE__, std::string("malformed request accepted: ") + bad);
        }
    }

    // Framing.
    std::FILE* pipe = std::tmpfile();
    VT_REQUIRE(pipe != nullptr);
    VT_CHECK(privileged::writeFrame(pipe, wire).ok());
    VT_CHECK(privileged::writeFrame(pipe, "").ok());
    std::rewind(pipe);
    auto frame = privileged::readFrame(pipe);
    VT_REQUIRE(frame.ok());
    VT_CHECK_EQ(frame.value(), wire);
    auto emptyFrame = privileged::readFrame(pipe);
    VT_REQUIRE(emptyFrame.ok());
    VT_CHECK(emptyFrame.value().empty());
    VT_CHECK(!privileged::readFrame(pipe).ok());  // end of stream
    std::fclose(pipe);

    for (const char* bad : {"abc\npayload", "\npayload", "99999999999999999999999\nx", "10\nshort", "5"}) {
        std::FILE* stream = std::tmpfile();
        VT_REQUIRE(stream != nullptr);
        std::fputs(bad, stream);
        std::rewind(stream);
        if (privileged::readFrame(stream, 1024).ok()) {
            vt::reportFailure(__FILE__, __LINE__, std::string("malformed frame accepted: ") + bad);
        }
        std::fclose(stream);
    }
    std::FILE* oversize = std::tmpfile();
    VT_REQUIRE(oversize != nullptr);
    std::fputs("2048\n", oversize);
    std::rewind(oversize);
    VT_CHECK(!privileged::readFrame(oversize, 1024).ok());
    std::fclose(oversize);
}

VT_TEST(privileged_helper_enforces_its_own_policy) {
    Fixture f;
    privileged::HelperPolicy policy;
    policy.systemRoot = f.root;
    policy.invokerHome = f.home;
    policy.invokerUid = ::getuid();

    VT_CHECK(privileged::isLocationAllowed(policy, f.root + "/Library/LaunchDaemons/x.plist"));
    VT_CHECK(privileged::isLocationAllowed(policy, f.root + "/Applications/Rocket.app"));
    VT_CHECK(privileged::isLocationAllowed(policy, f.root + "/usr/local/bin/tool"));
    VT_CHECK(privileged::isLocationAllowed(policy, f.root + "/private/var/db/receipts/com.acme.bom"));
    VT_CHECK(privileged::isLocationAllowed(policy, f.root + "/var/db/receipts/com.acme.bom"));
    VT_CHECK(privileged::isLocationAllowed(policy, f.home + "/Library/Caches/com.acme.Rocket"));
    VT_CHECK(!privileged::isLocationAllowed(policy, f.root + "/etc/sudoers"));
    VT_CHECK(!privileged::isLocationAllowed(policy, f.root + "/private/var/db/dslocal/x"));
    VT_CHECK(!privileged::isLocationAllowed(policy, f.root + "/Users/other/Library/Caches/x"));
    VT_CHECK(!privileged::isLocationAllowed(policy, f.home + "/Documents/thesis.pdf"));
    VT_CHECK(!privileged::isLocationAllowed(policy, f.root + "/Library"));
    VT_CHECK(!privileged::isLocationAllowed(policy, f.root + "/bin/sh"));
    VT_CHECK(!privileged::isLocationAllowed(policy, "/Library/LaunchDaemons/x.plist"));  // outside the system root

    const std::string daemon = f.systemLibrary + "/LaunchDaemons/com.acme.rocketd.plist";
    const std::string globalAgent = f.systemLibrary + "/LaunchAgents/com.acme.agent.plist";
    const std::string bundle = f.root + "/Applications/Rocket.app";
    const std::string userCache = f.home + "/Library/Caches/com.acme.Rocket";
    const std::string etcFile = f.root + "/etc/secret.conf";
    const std::string otherUser = f.root + "/Users/other/Library/Caches/theirs";
    const std::string documents = f.home + "/Documents/thesis.pdf";
    const std::string sharedDir = f.systemLibrary + "/Application Support";
    vt::writeFile(globalAgent, "x");
    vt::writeFile(etcFile, "x");
    vt::writeFile(otherUser, "x");
    vt::writeFile(documents, "x");
    vt::writeFile(sharedDir + "/Acme/x", "x");

    std::vector<std::string> booted;
    privileged::HelperHooks hooks;
    hooks.bootout = [&](const std::string& target) {
        booted.push_back(target);
        LaunchctlResult result;
        result.spawned = true;
        result.exitStatus = 3;
        result.detail = "service was not loaded";
        return result;
    };

    PrivilegedRequest request;
    request.mode = DeleteMode::Permanent;
    request.invokerUid = ::getuid();
    request.items = {
        item(daemon, RemnantCategory::LaunchDaemons, "com.acme.rocketd"),
        item(globalAgent, RemnantCategory::LaunchAgents, "com.acme.agent"),
        item(bundle, RemnantCategory::AppBundle),
        item(userCache),
        item(etcFile),
        item(otherUser),
        item(documents),
        item("/System/Library/CoreServices/Finder.app", RemnantCategory::AppBundle),
        item(sharedDir),
        item(f.systemLibrary + "/LaunchDaemons/already-gone.plist", RemnantCategory::LaunchDaemons, "x"),
        item("relative/path"),
        item(f.systemLibrary + "/LaunchDaemons/../../etc/secret.conf"),
    };
    const std::vector<ItemOutcome> outcomes = privileged::execute(request, policy, hooks);
    VT_REQUIRE(outcomes.size() == request.items.size());

    VT_CHECK(outcomes[0].status == ItemStatus::Deleted);
    VT_CHECK(!vt::exists(daemon));
    VT_CHECK_EQ(outcomes[0].launchctlTarget, std::string("system/com.acme.rocketd"));
    VT_CHECK(outcomes[1].status == ItemStatus::Deleted);
    VT_CHECK_EQ(outcomes[1].launchctlTarget, "gui/" + std::to_string(::getuid()) + "/com.acme.agent");
    VT_CHECK_EQ(booted.size(), std::size_t{2});
    VT_CHECK(outcomes[2].status == ItemStatus::Deleted);
    VT_CHECK(!vt::exists(bundle));
    VT_CHECK(outcomes[3].status == ItemStatus::Deleted);
    VT_CHECK(!vt::exists(userCache));

    // Everything outside the allow-list is refused and left exactly as it was.
    for (const std::size_t index : {std::size_t{4}, std::size_t{5}, std::size_t{6}}) {
        VT_CHECK(outcomes[index].status == ItemStatus::Error);
        VT_CHECK_EQ(outcomes[index].errorCode, EPERM);
        VT_CHECK(vt::exists(request.items[index].path));
    }
    VT_CHECK(outcomes[7].status == ItemStatus::SkippedSip);
    VT_CHECK(outcomes[8].status == ItemStatus::Error);  // a shared directory
    VT_CHECK(vt::exists(sharedDir + "/Acme/x"));
    VT_CHECK(outcomes[9].status == ItemStatus::SkippedMissing);
    VT_CHECK(outcomes[10].status == ItemStatus::Error);
    VT_CHECK(outcomes[11].status == ItemStatus::Error);  // ".." is never normalised away silently
    VT_CHECK_EQ(outcomes[11].errorCode, EINVAL);
    VT_CHECK(vt::exists(etcFile));
    for (const ItemOutcome& outcome : outcomes) VT_CHECK(outcome.elevated);
}

VT_TEST(privileged_helper_moves_items_to_the_invokers_trash) {
    Fixture f;
    privileged::HelperPolicy policy;
    policy.systemRoot = f.root;
    policy.invokerHome = f.home;
    policy.invokerUid = ::getuid();
    const std::string first = f.root + "/Applications/Rocket.app";
    const std::string second = f.root + "/Applications/Suite/Rocket.app";
    const std::string third = f.systemLibrary + "/LaunchDaemons/com.acme.rocketd.plist";
    vt::writeFileOfSize(second + "/Contents/MacOS/Rocket", 10);

    PrivilegedRequest request;
    request.mode = DeleteMode::Trash;
    request.invokerUid = ::getuid();
    request.items = {item(first, RemnantCategory::AppBundle), item(second, RemnantCategory::AppBundle),
                     item(third, RemnantCategory::PackagePayload)};
    VT_CHECK(!vt::exists(f.home + "/.Trash"));
    const std::vector<ItemOutcome> outcomes = privileged::execute(request, policy);
    VT_REQUIRE(outcomes.size() == 3);

    VT_CHECK(outcomes[0].status == ItemStatus::Trashed);
    VT_CHECK_EQ(outcomes[0].trashPath, f.home + "/.Trash/Rocket.app");
    VT_CHECK(vt::exists(f.home + "/.Trash/Rocket.app/Contents/MacOS/Rocket"));
    VT_CHECK(!vt::exists(first));
    // A second item of the same name never replaces the first one.
    VT_CHECK(outcomes[1].status == ItemStatus::Trashed);
    VT_CHECK_EQ(outcomes[1].trashPath, f.home + "/.Trash/Rocket 2.app");
    VT_CHECK(vt::exists(f.home + "/.Trash/Rocket 2.app/Contents/MacOS/Rocket"));
    VT_CHECK(outcomes[2].status == ItemStatus::Trashed);
    VT_CHECK(vt::exists(f.home + "/.Trash/com.acme.rocketd.plist"));

    struct stat trash {};
    VT_REQUIRE(::lstat((f.home + "/.Trash").c_str(), &trash) == 0);
    VT_CHECK(S_ISDIR(trash.st_mode));
    VT_CHECK_EQ(trash.st_mode & 0777, static_cast<mode_t>(0700));

    // A Trash that is really a symlink somewhere else is refused; the item stays put.
    Fixture g;
    privileged::HelperPolicy redirected;
    redirected.systemRoot = g.root;
    redirected.invokerHome = g.home;
    redirected.invokerUid = ::getuid();
    vt::makeDirs(g.root + "/elsewhere");
    VT_REQUIRE(::symlink((g.root + "/elsewhere").c_str(), (g.home + "/.Trash").c_str()) == 0);
    PrivilegedRequest again;
    again.mode = DeleteMode::Trash;
    again.invokerUid = ::getuid();
    again.items = {item(g.root + "/Applications/Rocket.app", RemnantCategory::AppBundle)};
    const std::vector<ItemOutcome> refused = privileged::execute(again, redirected);
    VT_REQUIRE(refused.size() == 1);
    VT_CHECK(refused[0].status == ItemStatus::Error || refused[0].status == ItemStatus::SkippedPermission);
    VT_CHECK(vt::exists(g.root + "/Applications/Rocket.app"));
    VT_CHECK(vt::snapshotTree(g.root + "/elsewhere").empty());
}

VT_TEST(privileged_helper_refuses_to_run_without_root) {
    if (::geteuid() == 0) {
        vt::note("running as root: the refusal path cannot be exercised");
        return;
    }
    std::FILE* in = std::tmpfile();
    std::FILE* out = std::tmpfile();
    VT_REQUIRE(in != nullptr && out != nullptr);
    PrivilegedRequest request;
    request.mode = DeleteMode::Permanent;
    request.invokerUid = ::getuid();
    request.items = {item("/Library/LaunchDaemons/com.acme.rocketd.plist", RemnantCategory::LaunchDaemons)};
    VT_REQUIRE(privileged::writeFrame(in, privileged::encodeRequest(request).dump(-1)).ok());
    std::rewind(in);

    VT_CHECK(privileged::runHelperMain(in, out) != 0);
    std::rewind(out);
    auto frame = privileged::readFrame(out);
    VT_REQUIRE(frame.ok());
    auto json = JsonValue::parse(frame.value());
    VT_REQUIRE(json.ok());
    auto response = privileged::decodeResponse(json.value());
    VT_REQUIRE(response.ok());
    VT_CHECK(!response.value().error.empty());
    VT_CHECK(response.value().outcomes.empty());
    VT_CHECK_EQ(response.value().helperPid, ::getpid());
    std::fclose(in);
    std::fclose(out);
}
