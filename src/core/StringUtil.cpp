#include "StringUtil.hpp"

#include <array>
#include <cstdio>

namespace vortex::str {

namespace {

constexpr char lowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool isAsciiAlnum(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

constexpr bool isHexDigit(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

}  // namespace

std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lowerAscii(c);
    return out;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    }
    return true;
}

bool istartsWith(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool iendsWith(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

std::vector<std::string> split(std::string_view s, char delimiter, bool keepEmpty) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t end = s.find(delimiter, start);
        const std::string_view piece =
            s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (keepEmpty || !piece.empty()) parts.emplace_back(piece);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return parts;
}

std::string_view trim(std::string_view s) noexcept {
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const std::size_t first = s.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    const std::size_t last = s.find_last_not_of(whitespace);
    return s.substr(first, last - first + 1);
}

std::string normalizeName(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x80) {
            out.push_back(ch);
        } else if (isAsciiAlnum(c)) {
            out.push_back(lowerAscii(ch));
        }
    }
    return out;
}

std::vector<std::string> nameTokens(std::string_view s) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x80 || isAsciiAlnum(c)) {
            current.push_back(lowerAscii(ch));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

bool isHex(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (const char ch : s) {
        if (!isHexDigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

bool isAllDigits(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (const char ch : s) {
        if (ch < '0' || ch > '9') return false;
    }
    return true;
}

bool isUuid(std::string_view s) noexcept {
    constexpr std::array<std::size_t, 5> groups{8, 4, 4, 4, 12};
    if (s.size() != 36) return false;
    std::size_t pos = 0;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        if (!isHex(s.substr(pos, groups[g]))) return false;
        pos += groups[g];
        if (g + 1 < groups.size()) {
            if (s[pos] != '-') return false;
            ++pos;
        }
    }
    return true;
}

std::string formatBytes(std::uint64_t bytes) {
    constexpr std::array<const char*, 6> units{"bytes", "KB", "MB", "GB", "TB", "PB"};
    if (bytes < 1000) {
        return std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes");
    }
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < units.size()) {
        value /= 1000.0;
        ++unit;
    }
    std::array<char, 48> buffer{};
    const char* format = value >= 100.0 ? "%.0f %s" : (value >= 10.0 ? "%.1f %s" : "%.2f %s");
    std::snprintf(buffer.data(), buffer.size(), format, value, units[unit]);
    return std::string(buffer.data());
}

std::string sanitizeUtf8(std::string_view s) {
    constexpr std::string_view replacement = "\xEF\xBF\xBD";
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto lead = static_cast<unsigned char>(s[i]);
        std::size_t length = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t minimum = 0;
        if (lead < 0x80) {
            out.push_back(s[i]);
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            codePoint = lead & 0x1Fu;
            minimum = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            codePoint = lead & 0x0Fu;
            minimum = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            codePoint = lead & 0x07u;
            minimum = 0x10000;
        }
        bool valid = length != 0 && i + length <= s.size();
        if (valid) {
            for (std::size_t k = 1; k < length; ++k) {
                const auto continuation = static_cast<unsigned char>(s[i + k]);
                if ((continuation & 0xC0u) != 0x80u) {
                    valid = false;
                    break;
                }
                codePoint = (codePoint << 6) | (continuation & 0x3Fu);
            }
        }
        if (valid) {
            const bool surrogate = codePoint >= 0xD800 && codePoint <= 0xDFFF;
            if (codePoint < minimum || codePoint > 0x10FFFF || surrogate) valid = false;
        }
        if (valid) {
            out.append(s.substr(i, length));
            i += length;
        } else {
            out.append(replacement);
            ++i;
        }
    }
    return out;
}

std::uint64_t fnv1a64(std::string_view s) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char ch : s) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

}  // namespace vortex::str
