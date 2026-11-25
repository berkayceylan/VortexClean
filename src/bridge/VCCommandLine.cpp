#include "VCCommandLine.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "BundleInspector.hpp"
#include "DeletionEngine.hpp"
#include "DiskAnalyzer.hpp"
#include "Json.hpp"
#include "MatchEngine.hpp"
#include "PathUtil.hpp"
#include "ProcessInspector.hpp"
#include "RemnantScanner.hpp"
#include "ThreadPool.hpp"
#include "Version.hpp"

namespace vcbridge {

namespace {

void printUsage() {
    std::printf(
        "%s %s — native macOS deep application uninstaller\n\n"
        "Usage:\n"
        "  VortexClean                      start the application\n"
        "  VortexClean <path.app>           start and analyze an application\n"
        "  VortexClean --launch-check       start, scan one application, print a summary, quit\n"
        "      [path.app]                   ... scan this application instead of the smallest one\n"
        "      [--empty]                    ... or select nothing (empty state)\n"
        "      [--snapshot out.png]         ... and render the window into a PNG first\n"
        "      [--hover <n>]                ... with the pointer over segment n of the storage bar\n"
        "      [--appearance dark|light]    force an appearance (also without --launch-check)\n\n"
        "Any of these also accepts, before everything else:\n"
        "  VortexClean --root <dir> ...     work on a synthetic disk instead of the real one:\n"
        "                                   <dir> is \"/\" and <dir>/Users/demo the home folder\n\n"
        "Headless commands (read-only, JSON on standard output):\n"
        "  VortexClean --version\n"
        "  VortexClean --list [--sizes]     installed applications\n"
        "  VortexClean --scan <path.app>    everything that belongs to an application\n"
        "  VortexClean --dry-run <path.app> simulate removing the default selection and\n"
        "                                   write an audit report; nothing is touched\n",
        vortex::kProductName, vortex::kProductVersion);
}

/// Inspects every bundle in the default locations, in parallel.
std::vector<vortex::AppInfo> discoverApplications(vortex::ThreadPool& pool, const vortex::ScanRoots& roots, bool withSizes) {
    const std::vector<std::string> paths =
        vortex::BundleInspector::discoverBundles(
        vortex::BundleInspector::defaultSearchRoots(roots.homeDir, roots.installRoot));
    std::vector<vortex::AppInfo> inspected(paths.size());
    std::vector<char> succeeded(paths.size(), 0);
    {
        vortex::TaskGroup group(pool);
        for (std::size_t i = 0; i < paths.size(); ++i) {
            group.run([&paths, &inspected, &succeeded, withSizes, i] {
                auto result = vortex::BundleInspector::inspect(paths[i]);
                if (!result) return;
                inspected[i] = std::move(result.value());
                if (withSizes) {
                    inspected[i].usage = vortex::DiskAnalyzer::measure(inspected[i].bundlePath);
                    inspected[i].usageKnown = true;
                }
                succeeded[i] = 1;
            });
        }
        group.wait();
    }
    std::vector<vortex::AppInfo> apps;
    for (std::size_t i = 0; i < inspected.size(); ++i) {
        if (succeeded[i] != 0) apps.push_back(std::move(inspected[i]));
    }
    return apps;
}

vortex::JsonValue describeApp(const vortex::AppInfo& app) {
    vortex::JsonValue json = vortex::JsonValue::makeObject();
    json.set("name", app.displayName);
    json.set("bundle_id", app.bundleId);
    json.set("path", app.bundlePath);
    json.set("version", app.shortVersion);
    json.set("team_id", app.signing.teamIdentifier);
    json.set("signing", app.signing.summary());
    json.set("apple_app", app.isAppleApp);
    json.set("sip_protected", app.isSipProtected);
    json.set("app_store", app.isAppStore);
    if (app.usageKnown) {
        json.set("allocated_bytes", app.usage.allocatedBytes);
        json.set("logical_bytes", app.usage.logicalBytes);
    }
    if (app.lastUsedEpoch > 0) json.set("last_used_epoch", app.lastUsedEpoch);
    return json;
}

int commandList(const vortex::ScanRoots& roots, bool withSizes) {
    vortex::ThreadPool pool;
    const std::vector<vortex::AppInfo> apps = discoverApplications(pool, roots, withSizes);
    vortex::JsonValue list = vortex::JsonValue::makeArray();
    for (const vortex::AppInfo& app : apps) list.push(describeApp(app));
    vortex::JsonValue json = vortex::JsonValue::makeObject();
    json.set("count", apps.size());
    json.set("applications", std::move(list));
    std::fputs(json.dump(2).c_str(), stdout);
    return 0;
}

/// Scans one application the same way the interface does.
bool scanApplication(const std::string& bundlePath, const vortex::ScanRoots& roots, vortex::ThreadPool& pool,
                     vortex::ScanResult& result) {
    auto target = vortex::BundleInspector::inspect(bundlePath);
    if (!target) {
        std::fprintf(stderr, "VortexClean: %s\n", target.error().message.c_str());
        return false;
    }
    std::vector<vortex::AppInfo> installed = discoverApplications(pool, roots, false);
    // Known to the cross-checks, never offered: system apps and installed helper apps.
    for (vortex::AppInfo& auxiliary : vortex::BundleInspector::inspectAuxiliaryApplications({}, roots.installRoot)) {
        installed.push_back(std::move(auxiliary));
    }
    bool listed = false;
    for (const vortex::AppInfo& app : installed) {
        if (app.bundlePath == target.value().bundlePath) listed = true;
    }
    if (!listed) installed.push_back(target.value());
    const vortex::InstalledAppIndex index(installed);
    const vortex::RemnantScanner scanner(roots, pool);
    result = scanner.scan(target.value(), index, vortex::CancellationToken{});
    return true;
}

int commandScan(const std::string& bundlePath, const vortex::ScanRoots& roots) {
    vortex::ThreadPool pool;
    vortex::ScanResult result;
    if (!scanApplication(bundlePath, roots, pool, result)) return 1;

    vortex::JsonValue items = vortex::JsonValue::makeArray();
    std::uint64_t total = 0;
    std::uint64_t selected = 0;
    for (const vortex::Remnant& remnant : result.remnants) {
        vortex::JsonValue item = vortex::JsonValue::makeObject();
        item.set("path", remnant.path);
        item.set("display_path", roots.displayPath(remnant.path));
        item.set("category", vortex::jsonKey(remnant.category));
        item.set("confidence", vortex::jsonKey(remnant.confidence));
        item.set("reason", vortex::displayName(remnant.reason));
        item.set("domain", vortex::jsonKey(remnant.domain));
        item.set("allocated_bytes", remnant.usage.allocatedBytes);
        item.set("logical_bytes", remnant.usage.logicalBytes);
        item.set("selected_by_default", remnant.defaultSelected);
        item.set("shared", remnant.isShared);
        if (!remnant.sharedWith.empty()) {
            vortex::JsonValue sharedWith = vortex::JsonValue::makeArray();
            for (const std::string& name : remnant.sharedWith) sharedWith.push(name);
            item.set("shared_with", std::move(sharedWith));
        }
        item.set("sip_protected", remnant.sipProtected);
        item.set("requires_root", remnant.requiresRoot);
        item.set("permission_denied", remnant.permissionDenied);
        if (!remnant.warning.empty()) item.set("warning", remnant.warning);
        if (!remnant.launchdLabel.empty()) item.set("launchd_label", remnant.launchdLabel);
        if (!remnant.receiptPackageId.empty()) item.set("package_id", remnant.receiptPackageId);
        items.push(std::move(item));
        total += remnant.usage.allocatedBytes;
        if (remnant.defaultSelected) selected += remnant.usage.allocatedBytes;
    }

    vortex::JsonValue processes = vortex::JsonValue::makeArray();
    for (const vortex::RunningProcess& process : vortex::ProcessInspector::findRelated(result.app)) {
        vortex::JsonValue entry = vortex::JsonValue::makeObject();
        entry.set("pid", static_cast<std::int64_t>(process.pid));
        entry.set("path", process.executablePath);
        entry.set("main_executable", process.isMainExecutable);
        processes.push(std::move(entry));
    }

    vortex::JsonValue notes = vortex::JsonValue::makeArray();
    for (const std::string& note : result.notes) notes.push(note);
    vortex::JsonValue packages = vortex::JsonValue::makeArray();
    for (const std::string& packageId : result.matchedPackageIds) packages.push(packageId);

    vortex::JsonValue json = vortex::JsonValue::makeObject();
    json.set("application", describeApp(result.app));
    json.set("items", std::move(items));
    json.set("item_count", result.remnants.size());
    json.set("allocated_bytes_found", total);
    json.set("allocated_bytes_selected_by_default", selected);
    json.set("matched_packages", std::move(packages));
    json.set("running_processes", std::move(processes));
    json.set("full_disk_access_needed", result.permissionDenied);
    vortex::JsonValue denied = vortex::JsonValue::makeArray();
    for (const std::string& path : result.deniedPaths) denied.push(path);
    json.set("unreadable_paths", std::move(denied));
    json.set("notes", std::move(notes));
    json.set("scan_seconds", result.elapsedSeconds);
    std::fputs(json.dump(2).c_str(), stdout);
    return 0;
}

int commandDryRun(const std::string& bundlePath, const vortex::ScanRoots& roots) {
    vortex::ThreadPool pool;
    vortex::ScanResult result;
    if (!scanApplication(bundlePath, roots, pool, result)) return 1;

    vortex::DeletionRequest request;
    request.target = result.app;
    request.mode = vortex::DeleteMode::Trash;
    request.dryRun = true;  // the only mode this command ever uses
    for (const vortex::Remnant& remnant : result.remnants) {
        if (remnant.defaultSelected) request.items.push_back(vortex::DeletionItem::fromRemnant(remnant));
    }
    vortex::DeletionEngine::Config config;
    config.homeDirectory = roots.homeDir;
    config.systemRoot = roots.installRoot;
    const vortex::DeletionEngine engine(config, vortex::DeletionHooks{});
    const vortex::DeletionReport report = engine.execute(request);

    vortex::JsonValue json = vortex::JsonValue::makeObject();
    json.set("application", result.app.displayName);
    json.set("dry_run", true);
    json.set("items", report.outcomes.size());
    json.set("would_remove", report.countOf(vortex::ItemStatus::DryRun));
    json.set("skipped_sip", report.countOf(vortex::ItemStatus::SkippedSip));
    json.set("skipped_permission", report.countOf(vortex::ItemStatus::SkippedPermission));
    json.set("errors", report.countOf(vortex::ItemStatus::Error));
    json.set("bytes_reclaimable", report.reclaimedBytes);
    json.set("audit_log", report.auditLogPath);
    if (!report.auditLogError.empty()) json.set("audit_log_error", report.auditLogError);
    std::fputs(json.dump(2).c_str(), stdout);
    return report.auditLogError.empty() ? 0 : 1;
}

}  // namespace

bool runCommandLine(int argc, const char* const* argv, const vortex::ScanRoots& roots, int& exitStatus) {
    if (argc < 2) return false;
    const std::string_view command = argv[1];
    const auto operand = [argc, argv]() -> std::string { return argc >= 3 ? std::string(argv[2]) : std::string(); };

    if (command == "--version" || command == "-v") {
        std::printf("%s %s\n", vortex::kProductName, vortex::kProductVersion);
        exitStatus = 0;
        return true;
    }
    if (command == "--help" || command == "-h") {
        printUsage();
        exitStatus = 0;
        return true;
    }
    if (command == "--list") {
        exitStatus = commandList(roots, argc >= 3 && std::string_view(argv[2]) == "--sizes");
        return true;
    }
    if (command == "--scan" || command == "--dry-run") {
        const std::string path = operand();
        if (path.empty()) {
            std::fprintf(stderr, "VortexClean: %.*s needs the path of an application bundle\n",
                         static_cast<int>(command.size()), command.data());
            exitStatus = 2;
            return true;
        }
        exitStatus = command == "--scan" ? commandScan(path, roots) : commandDryRun(path, roots);
        return true;
    }
    return false;
}

}  // namespace vcbridge
