#include "AuditLogger.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <sys/sysctl.h>
#include <unistd.h>

#include "FileUtil.hpp"
#include "PathUtil.hpp"
#include "Version.hpp"

namespace vortex {

namespace {

std::string operatingSystemVersion() {
    std::array<char, 64> buffer{};
    std::size_t size = buffer.size();
    if (::sysctlbyname("kern.osproductversion", buffer.data(), &size, nullptr, 0) != 0 || size == 0) return {};
    return std::string(buffer.data());
}

const char* architecture() noexcept {
#if defined(__arm64__) || defined(__aarch64__)
    return "arm64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

JsonValue describeTarget(const AppInfo& app) {
    JsonValue target = JsonValue::makeObject();
    target.set("name", app.displayName);
    target.set("bundle_id", app.bundleId);
    target.set("bundle_path", app.bundlePath);
    target.set("version", app.shortVersion);
    target.set("build", app.bundleVersion);
    target.set("executable", app.executableName);
    target.set("team_id", app.signing.teamIdentifier);
    target.set("signing", app.signing.summary());
    target.set("apple_app", app.isAppleApp);
    target.set("app_store", app.isAppStore);
    target.set("sip_protected", app.isSipProtected);
    return target;
}

JsonValue describeItem(const ItemOutcome& outcome) {
    JsonValue item = JsonValue::makeObject();
    item.set("path", outcome.item.path);
    item.set("category", jsonKey(outcome.item.category));
    item.set("confidence", jsonKey(outcome.item.confidence));
    item.set("reason", jsonKey(outcome.item.reason));
    item.set("status", jsonKey(outcome.status));
    item.set("size_bytes", outcome.allocatedBytes);
    item.set("logical_bytes", outcome.logicalBytes);
    item.set("requires_root", outcome.requiresRoot);
    item.set("elevated", outcome.elevated);
    if (!outcome.trashPath.empty()) item.set("trash_path", outcome.trashPath);

    if (outcome.errorCode != 0 || !outcome.errorMessage.empty()) {
        JsonValue error = JsonValue::makeObject();
        error.set("code", outcome.errorCode);
        error.set("message", outcome.errorMessage);
        item.set("error", std::move(error));
    } else {
        item.set("error", nullptr);
    }

    if (!outcome.launchctlTarget.empty()) {
        JsonValue launchd = JsonValue::makeObject();
        launchd.set("label", outcome.item.launchdLabel);
        launchd.set("service_target", outcome.launchctlTarget);
        launchd.set("bootout_ran", outcome.launchctlRan);
        if (outcome.launchctlRan) launchd.set("bootout_exit_status", outcome.launchctlExit);
        item.set("launchd", std::move(launchd));
    }
    if (!outcome.note.empty()) item.set("note", outcome.note);
    return item;
}

}  // namespace

std::string AuditLogger::iso8601(std::int64_t epochMs) {
    const auto seconds = static_cast<std::time_t>(epochMs / 1000);
    const int millis = static_cast<int>(((epochMs % 1000) + 1000) % 1000);
    struct tm utc {};
    ::gmtime_r(&seconds, &utc);
    std::array<char, 40> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
    return std::string(buffer.data());
}

std::string AuditLogger::fileStamp(std::int64_t epochMs) {
    const auto seconds = static_cast<std::time_t>(epochMs / 1000);
    const int millis = static_cast<int>(((epochMs % 1000) + 1000) % 1000);
    struct tm local {};
    ::localtime_r(&seconds, &local);
    std::array<char, 40> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04d%02d%02d-%02d%02d%02d-%03d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, millis);
    return std::string(buffer.data());
}

JsonValue AuditLogger::buildDocument(const DeletionRequest& request, const DeletionReport& report) {
    JsonValue document = JsonValue::makeObject();
    document.set("schema", "vortexclean.audit/1");

    JsonValue generator = JsonValue::makeObject();
    generator.set("name", kProductName);
    generator.set("version", kProductVersion);
    document.set("generator", std::move(generator));

    document.set("started_at", iso8601(report.startedEpochMs));
    document.set("finished_at", iso8601(report.finishedEpochMs));
    document.set("duration_ms", report.finishedEpochMs - report.startedEpochMs);

    JsonValue host = JsonValue::makeObject();
    host.set("os_version", operatingSystemVersion());
    host.set("architecture", architecture());
    host.set("user_id", static_cast<std::int64_t>(::getuid()));
    document.set("host", std::move(host));

    JsonValue operation = JsonValue::makeObject();
    operation.set("mode", jsonKey(report.mode));
    operation.set("dry_run", report.dryRun);
    operation.set("cancelled", report.cancelled);
    operation.set("elevation_attempted", report.elevationAttempted);
    operation.set("elevation_authorized", report.elevationAuthorized);
    if (!report.elevationError.empty()) operation.set("elevation_error", report.elevationError);
    operation.set("privacy_denied", report.privacyDenied);
    document.set("operation", std::move(operation));

    document.set("target", describeTarget(request.target));

    JsonValue summary = JsonValue::makeObject();
    summary.set("attempted", report.outcomes.size());
    for (const ItemStatus status :
         {ItemStatus::Trashed, ItemStatus::Deleted, ItemStatus::SkippedSip, ItemStatus::SkippedPermission,
          ItemStatus::DryRun, ItemStatus::Error, ItemStatus::SkippedMissing}) {
        summary.set(jsonKey(status), report.countOf(status));
    }
    // A simulation frees nothing; it reports what a real run would free.
    summary.set("bytes_reclaimed", report.dryRun ? std::uint64_t{0} : report.reclaimedBytes);
    summary.set("bytes_reclaimable", report.dryRun ? report.reclaimedBytes : std::uint64_t{0});
    document.set("summary", std::move(summary));

    JsonValue items = JsonValue::makeArray();
    for (const ItemOutcome& outcome : report.outcomes) items.push(describeItem(outcome));
    document.set("items", std::move(items));
    return document;
}

Result<std::string> AuditLogger::write(const DeletionRequest& request, const DeletionReport& report,
                                       const std::string& directory) {
    if (directory.empty() || directory.front() != '/') {
        return Error{EINVAL, "audit directory must be an absolute path"};
    }
    if (const Status made = io::makeDirectories(directory, 0755); !made) return made.error();

    const std::string text = buildDocument(request, report).dump(2);
    const std::string base = "audit-" + fileStamp(report.startedEpochMs);
    // Exclusive creation: two runs in the same millisecond get distinct files.
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::string name = attempt == 0 ? base + ".json" : base + "-" + std::to_string(attempt + 1) + ".json";
        const std::string full = path::join(directory, name);
        const Status written = io::writeFileExclusive(full, text, 0644);
        if (written) return full;
        if (written.error().code != EEXIST) return written.error();
    }
    return Error{EEXIST, "could not find a free audit file name in '" + directory + "'"};
}

}  // namespace vortex
