#include "ReceiptStore.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <sys/stat.h>
#include <utility>

#include "BomParser.hpp"
#include "CFUtil.hpp"
#include "FileUtil.hpp"
#include "PathUtil.hpp"
#include "StringUtil.hpp"

namespace vortex {

namespace {

constexpr std::size_t kMaxReceiptPlistBytes = std::size_t{4} << 20;

std::int64_t dictDateEpoch(CFDictionaryRef dict, std::string_view key) {
    const CFTypeRef value = cf::dictValue(dict, key);
    if (value == nullptr || CFGetTypeID(value) != CFDateGetTypeID()) return 0;
    return static_cast<std::int64_t>(CFDateGetAbsoluteTime(static_cast<CFDateRef>(value)) +
                                     kCFAbsoluteTimeIntervalSince1970);
}

}  // namespace

ReceiptStore::ReceiptStore(std::string installRoot) : installRoot_(path::normalize(installRoot)) {
    if (installRoot_.empty() || installRoot_.front() != '/') installRoot_ = "/";
}

std::uint64_t ReceiptStore::fingerprint(std::string_view absolutePath) {
    return str::fnv1a64(str::toLower(absolutePath));
}

std::string ReceiptStore::resolve(std::string_view installPrefix, std::string_view bomRelativePath) const {
    if (path::hasParentReference(installPrefix) || path::hasParentReference(bomRelativePath)) return {};
    if (bomRelativePath.find('\0') != std::string_view::npos) return {};

    std::string_view relative = bomRelativePath;
    if (relative == ".") {
        relative = {};
    } else if (relative.starts_with("./")) {
        relative.remove_prefix(2);
    }

    std::string joined = installRoot_;
    joined = path::join(joined, installPrefix);
    joined = path::join(joined, relative);
    std::string resolved = path::normalize(joined);
    // Only the real root has /etc, /var and /tmp aliases worth folding.
    if (installRoot_ == "/") resolved = path::resolveSystemAliases(resolved);
    if (!path::isWithinOrEqual(resolved, installRoot_)) return {};
    return resolved;
}

std::vector<Receipt> ReceiptStore::load(const std::vector<std::string>& directories,
                                        const CancellationToken& token) const {
    std::vector<Receipt> receipts;
    std::vector<std::string> visited;
    for (const std::string& directory : directories) {
        if (token.isCancelled()) break;
        const auto canonical = path::canonical(directory);
        if (!canonical) continue;
        if (std::find(visited.begin(), visited.end(), *canonical) != visited.end()) continue;
        visited.push_back(*canonical);

        auto entries = io::listDirectory(*canonical);
        if (!entries) continue;
        for (const io::DirEntry& entry : entries.value()) {
            if (token.isCancelled()) break;
            if (!str::iendsWith(entry.name, ".plist")) continue;
            const std::string plistPath = path::join(*canonical, entry.name);
            auto plist = cf::readPropertyList(plistPath, kMaxReceiptPlistBytes);
            if (!plist) continue;
            const CFDictionaryRef dict = cf::asDictionary(plist.value().get());
            if (dict == nullptr) continue;

            Receipt receipt;
            receipt.plistPath = plistPath;
            receipt.packageId = cf::dictString(dict, "PackageIdentifier");
            const std::string stem(entry.name.substr(0, entry.name.size() - 6));
            if (receipt.packageId.empty()) receipt.packageId = stem;
            receipt.installPrefix = cf::dictString(dict, "InstallPrefixPath");
            receipt.version = cf::dictString(dict, "PackageVersion");
            receipt.installProcess = cf::dictString(dict, "InstallProcessName");
            receipt.packageFileName = cf::dictString(dict, "PackageFileName");
            receipt.installEpoch = dictDateEpoch(dict, "InstallDate");

            std::string bomPath = path::join(*canonical, stem + ".bom");
            if (io::isRegularFile(bomPath)) receipt.bomPath = std::move(bomPath);
            receipts.push_back(std::move(receipt));
        }
    }
    std::sort(receipts.begin(), receipts.end(),
              [](const Receipt& a, const Receipt& b) { return a.packageId < b.packageId; });
    return receipts;
}

std::shared_ptr<const ReceiptDigest> ReceiptStore::digest(const Receipt& receipt,
                                                          const CancellationToken& token) const {
    auto result = std::make_shared<ReceiptDigest>();
    if (receipt.bomPath.empty()) return result;

    struct stat st {};
    if (::stat(receipt.bomPath.c_str(), &st) != 0) return result;
    const auto seconds = static_cast<std::int64_t>(st.st_mtimespec.tv_sec);
    const auto nanoseconds = static_cast<std::int64_t>(st.st_mtimespec.tv_nsec);
    const auto size = static_cast<std::int64_t>(st.st_size);
    // The prefix is part of the key: the same BOM maps to different absolute paths under another prefix.
    const std::string cacheKey = receipt.bomPath + '\n' + receipt.installPrefix;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = cache_.find(cacheKey);
        if (found != cache_.end() && found->second.modifiedSeconds == seconds &&
            found->second.modifiedNanoseconds == nanoseconds && found->second.size == size) {
            return found->second.digest;
        }
    }

    auto parsed = BomParser::parseFile(receipt.bomPath, BomParseOptions{}, token);
    if (!parsed) return result;  // unreadable, malformed or cancelled: not cached, not valid
    const BomContents& contents = parsed.value();

    result->entryCount = contents.entries.size();
    result->rootPath = resolve(receipt.installPrefix, ".");

    // Pass 1: outermost *.app directories.
    std::vector<std::string> bundles;
    for (const BomEntry& entry : contents.entries) {
        if (entry.type != BomEntryType::Directory || !str::iendsWith(entry.path, ".app")) continue;
        std::string absolute = resolve(receipt.installPrefix, entry.path);
        if (!absolute.empty()) bundles.push_back(std::move(absolute));
    }
    std::sort(bundles.begin(), bundles.end(),
              [](const std::string& a, const std::string& b) { return a.size() < b.size(); });
    for (std::string& candidate : bundles) {
        const bool nested = std::any_of(result->appBundles.begin(), result->appBundles.end(),
                                        [&](const std::string& outer) {
                                            return path::iIsWithinOrEqual(candidate, outer);
                                        });
        if (!nested) result->appBundles.push_back(std::move(candidate));
    }

    // Pass 2: fingerprints of everything installed outside those bundles.
    std::size_t counter = 0;
    for (const BomEntry& entry : contents.entries) {
        if ((++counter % 4096) == 0 && token.isCancelled()) return std::make_shared<ReceiptDigest>();
        if (entry.type == BomEntryType::Directory) continue;
        const std::string absolute = resolve(receipt.installPrefix, entry.path);
        if (absolute.empty()) continue;
        const bool insideBundle = std::any_of(result->appBundles.begin(), result->appBundles.end(),
                                              [&](const std::string& bundle) {
                                                  return path::iIsWithinOrEqual(absolute, bundle);
                                              });
        if (!insideBundle) result->payload.insert(fingerprint(absolute));
    }

    result->valid = contents.skippedEntries == 0 && !result->rootPath.empty();

    const std::lock_guard<std::mutex> lock(mutex_);
    cache_[cacheKey] = CacheSlot{seconds, nanoseconds, size, result};
    return result;
}

}  // namespace vortex
