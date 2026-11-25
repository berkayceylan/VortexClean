#include "CFUtil.hpp"

#include <cerrno>

#include "FileUtil.hpp"
#include "StringUtil.hpp"

namespace vortex::cf {

std::string toString(CFStringRef string) {
    if (string == nullptr) return {};
    if (const char* direct = CFStringGetCStringPtr(string, kCFStringEncodingUTF8)) {
        return std::string(direct);
    }
    const CFIndex length = CFStringGetLength(string);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    if (capacity <= 0) return {};
    std::string buffer(static_cast<std::size_t>(capacity), '\0');
    if (!CFStringGetCString(string, buffer.data(), capacity, kCFStringEncodingUTF8)) return {};
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    return buffer;
}

Ref<CFStringRef> makeString(std::string_view text) {
    CFStringRef created =
        CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
    if (created == nullptr) {
        // Not valid UTF-8 (possible for names read from a damaged BOM): repair and retry.
        const std::string repaired = str::sanitizeUtf8(text);
        created = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(repaired.data()),
                                          static_cast<CFIndex>(repaired.size()), kCFStringEncodingUTF8, false);
    }
    return Ref<CFStringRef>::adopt(created);
}

Ref<CFURLRef> makeFileUrl(std::string_view path, bool isDirectory) {
    return Ref<CFURLRef>::adopt(CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.data()), static_cast<CFIndex>(path.size()),
        isDirectory));
}

Result<Ref<CFPropertyListRef>> readPropertyList(const std::string& path, std::size_t maxBytes) {
    auto bytes = io::readBinaryFile(path, maxBytes);
    if (!bytes) return bytes.error();
    const auto& buffer = bytes.value();
    if (buffer.empty()) return Error{EINVAL, "'" + path + "' is empty"};

    const auto data = Ref<CFDataRef>::adopt(CFDataCreateWithBytesNoCopy(
        kCFAllocatorDefault, buffer.data(), static_cast<CFIndex>(buffer.size()), kCFAllocatorNull));
    if (!data) return Error{ENOMEM, "cannot wrap '" + path + "' in CFData"};

    Ref<CFErrorRef> error;
    const auto plist = Ref<CFPropertyListRef>::adopt(
        CFPropertyListCreateWithData(kCFAllocatorDefault, data.get(), kCFPropertyListImmutable, nullptr, error.put()));
    if (!plist) {
        std::string reason = "not a valid property list";
        if (error) {
            const auto description = Ref<CFStringRef>::adopt(CFErrorCopyDescription(error.get()));
            if (description) reason = toString(description.get());
        }
        return Error{EINVAL, "cannot parse '" + path + "': " + reason};
    }
    return plist;
}

CFDictionaryRef asDictionary(CFTypeRef value) noexcept {
    return (value != nullptr && CFGetTypeID(value) == CFDictionaryGetTypeID()) ? static_cast<CFDictionaryRef>(value)
                                                                               : nullptr;
}

CFArrayRef asArray(CFTypeRef value) noexcept {
    return (value != nullptr && CFGetTypeID(value) == CFArrayGetTypeID()) ? static_cast<CFArrayRef>(value) : nullptr;
}

CFStringRef asString(CFTypeRef value) noexcept {
    return (value != nullptr && CFGetTypeID(value) == CFStringGetTypeID()) ? static_cast<CFStringRef>(value) : nullptr;
}

CFTypeRef dictValue(CFDictionaryRef dict, std::string_view key) {
    if (dict == nullptr) return nullptr;
    const auto cfKey = makeString(key);
    if (!cfKey) return nullptr;
    return CFDictionaryGetValue(dict, cfKey.get());
}

std::string dictString(CFDictionaryRef dict, std::string_view key) {
    return toString(asString(dictValue(dict, key)));
}

std::vector<std::string> arrayStrings(CFArrayRef array) {
    std::vector<std::string> out;
    if (array == nullptr) return out;
    const CFIndex count = CFArrayGetCount(array);
    out.reserve(static_cast<std::size_t>(count));
    for (CFIndex i = 0; i < count; ++i) {
        if (CFStringRef element = asString(CFArrayGetValueAtIndex(array, i))) {
            out.push_back(toString(element));
        }
    }
    return out;
}

std::vector<std::string> dictStringArray(CFDictionaryRef dict, std::string_view key) {
    return arrayStrings(asArray(dictValue(dict, key)));
}

std::vector<std::string> dictKeys(CFDictionaryRef dict) {
    std::vector<std::string> out;
    if (dict == nullptr) return out;
    const CFIndex count = CFDictionaryGetCount(dict);
    if (count <= 0) return out;
    std::vector<const void*> keys(static_cast<std::size_t>(count));
    CFDictionaryGetKeysAndValues(dict, keys.data(), nullptr);
    out.reserve(keys.size());
    for (const void* key : keys) {
        if (CFStringRef text = asString(static_cast<CFTypeRef>(key))) out.push_back(toString(text));
    }
    return out;
}

}  // namespace vortex::cf
