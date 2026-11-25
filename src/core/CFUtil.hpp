// VortexClean core — RAII and conversion helpers for CoreFoundation.
// Plain C++: CoreFoundation is a C API, so the engine never needs Objective-C.
#pragma once

#include <CoreFoundation/CoreFoundation.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Result.hpp"

namespace vortex::cf {

/// Owning smart pointer for any CoreFoundation-style reference type.
template <typename T>
class Ref {
public:
    Ref() noexcept = default;

    /// Takes over a +1 reference (the result of a Create/Copy function).
    [[nodiscard]] static Ref adopt(T ref) noexcept {
        Ref out;
        out.ref_ = ref;
        return out;
    }
    /// Shares a +0 reference (the result of a Get function).
    [[nodiscard]] static Ref retain(T ref) noexcept {
        Ref out;
        if (ref != nullptr) CFRetain(ref);
        out.ref_ = ref;
        return out;
    }

    Ref(const Ref& other) noexcept : ref_(other.ref_) {
        if (ref_ != nullptr) CFRetain(ref_);
    }
    Ref(Ref&& other) noexcept : ref_(std::exchange(other.ref_, nullptr)) {}
    Ref& operator=(Ref other) noexcept {
        std::swap(ref_, other.ref_);
        return *this;
    }
    ~Ref() { reset(); }

    [[nodiscard]] T get() const noexcept { return ref_; }
    explicit operator bool() const noexcept { return ref_ != nullptr; }

    void reset() noexcept {
        if (ref_ != nullptr) {
            CFRelease(ref_);
            ref_ = nullptr;
        }
    }
    /// Address for use as a Create/Copy out-parameter; releases any current value first.
    [[nodiscard]] T* put() noexcept {
        reset();
        return &ref_;
    }

private:
    T ref_ = nullptr;
};

[[nodiscard]] std::string toString(CFStringRef string);
[[nodiscard]] Ref<CFStringRef> makeString(std::string_view text);
[[nodiscard]] Ref<CFURLRef> makeFileUrl(std::string_view path, bool isDirectory);

/// Reads and parses an XML or binary property list with CFPropertyListCreateWithData.
[[nodiscard]] Result<Ref<CFPropertyListRef>> readPropertyList(const std::string& path,
                                                              std::size_t maxBytes = std::size_t{16} << 20);

/// Type-checked downcasts: nullptr unless the value really has the requested type.
[[nodiscard]] CFDictionaryRef asDictionary(CFTypeRef value) noexcept;
[[nodiscard]] CFArrayRef asArray(CFTypeRef value) noexcept;
[[nodiscard]] CFStringRef asString(CFTypeRef value) noexcept;

[[nodiscard]] CFTypeRef dictValue(CFDictionaryRef dict, std::string_view key);
/// String member, or "" when absent or not a string.
[[nodiscard]] std::string dictString(CFDictionaryRef dict, std::string_view key);
/// Array-of-strings member; non-string elements are skipped.
[[nodiscard]] std::vector<std::string> dictStringArray(CFDictionaryRef dict, std::string_view key);
/// All string keys of a dictionary.
[[nodiscard]] std::vector<std::string> dictKeys(CFDictionaryRef dict);
[[nodiscard]] std::vector<std::string> arrayStrings(CFArrayRef array);

}  // namespace vortex::cf
