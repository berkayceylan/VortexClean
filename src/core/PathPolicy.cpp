#include "PathPolicy.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "PathUtil.hpp"
#include "StringUtil.hpp"

namespace vortex {

namespace {

/// Absolute directories that are infrastructure, never an application's property.
constexpr auto kFixedRoots = std::to_array<std::string_view>({
    "/",
    "/Applications",
    "/Applications/Utilities",
    "/Library",
    "/System",
    "/Users",
    "/Users/Shared",
    "/Volumes",
    "/Network",
    "/cores",
    "/dev",
    "/home",
    "/net",
    "/bin",
    "/sbin",
    "/opt",
    "/opt/homebrew",
    "/opt/homebrew/bin",
    "/opt/homebrew/lib",
    "/opt/homebrew/share",
    "/opt/homebrew/etc",
    "/opt/homebrew/opt",
    "/opt/homebrew/var",
    "/opt/homebrew/Cellar",
    "/opt/homebrew/Caskroom",
    "/opt/local",
    "/usr",
    "/usr/local",
    "/usr/local/bin",
    "/usr/local/sbin",
    "/usr/local/lib",
    "/usr/local/libexec",
    "/usr/local/include",
    "/usr/local/share",
    "/usr/local/share/man",
    "/usr/local/etc",
    "/usr/local/var",
    "/usr/local/opt",
    "/usr/local/man",
    "/usr/local/Cellar",
    "/usr/local/Caskroom",
    "/usr/local/Frameworks",
    "/private",
    "/private/etc",
    "/private/tmp",
    "/private/var",
    "/private/var/db",
    "/private/var/db/receipts",
    "/private/var/folders",
    "/private/var/log",
    "/private/var/root",
    "/private/var/run",
    "/private/var/tmp",
    "/private/var/vm",
    "/private/var/containers",
    "/Library/Application Support/Apple",
    "/Library/Apple",
    "/Library/Developer",
});

/// Standard children of a user's home directory.
constexpr auto kHomeChildren = std::to_array<std::string_view>({
    "Library", "Desktop", "Documents", "Downloads", "Movies", "Music",     "Pictures",
    "Public",  "Applications", "Sites", ".Trash", ".config", ".local", ".cache",
});

/// Well-known first-level entries of a Library folder (/Library and ~/Library).
constexpr auto kLibraryChildren = std::to_array<std::string_view>({
    "Accessibility",          "Accounts",              "Address Book Plug-Ins", "Application Scripts",
    "Application Support",    "Assistant",             "Assistants",            "Audio",
    "Autosave Information",   "Biome",                 "Bluetooth",             "Caches",
    "Calendars",              "CallServices",          "CloudStorage",          "ColorPickers",
    "ColorSync",              "Colors",                "Components",            "Compositions",
    "Contextual Menu Items",  "ContainerManager",      "Containers",            "Cookies",
    "CoreAnalytics",          "CoreFollowUp",          "CoreMediaIO",           "Daemon Containers",
    "DataAccess",             "DataDeliveryServices",  "DBStore",               "Developer",
    "DirectoryServices",      "Documentation",         "DriverExtensions",      "DuetExpertCenter",
    "Extensions",             "Favorites",             "Filesystems",           "Finance",
    "FontCollections",        "Fonts",                 "Frameworks",            "FrontBoard",
    "GameKit",                "GPUBundles",            "Graphics",              "Group Containers",
    "HTTPStorages",           "HomeKit",               "IdentityServices",      "Image Capture",
    "Input Methods",          "IntelligencePlatform",  "Internet Accounts",     "Internet Plug-Ins",
    "Java",                   "KernelCollections",     "Keyboard",              "Keyboard Layouts",
    "KeyboardServices",       "Keychains",             "LanguageModeling",      "LaunchAgents",
    "LaunchDaemons",          "LockdownMode",          "Logs",                  "Mail",
    "Managed Preferences",    "Maps",                  "Messages",              "Metadata",
    "Mobile Documents",       "Modem Scripts",         "News",                  "OpenDirectory",
    "OSAnalytics",            "Passes",                "PDF Services",          "Perl",
    "PersonalizationPortrait", "Photos",               "PPM",                   "PreferencePanes",
    "Preferences",            "Printers",              "PrivilegedHelperTools", "Python",
    "QuickLook",              "QuickTime",             "Receipts",              "Reminders",
    "ResponseKit",            "Ruby",                  "Safari",                "SafariSafeBrowsing",
    "Sandbox",                "Saved Application State", "Screen Savers",       "ScriptingAdditions",
    "Scripts",                "Security",              "Services",              "Sharing",
    "Shortcuts",              "Sounds",                "Speech",                "Spelling",
    "Spotlight",              "StagedDriverExtensions", "StagedExtensions",     "StartupItems",
    "StatusKit",              "Suggestions",           "SyncedPreferences",     "SystemExtensions",
    "SystemMigration",        "SystemProfiler",        "Translation",           "Trial",
    "Updates",                "User Pictures",         "User Template",         "WebKit",
    "WebServer",
});

/// Deeper Library entries that are shared collection points rather than app property.
constexpr auto kLibraryDeepRoots = std::to_array<std::string_view>({
    "Preferences/ByHost",
    "Logs/DiagnosticReports",
    "Logs/CrashReporter",
    "Application Support/CrashReporter",
    "Application Support/com.apple.sharedfilelist",
    "Application Support/com.apple.sharedfilelist/com.apple.LSSharedFileList.ApplicationRecentDocuments",
    "Application Support/com.apple.TCC",
    "Application Support/AddressBook",
    "Application Support/MobileSync",
    "Application Support/CloudDocs",
    "Application Support/iCloud",
    "Audio/Plug-Ins",
    "Audio/Plug-Ins/Components",
    "Audio/Plug-Ins/HAL",
    "Audio/Plug-Ins/VST",
    "Audio/Plug-Ins/VST3",
    "Audio/MIDI Drivers",
    "Audio/Sounds",
    "Developer/Xcode",
    "Developer/CoreSimulator",
    "Caches/com.apple.cloudkit",
    "Group Containers/group.com.apple.notes",
});

template <std::size_t N>
bool containsIgnoringCase(const std::array<std::string_view, N>& set, std::string_view value) noexcept {
    for (const std::string_view candidate : set) {
        if (str::iequals(candidate, value)) return true;
    }
    return false;
}

/// True when `relative` (a path below some Library folder) names a protected entry.
bool isProtectedLibraryEntry(std::string_view relative) noexcept {
    if (relative.empty()) return true;  // the Library folder itself
    if (relative.find('/') == std::string_view::npos) {
        return containsIgnoringCase(kLibraryChildren, relative);
    }
    return containsIgnoringCase(kLibraryDeepRoots, relative);
}

/// If `p` lies in (or is) a Library folder — /Library or /Users/<name>/Library —
/// returns true and sets `relative` to the part below it.
bool splitLibraryPath(std::string_view p, std::string_view home, std::string_view& relative) noexcept {
    const auto tryRoot = [&](std::string_view root) noexcept {
        if (!path::iIsWithinOrEqual(p, root)) return false;
        relative = p.size() > root.size() ? p.substr(root.size() + 1) : std::string_view{};
        return true;
    };
    if (tryRoot("/Library")) return true;
    if (!home.empty()) {
        // Avoid a heap allocation: compare "<home>/Library" piecewise.
        if (path::iIsStrictlyWithin(p, home)) {
            const std::string_view rest = p.substr(home.size() + 1);
            constexpr std::string_view library = "Library";
            if (str::iequals(rest, library)) {
                relative = {};
                return true;
            }
            if (rest.size() > library.size() && rest[library.size()] == '/' &&
                str::iequals(rest.substr(0, library.size()), library)) {
                relative = rest.substr(library.size() + 1);
                return true;
            }
        }
    }
    // Any other account's Library: /Users/<name>/Library[/...]
    constexpr std::string_view users = "/Users/";
    if (str::istartsWith(p, users)) {
        const std::string_view afterUsers = p.substr(users.size());
        const std::size_t slash = afterUsers.find('/');
        if (slash != std::string_view::npos) {
            const std::string_view rest = afterUsers.substr(slash + 1);
            constexpr std::string_view library = "Library";
            if (str::iequals(rest, library)) {
                relative = {};
                return true;
            }
            if (rest.size() > library.size() && rest[library.size()] == '/' &&
                str::iequals(rest.substr(0, library.size()), library)) {
                relative = rest.substr(library.size() + 1);
                return true;
            }
        }
    }
    return false;
}

}  // namespace

const char* toString(Protection protection) noexcept {
    switch (protection) {
        case Protection::None: return "none";
        case Protection::SipProtected: return "sip";
        case Protection::ProtectedRoot: return "protected_root";
        case Protection::Invalid: return "invalid";
    }
    return "invalid";
}

PathPolicy::PathPolicy(std::string homeDirectory, std::string systemRoot)
    : actualHome_(path::normalize(homeDirectory)), systemRoot_(path::normalize(systemRoot)) {
    if (systemRoot_.empty() || systemRoot_.front() != '/') systemRoot_ = "/";
    home_ = logicalPath(actualHome_);
}

std::string PathPolicy::logicalPath(std::string_view absolutePath) const {
    std::string_view view = absolutePath;
    if (systemRoot_ != "/") {
        if (path::isStrictlyWithin(view, systemRoot_)) {
            view.remove_prefix(systemRoot_.size());
        } else if (view == systemRoot_) {
            view = "/";
        }
    }
    return path::resolveSystemAliases(view);
}

bool PathPolicy::isSystemLocation(std::string_view absolutePath) const {
    return isSipLocation(logicalPath(absolutePath));
}

bool PathPolicy::isProtectedRoot(std::string_view absolutePath) const {
    return isProtectedLogical(logicalPath(absolutePath));
}

bool PathPolicy::hasRestrictedFlag(std::uint32_t stFlags) noexcept {
    return (stFlags & (SF_RESTRICTED | SF_NOUNLINK)) != 0;
}

bool PathPolicy::isSipLocation(std::string_view p) noexcept {
    if (path::iIsWithinOrEqual(p, "/System")) return true;
    if (path::iIsWithinOrEqual(p, "/bin")) return true;
    if (path::iIsWithinOrEqual(p, "/sbin")) return true;
    if (path::iIsWithinOrEqual(p, "/Library/Apple")) return true;
    if (path::iIsWithinOrEqual(p, "/usr")) {
        // /usr/local is the one user-serviceable corner of /usr.
        return !path::iIsWithinOrEqual(p, "/usr/local");
    }
    return false;
}

bool PathPolicy::isProtectedLogical(std::string_view p) const {
    if (containsIgnoringCase(kFixedRoots, p)) return true;

    // /Volumes/<name> and /Users/<name> are roots of their own.
    for (const std::string_view container : {std::string_view("/Volumes"), std::string_view("/Users")}) {
        if (path::iIsStrictlyWithin(p, container) && path::depth(p) == 2) return true;
    }

    if (!home_.empty() && home_ != "/") {
        if (str::iequals(p, home_)) return true;
        if (path::iIsStrictlyWithin(p, home_)) {
            const std::string_view rest = p.substr(home_.size() + 1);
            if (rest.find('/') == std::string_view::npos && containsIgnoringCase(kHomeChildren, rest)) return true;
        }
        // An ancestor of the home directory can never be removed either.
        if (path::iIsStrictlyWithin(home_, p)) return true;
    }

    std::string_view relative;
    if (splitLibraryPath(p, home_, relative)) return isProtectedLibraryEntry(relative);
    return false;
}

Protection PathPolicy::classifyLexically(std::string_view absolutePath) const {
    if (absolutePath.empty() || absolutePath.front() != '/') return Protection::Invalid;
    if (absolutePath.find('\0') != std::string_view::npos) return Protection::Invalid;
    if (path::normalize(absolutePath) != absolutePath) return Protection::Invalid;  // "..", ".", "//", trailing "/"
    const std::string resolved = logicalPath(absolutePath);
    if (isSipLocation(resolved)) return Protection::SipProtected;
    if (isProtectedLogical(resolved)) return Protection::ProtectedRoot;
    return Protection::None;
}

bool PathPolicy::containerIsRestricted(const std::string& absolutePath) {
    // Only SF_RESTRICTED on a directory protects what is inside it. SF_NOUNLINK
    // says that the directory itself may not be removed — /Applications carries
    // it — and puts no restriction on its children.
    const std::string parent = path::parent(absolutePath);
    struct stat st {};
    return !parent.empty() && ::lstat(parent.c_str(), &st) == 0 && (st.st_flags & SF_RESTRICTED) != 0;
}

bool PathPolicy::isSipProtected(const std::string& absolutePath) {
    if (isSipLocation(path::resolveSystemAliases(absolutePath))) return true;
    struct stat st {};
    if (::lstat(absolutePath.c_str(), &st) == 0 && hasRestrictedFlag(st.st_flags)) return true;
    return containerIsRestricted(absolutePath);
}

Protection PathPolicy::classify(const std::string& absolutePath) const {
    const Protection lexical = classifyLexically(absolutePath);
    if (lexical != Protection::None) return lexical;
    struct stat st {};
    if (::lstat(absolutePath.c_str(), &st) == 0 && hasRestrictedFlag(st.st_flags)) return Protection::SipProtected;
    return containerIsRestricted(absolutePath) ? Protection::SipProtected : Protection::None;
}

bool PathPolicy::requiresRoot(const std::string& absolutePath) const {
    const uid_t user = ::geteuid();
    if (user == 0) return false;

    const std::string parent = path::parent(absolutePath);
    if (parent.empty()) return false;

    // Removing or renaming an entry needs write + search permission on its directory.
    // Only EACCES (plain UNIX permissions) and EROFS are something root would
    // overcome; EPERM comes from TCC, the sandbox or SIP, which bind root as well.
    if (::faccessat(AT_FDCWD, parent.c_str(), W_OK | X_OK, AT_EACCESS) != 0) {
        return errno == EACCES || errno == EROFS;
    }

    struct stat parentStat {};
    struct stat itemStat {};
    const bool haveParent = ::lstat(parent.c_str(), &parentStat) == 0;
    const bool haveItem = ::lstat(absolutePath.c_str(), &itemStat) == 0;
    if (!haveItem) return false;

    // Sticky directory: only the owner of the entry (or of the directory) may remove it.
    if (haveParent && (parentStat.st_mode & S_ISVTX) != 0 && parentStat.st_uid != user && itemStat.st_uid != user) {
        return true;
    }

    // A directory that the user cannot write can neither be emptied (permanent
    // delete) nor moved to another parent (rename must rewrite its ".." entry).
    if (S_ISDIR(itemStat.st_mode) && ::faccessat(AT_FDCWD, absolutePath.c_str(), W_OK | X_OK, AT_EACCESS) != 0) {
        return errno == EACCES || errno == EROFS;
    }
    return false;
}

}  // namespace vortex
