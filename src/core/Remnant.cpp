#include "Remnant.hpp"

namespace vortex {

const char* displayName(RemnantCategory category) noexcept {
    switch (category) {
        case RemnantCategory::AppBundle: return "Application";
        case RemnantCategory::ApplicationSupport: return "Application Support";
        case RemnantCategory::Caches: return "Caches";
        case RemnantCategory::Preferences: return "Preferences";
        case RemnantCategory::Containers: return "Containers";
        case RemnantCategory::GroupContainers: return "Group Containers";
        case RemnantCategory::SavedState: return "Saved Application State";
        case RemnantCategory::Logs: return "Logs";
        case RemnantCategory::WebKit: return "WebKit";
        case RemnantCategory::HTTPStorages: return "HTTP Storages";
        case RemnantCategory::Cookies: return "Cookies";
        case RemnantCategory::ApplicationScripts: return "Application Scripts";
        case RemnantCategory::LaunchAgents: return "Launch Agents";
        case RemnantCategory::LaunchDaemons: return "Launch Daemons";
        case RemnantCategory::PrivilegedHelperTools: return "Privileged Helper Tools";
        case RemnantCategory::Receipts: return "Package Receipts";
        case RemnantCategory::PackagePayload: return "Package Payload";
        case RemnantCategory::QuickLook: return "Quick Look Plug-ins";
        case RemnantCategory::PreferencePanes: return "Preference Panes";
        case RemnantCategory::Frameworks: return "Frameworks";
    }
    return "Other";
}

const char* jsonKey(RemnantCategory category) noexcept {
    switch (category) {
        case RemnantCategory::AppBundle: return "app_bundle";
        case RemnantCategory::ApplicationSupport: return "application_support";
        case RemnantCategory::Caches: return "caches";
        case RemnantCategory::Preferences: return "preferences";
        case RemnantCategory::Containers: return "containers";
        case RemnantCategory::GroupContainers: return "group_containers";
        case RemnantCategory::SavedState: return "saved_application_state";
        case RemnantCategory::Logs: return "logs";
        case RemnantCategory::WebKit: return "webkit";
        case RemnantCategory::HTTPStorages: return "http_storages";
        case RemnantCategory::Cookies: return "cookies";
        case RemnantCategory::ApplicationScripts: return "application_scripts";
        case RemnantCategory::LaunchAgents: return "launch_agents";
        case RemnantCategory::LaunchDaemons: return "launch_daemons";
        case RemnantCategory::PrivilegedHelperTools: return "privileged_helper_tools";
        case RemnantCategory::Receipts: return "receipts";
        case RemnantCategory::PackagePayload: return "package_payload";
        case RemnantCategory::QuickLook: return "quicklook";
        case RemnantCategory::PreferencePanes: return "preference_panes";
        case RemnantCategory::Frameworks: return "frameworks";
    }
    return "other";
}

const char* displayName(Confidence confidence) noexcept {
    switch (confidence) {
        case Confidence::Exact: return "Exact";
        case Confidence::High: return "High";
        case Confidence::Medium: return "Medium";
    }
    return "Medium";
}

const char* jsonKey(Confidence confidence) noexcept {
    switch (confidence) {
        case Confidence::Exact: return "exact";
        case Confidence::High: return "high";
        case Confidence::Medium: return "medium";
    }
    return "medium";
}

const char* displayName(MatchReason reason) noexcept {
    switch (reason) {
        case MatchReason::TargetBundle: return "Application Bundle";
        case MatchReason::ExactBundleId: return "Exact Bundle ID";
        case MatchReason::BundleIdPrefix: return "Bundle ID Prefix";
        case MatchReason::EmbeddedHelperId: return "Embedded Helper ID";
        case MatchReason::HelperLabel: return "Declared Helper Label";
        case MatchReason::EntitledAppGroup: return "Entitled App Group";
        case MatchReason::TeamIdGroupContainer: return "TeamID Group Container";
        case MatchReason::ContainerMetadata: return "Container Metadata";
        case MatchReason::LaunchdProgramInBundle: return "Launchd Job in Bundle";
        case MatchReason::LaunchdAssociatedBundleId: return "Launchd Associated ID";
        case MatchReason::LaunchdProgramFromReceipt: return "Launchd Job from Receipt";
        case MatchReason::LaunchdJobProgram: return "Launchd Job Program";
        case MatchReason::BomReceipt: return "BOM Receipt";
        case MatchReason::ReceiptPackageId: return "Receipt Package ID";
        case MatchReason::AppNameMatch: return "App Name Heuristic";
        case MatchReason::ExecutableNameHeuristic: return "Executable Name Heuristic";
        case MatchReason::BundleIdNameHeuristic: return "Bundle ID Name Heuristic";
        case MatchReason::VendorDirectory: return "Vendor Directory";
    }
    return "Unknown";
}

const char* jsonKey(MatchReason reason) noexcept {
    switch (reason) {
        case MatchReason::TargetBundle: return "target_bundle";
        case MatchReason::ExactBundleId: return "exact_bundle_id";
        case MatchReason::BundleIdPrefix: return "bundle_id_prefix";
        case MatchReason::EmbeddedHelperId: return "embedded_helper_id";
        case MatchReason::HelperLabel: return "declared_helper_label";
        case MatchReason::EntitledAppGroup: return "entitled_app_group";
        case MatchReason::TeamIdGroupContainer: return "team_id_group_container";
        case MatchReason::ContainerMetadata: return "container_metadata";
        case MatchReason::LaunchdProgramInBundle: return "launchd_program_in_bundle";
        case MatchReason::LaunchdAssociatedBundleId: return "launchd_associated_bundle_id";
        case MatchReason::LaunchdProgramFromReceipt: return "launchd_program_from_receipt";
        case MatchReason::LaunchdJobProgram: return "launchd_job_program";
        case MatchReason::BomReceipt: return "bom_receipt";
        case MatchReason::ReceiptPackageId: return "receipt_package_id";
        case MatchReason::AppNameMatch: return "app_name_heuristic";
        case MatchReason::ExecutableNameHeuristic: return "executable_name_heuristic";
        case MatchReason::BundleIdNameHeuristic: return "bundle_id_name_heuristic";
        case MatchReason::VendorDirectory: return "vendor_directory";
    }
    return "unknown";
}

const char* jsonKey(Domain domain) noexcept {
    return domain == Domain::User ? "user" : "system";
}

}  // namespace vortex
