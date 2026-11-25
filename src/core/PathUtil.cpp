#include "PathUtil.hpp"

#include <climits>
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

#include "StringUtil.hpp"

namespace vortex::path {

std::string join(std::string_view base, std::string_view component) {
    if (base.empty()) return std::string(component);
    if (component.empty()) return std::string(base);
    std::string out(base);
    if (out.back() != '/') out.push_back('/');
    while (!component.empty() && component.front() == '/') component.remove_prefix(1);
    out.append(component);
    return out;
}

std::string_view fileName(std::string_view p) noexcept {
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    const std::size_t slash = p.rfind('/');
    if (slash == std::string_view::npos) return p;
    return p.substr(slash + 1);
}

std::string parent(std::string_view p) {
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    const std::size_t slash = p.rfind('/');
    if (slash == std::string_view::npos) return {};
    if (slash == 0) return "/";
    return std::string(p.substr(0, slash));
}

std::string_view extension(std::string_view name) noexcept {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) return {};
    return name.substr(dot + 1);
}

std::string_view stem(std::string_view name) noexcept {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) return name;
    return name.substr(0, dot);
}

std::vector<std::string_view> components(std::string_view p) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start < p.size()) {
        const std::size_t end = p.find('/', start);
        const std::size_t stop = end == std::string_view::npos ? p.size() : end;
        if (stop > start) parts.push_back(p.substr(start, stop - start));
        start = stop + 1;
    }
    return parts;
}

std::string normalize(std::string_view p) {
    const bool absolute = !p.empty() && p.front() == '/';
    std::vector<std::string_view> stack;
    for (const std::string_view part : components(p)) {
        if (part == ".") continue;
        if (part == "..") {
            if (!stack.empty() && stack.back() != "..") {
                stack.pop_back();
            } else if (!absolute) {
                stack.push_back(part);
            }
            continue;
        }
        stack.push_back(part);
    }
    std::string out;
    if (absolute) out.push_back('/');
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (i != 0) out.push_back('/');
        out.append(stack[i]);
    }
    if (out.empty()) out = ".";
    return out;
}

std::size_t depth(std::string_view p) noexcept {
    std::size_t count = 0;
    std::size_t start = 0;
    while (start < p.size()) {
        const std::size_t end = p.find('/', start);
        const std::size_t stop = end == std::string_view::npos ? p.size() : end;
        if (stop > start) ++count;
        start = stop + 1;
    }
    return count;
}

bool hasParentReference(std::string_view p) noexcept {
    std::size_t start = 0;
    while (start < p.size()) {
        const std::size_t end = p.find('/', start);
        const std::size_t stop = end == std::string_view::npos ? p.size() : end;
        if (p.substr(start, stop - start) == "..") return true;
        start = stop + 1;
    }
    return false;
}

namespace {

bool within(std::string_view p, std::string_view ancestor, bool allowEqual, bool ignoreCase) noexcept {
    if (ancestor.empty() || p.empty()) return false;
    while (ancestor.size() > 1 && ancestor.back() == '/') ancestor.remove_suffix(1);
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    const auto equal = [ignoreCase](std::string_view a, std::string_view b) noexcept {
        return ignoreCase ? str::iequals(a, b) : a == b;
    };
    if (p.size() == ancestor.size()) return allowEqual && equal(p, ancestor);
    if (p.size() < ancestor.size()) return false;
    if (ancestor == "/") return p.front() == '/';
    return p[ancestor.size()] == '/' && equal(p.substr(0, ancestor.size()), ancestor);
}

}  // namespace

bool isWithinOrEqual(std::string_view p, std::string_view ancestor) noexcept {
    return within(p, ancestor, true, false);
}
bool isStrictlyWithin(std::string_view p, std::string_view ancestor) noexcept {
    return within(p, ancestor, false, false);
}
bool iIsWithinOrEqual(std::string_view p, std::string_view ancestor) noexcept {
    return within(p, ancestor, true, true);
}
bool iIsStrictlyWithin(std::string_view p, std::string_view ancestor) noexcept {
    return within(p, ancestor, false, true);
}

std::string abbreviateHome(std::string_view p, std::string_view home) {
    if (!home.empty() && home != "/" && isWithinOrEqual(p, home)) {
        std::string out = "~";
        out.append(p.substr(home.size()));
        return out;
    }
    return std::string(p);
}

std::string homeDirectory() {
    const long suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(suggested > 0 ? static_cast<std::size_t>(suggested) : 4096);
    struct passwd entry {};
    struct passwd* found = nullptr;
    if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr &&
        found->pw_dir != nullptr && found->pw_dir[0] == '/') {
        return normalize(found->pw_dir);
    }
    if (const char* env = std::getenv("HOME"); env != nullptr && env[0] == '/') {
        return normalize(env);
    }
    return "/var/empty";
}

std::optional<std::string> canonical(const std::string& p) {
    char resolved[PATH_MAX];
    if (::realpath(p.c_str(), resolved) == nullptr) return std::nullopt;
    return std::string(resolved);
}

std::string resolveSystemAliases(std::string_view p) {
    std::string out = normalize(p);
    constexpr std::string_view dataVolume = "/System/Volumes/Data";
    if (iIsStrictlyWithin(out, dataVolume)) {
        out.erase(0, dataVolume.size());
    } else if (str::iequals(out, dataVolume)) {
        out = "/";
    }
    for (const std::string_view alias : {std::string_view("/etc"), std::string_view("/var"), std::string_view("/tmp")}) {
        if (iIsWithinOrEqual(out, alias)) {
            out.insert(0, "/private");
            break;
        }
    }
    return out;
}

}  // namespace vortex::path
