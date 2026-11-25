#include "BundleInspector.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <Security/Security.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <unordered_set>

#include "CFUtil.hpp"
#include "FileUtil.hpp"
#include "PathPolicy.hpp"
#include "PathUtil.hpp"
#include "StringUtil.hpp"

namespace vortex {

namespace {

constexpr std::size_t kMaxPlistBytes = std::size_t{16} << 20;

/// Sub-directories of an app bundle that may hold helper bundles with their own identifier.
constexpr auto kNestedBundleDirs = std::to_array<std::string_view>({
    "Contents/Library/LoginItems",
    "Contents/XPCServices",
    "Contents/PlugIns",
    "Contents/Extensions",
    "Contents/Helpers",
    "Contents/Frameworks",
    "Contents/MacOS",
    "Contents/Resources",
    "Contents/SharedSupport",
    "Contents/Applications",
    "Contents/Library/SystemExtensions",
    "Contents/Library/Spotlight",
    "Contents/Library/QuickLook",
});

constexpr auto kNestedBundleExtensions = std::to_array<std::string_view>({
    "app", "xpc", "appex", "systemextension", "dext", "mdimporter", "qlgenerator", "prefPane",
});

bool isNestedBundleName(std::string_view name) noexcept {
    const std::string_view extension = path::extension(name);
    if (extension.empty()) return false;
    for (const std::string_view candidate : kNestedBundleExtensions) {
        if (str::iequals(extension, candidate)) return true;
    }
    return false;
}

void appendUnique(std::vector<std::string>& list, std::string value) {
    if (value.empty()) return;
    for (const std::string& existing : list) {
        if (str::iequals(existing, value)) return;
    }
    list.push_back(std::move(value));
}

SigningInfo readSigningInfo(const std::string& bundlePath) {
    SigningInfo info;
    const auto url = cf::makeFileUrl(bundlePath, true);
    if (!url) return info;

    cf::Ref<SecStaticCodeRef> code;
    if (SecStaticCodeCreateWithPath(url.get(), kSecCSDefaultFlags, code.put()) != errSecSuccess || !code) {
        return info;
    }

    cf::Ref<CFDictionaryRef> details;
    const SecCSFlags flags = kSecCSSigningInformation | kSecCSRequirementInformation;
    if (SecCodeCopySigningInformation(code.get(), flags, details.put()) != errSecSuccess || !details) {
        return info;
    }

    // Unsigned code yields a dictionary without an identifier.
    const CFStringRef identifier = cf::asString(CFDictionaryGetValue(details.get(), kSecCodeInfoIdentifier));
    if (identifier == nullptr) return info;
    info.isSigned = true;
    info.signingIdentifier = cf::toString(identifier);
    info.teamIdentifier =
        cf::toString(cf::asString(CFDictionaryGetValue(details.get(), kSecCodeInfoTeamIdentifier)));

    if (const void* rawFlags = CFDictionaryGetValue(details.get(), kSecCodeInfoFlags);
        rawFlags != nullptr && CFGetTypeID(rawFlags) == CFNumberGetTypeID()) {
        std::int64_t value = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(rawFlags), kCFNumberSInt64Type, &value)) {
            info.isAdHoc = (static_cast<std::uint64_t>(value) & kSecCodeSignatureAdhoc) != 0;
        }
    }

    if (const CFArrayRef certificates = cf::asArray(CFDictionaryGetValue(details.get(), kSecCodeInfoCertificates))) {
        const CFIndex count = CFArrayGetCount(certificates);
        for (CFIndex i = 0; i < count; ++i) {
            const void* element = CFArrayGetValueAtIndex(certificates, i);
            if (element == nullptr || CFGetTypeID(element) != SecCertificateGetTypeID()) continue;
            const auto summary = cf::Ref<CFStringRef>::adopt(
                SecCertificateCopySubjectSummary(static_cast<SecCertificateRef>(const_cast<void*>(element))));
            if (summary) info.authorityChain.push_back(cf::toString(summary.get()));
        }
        if (!info.authorityChain.empty()) info.authority = info.authorityChain.front();
    }

    if (const CFDictionaryRef entitlements =
            cf::asDictionary(CFDictionaryGetValue(details.get(), kSecCodeInfoEntitlementsDict))) {
        info.applicationGroups = cf::dictStringArray(entitlements, "com.apple.security.application-groups");
    }

    // "anchor apple" is only satisfied by Apple's own software. The basic-validate
    // flags skip hashing the executable and resources, so this stays cheap even
    // for multi-gigabyte bundles, and no network access is allowed.
    cf::Ref<SecRequirementRef> appleAnchor;
    if (SecRequirementCreateWithString(CFSTR("anchor apple"), kSecCSDefaultFlags, appleAnchor.put()) ==
            errSecSuccess &&
        appleAnchor) {
        const SecCSFlags validation =
            static_cast<SecCSFlags>(kSecCSBasicValidateOnly) | static_cast<SecCSFlags>(kSecCSNoNetworkAccess);
        info.isApplePlatform =
            SecStaticCodeCheckValidityWithErrors(code.get(), validation, appleAnchor.get(), nullptr) == errSecSuccess;
    }
    return info;
}

std::int64_t readLastUsedEpoch(const std::string& bundlePath) {
    const auto cfPath = cf::makeString(bundlePath);
    if (!cfPath) return 0;
    const auto item = cf::Ref<MDItemRef>::adopt(MDItemCreate(kCFAllocatorDefault, cfPath.get()));
    if (!item) return 0;
    const auto value = cf::Ref<CFTypeRef>::adopt(MDItemCopyAttribute(item.get(), kMDItemLastUsedDate));
    if (!value || CFGetTypeID(value.get()) != CFDateGetTypeID()) return 0;
    const CFAbsoluteTime absolute = CFDateGetAbsoluteTime(static_cast<CFDateRef>(value.get()));
    return static_cast<std::int64_t>(absolute + kCFAbsoluteTimeIntervalSince1970);
}

/// Collects identifiers of helper bundles and launchd labels declared inside the bundle.
void enumerateEmbedded(const std::string& bundlePath, AppInfo& info) {
    for (const std::string_view relative : kNestedBundleDirs) {
        const std::string dir = path::join(bundlePath, relative);
        auto entries = io::listDirectory(dir);
        if (!entries) continue;
        for (const io::DirEntry& entry : entries.value()) {
            if (!isNestedBundleName(entry.name)) continue;
            std::string identifier = BundleInspector::bundleIdentifier(path::join(dir, entry.name));
            if (!identifier.empty() && !str::iequals(identifier, info.bundleId)) {
                appendUnique(info.embeddedBundleIds, std::move(identifier));
            }
        }
    }

    // Privileged helper tools are stored under their launchd label.
    if (auto tools = io::listDirectory(path::join(bundlePath, "Contents/Library/LaunchServices"))) {
        for (const io::DirEntry& entry : tools.value()) {
            if (!entry.name.empty() && entry.name.front() != '.') appendUnique(info.helperLabels, entry.name);
        }
    }

    // SMAppService-style agents and daemons embed their property lists in the bundle.
    for (const std::string_view relative :
         {std::string_view("Contents/Library/LaunchAgents"), std::string_view("Contents/Library/LaunchDaemons")}) {
        const std::string dir = path::join(bundlePath, relative);
        auto entries = io::listDirectory(dir);
        if (!entries) continue;
        for (const io::DirEntry& entry : entries.value()) {
            if (!str::iendsWith(entry.name, ".plist")) continue;
            auto plist = cf::readPropertyList(path::join(dir, entry.name), kMaxPlistBytes);
            if (!plist) continue;
            appendUnique(info.helperLabels, cf::dictString(cf::asDictionary(plist.value().get()), "Label"));
        }
    }
}

}  // namespace

std::string SigningInfo::summary() const {
    if (!isSigned) return "Unsigned";
    if (isAdHoc) return "Ad-hoc signed";
    std::string out = authority.empty() ? std::string("Signed") : authority;
    if (!teamIdentifier.empty() && out.find(teamIdentifier) == std::string::npos) {
        out += " (" + teamIdentifier + ")";
    }
    return out;
}

bool BundleInspector::hasAppExtension(std::string_view p) noexcept {
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    return str::iendsWith(p, ".app") && p.size() > 4 && p[p.size() - 5] != '/';
}

std::string BundleInspector::infoPlistPath(const std::string& bundlePath) {
    static constexpr auto kCandidates = std::to_array<std::string_view>({
        "Contents/Info.plist",
        "WrappedBundle/Info.plist",
        "Info.plist",
        "Resources/Info.plist",
        "Versions/Current/Resources/Info.plist",
        "Versions/A/Resources/Info.plist",
    });
    for (const std::string_view relative : kCandidates) {
        std::string candidate = path::join(bundlePath, relative);
        if (io::isRegularFile(candidate)) return candidate;
    }
    // iOS apps are wrapped as Foo.app/Wrapper/Foo.app when "WrappedBundle" is missing.
    const std::string wrapper = path::join(bundlePath, "Wrapper");
    if (auto entries = io::listDirectory(wrapper)) {
        for (const io::DirEntry& entry : entries.value()) {
            if (!str::iendsWith(entry.name, ".app")) continue;
            std::string candidate = path::join(path::join(wrapper, entry.name), "Info.plist");
            if (io::isRegularFile(candidate)) return candidate;
        }
    }
    return {};
}

std::string BundleInspector::bundleIdentifier(const std::string& bundlePath) {
    const std::string plistPath = infoPlistPath(bundlePath);
    if (plistPath.empty()) return {};
    auto plist = cf::readPropertyList(plistPath, kMaxPlistBytes);
    if (!plist) return {};
    return cf::dictString(cf::asDictionary(plist.value().get()), "CFBundleIdentifier");
}

Result<AppInfo> BundleInspector::inspect(const std::string& bundlePath, const Options& options) {
    const auto canonical = path::canonical(bundlePath);
    if (!canonical) {
        const int err = errno;
        return Error{err, "cannot resolve '" + bundlePath + "': " + io::errnoMessage(err)};
    }
    struct stat st {};
    if (::lstat(canonical->c_str(), &st) != 0) {
        const int err = errno;
        return Error{err, "cannot stat '" + *canonical + "': " + io::errnoMessage(err)};
    }
    if (!S_ISDIR(st.st_mode)) return Error{ENOTDIR, "'" + *canonical + "' is not a bundle directory"};

    const std::string plistPath = infoPlistPath(*canonical);
    if (plistPath.empty()) {
        return Error{ENOENT, "'" + *canonical + "' has no Info.plist and is not an application bundle"};
    }
    auto plist = cf::readPropertyList(plistPath, kMaxPlistBytes);
    if (!plist) return plist.error();
    const CFDictionaryRef dict = cf::asDictionary(plist.value().get());
    if (dict == nullptr) return Error{EINVAL, "'" + plistPath + "' is not a dictionary"};

    AppInfo info;
    info.bundlePath = *canonical;
    info.bundleId = cf::dictString(dict, "CFBundleIdentifier");
    info.bundleDisplayName = cf::dictString(dict, "CFBundleDisplayName");
    info.bundleName = cf::dictString(dict, "CFBundleName");
    info.executableName = cf::dictString(dict, "CFBundleExecutable");
    info.shortVersion = cf::dictString(dict, "CFBundleShortVersionString");
    info.bundleVersion = cf::dictString(dict, "CFBundleVersion");

    const std::string_view fileName = path::fileName(*canonical);
    info.displayName = std::string(str::iendsWith(fileName, ".app") ? path::stem(fileName) : fileName);
    if (info.displayName.empty()) {
        info.displayName = !info.bundleDisplayName.empty() ? info.bundleDisplayName : info.bundleName;
    }
    if (info.executableName.empty()) info.executableName = info.displayName;

    // The executable sits next to the Info.plist for flat/wrapped bundles and in
    // Contents/MacOS for regular macOS bundles.
    const std::string plistDir = path::parent(plistPath);
    info.isWrappedIosApp = path::fileName(plistDir) != "Contents";
    info.executablePath = info.isWrappedIosApp ? path::join(plistDir, info.executableName)
                                               : path::join(path::join(plistDir, "MacOS"), info.executableName);

    info.ownerUid = st.st_uid;
    info.modifiedEpoch = static_cast<std::int64_t>(st.st_mtime);
    info.isSipProtected = PathPolicy::isSipProtected(*canonical);
    info.isAppStore = io::isRegularFile(path::join(*canonical, "Contents/_MASReceipt/receipt"));

    for (const std::string& label :
         cf::dictKeys(cf::asDictionary(cf::dictValue(dict, "SMPrivilegedExecutables")))) {
        appendUnique(info.helperLabels, label);
    }
    if (options.enumerateEmbeddedBundles) enumerateEmbedded(*canonical, info);
    if (options.readSigningInfo) info.signing = readSigningInfo(*canonical);
    if (options.readLastUsedDate) info.lastUsedEpoch = readLastUsedEpoch(*canonical);

    info.isAppleApp = str::istartsWith(info.bundleId, "com.apple.") || info.signing.isApplePlatform;
    return info;
}

std::vector<std::string> BundleInspector::defaultSearchRoots(const std::string& home, const std::string& systemRoot) {
    std::vector<std::string> roots{path::join(systemRoot, "Applications")};
    if (!home.empty()) roots.push_back(path::join(home, "Applications"));
    return roots;
}

std::vector<std::string> BundleInspector::auxiliarySearchRoots(const std::string& systemRoot) {
    return {path::join(systemRoot, "System/Applications"), path::join(systemRoot, "Library/PrivilegedHelperTools")};
}

std::vector<AppInfo> BundleInspector::inspectAuxiliaryApplications(const CancellationToken& token,
                                                                   const std::string& systemRoot) {
    std::vector<AppInfo> apps;
    Options options;
    options.readSigningInfo = false;
    options.readLastUsedDate = false;
    options.enumerateEmbeddedBundles = true;  // helper apps nest further apps with their own identifiers
    for (const std::string& bundlePath : discoverBundles(auxiliarySearchRoots(systemRoot), token, 2)) {
        if (token.isCancelled()) break;
        auto inspected = inspect(bundlePath, options);
        if (!inspected) continue;
        AppInfo& app = inspected.value();
        if (path::iIsWithinOrEqual(app.bundlePath, path::join(systemRoot, "System"))) {
            app.isSipProtected = true;
            app.isAppleApp = true;
        }
        apps.push_back(std::move(app));
    }
    return apps;
}

bool BundleInspector::isRegisteredToAnotherApplication(std::string_view bundleId,
                                                       std::string_view targetBundlePath) {
    if (bundleId.empty()) return false;
    const auto identifier = cf::makeString(bundleId);
    if (!identifier) return false;
    cf::Ref<CFErrorRef> error;
    const auto urls =
        cf::Ref<CFArrayRef>::adopt(LSCopyApplicationURLsForBundleIdentifier(identifier.get(), error.put()));
    if (!urls) return false;

    bool registeredElsewhere = false;
    const CFIndex count = CFArrayGetCount(urls.get());
    for (CFIndex i = 0; i < count; ++i) {
        const void* element = CFArrayGetValueAtIndex(urls.get(), i);
        if (element == nullptr || CFGetTypeID(element) != CFURLGetTypeID()) continue;
        std::array<char, 4096> buffer{};
        if (!CFURLGetFileSystemRepresentation(static_cast<CFURLRef>(element), true,
                                              reinterpret_cast<UInt8*>(buffer.data()),
                                              static_cast<CFIndex>(buffer.size()))) {
            continue;
        }
        const std::string registered = path::normalize(buffer.data());
        if (!io::existsNoFollow(registered)) continue;  // a stale registration proves nothing
        // One registration inside the target is enough: the identifier is the target's own.
        if (path::iIsWithinOrEqual(registered, targetBundlePath)) return false;
        registeredElsewhere = true;
    }
    return registeredElsewhere;
}

std::vector<std::string> BundleInspector::discoverBundles(const std::vector<std::string>& roots,
                                                          const CancellationToken& token, int maxDepth) {
    std::vector<std::string> found;
    std::unordered_set<std::string> seen;

    struct Pending {
        std::string dir;
        int depth;
    };
    std::vector<Pending> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.push_back(Pending{*it, 1});

    while (!stack.empty()) {
        if (token.isCancelled()) break;
        const Pending current = std::move(stack.back());
        stack.pop_back();

        auto entries = io::listDirectory(current.dir);
        if (!entries) continue;
        for (const io::DirEntry& entry : entries.value()) {
            if (entry.name.empty() || entry.name.front() == '.') continue;
            // Symlinks are never followed: a link is not an installed copy.
            if (entry.type == DT_LNK) continue;
            const std::string full = path::join(current.dir, entry.name);
            const bool isDir =
                entry.type == DT_DIR || (entry.type == DT_UNKNOWN && io::isDirectoryNoFollow(full));
            if (!isDir) continue;
            if (hasAppExtension(entry.name)) {
                if (seen.insert(full).second) found.push_back(full);
            } else if (current.depth < maxDepth) {
                stack.push_back(Pending{full, current.depth + 1});
            }
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace vortex
