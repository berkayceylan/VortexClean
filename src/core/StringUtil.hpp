// VortexClean core — string helpers (ASCII case folding, tokenising, UTF-8 hygiene).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vortex::str {

/// ASCII-only lower casing; bytes >= 0x80 (UTF-8 continuation/lead bytes) pass through untouched.
[[nodiscard]] std::string toLower(std::string_view s);

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] bool istartsWith(std::string_view s, std::string_view prefix) noexcept;
[[nodiscard]] bool iendsWith(std::string_view s, std::string_view suffix) noexcept;

[[nodiscard]] std::vector<std::string> split(std::string_view s, char delimiter, bool keepEmpty = false);
[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

/// Canonical comparison key for human-facing names: lower-cased, with every
/// ASCII character that is not a letter or digit removed ("Visual Studio Code"
/// -> "visualstudiocode"). Non-ASCII bytes are preserved.
[[nodiscard]] std::string normalizeName(std::string_view s);

/// Lower-cased word tokens: maximal runs of ASCII alphanumerics / non-ASCII bytes.
[[nodiscard]] std::vector<std::string> nameTokens(std::string_view s);

/// True for the canonical 8-4-4-4-12 hexadecimal UUID text form.
[[nodiscard]] bool isUuid(std::string_view s) noexcept;
[[nodiscard]] bool isHex(std::string_view s) noexcept;
[[nodiscard]] bool isAllDigits(std::string_view s) noexcept;

/// Decimal (SI, like Finder) human readable size: "1.24 GB".
[[nodiscard]] std::string formatBytes(std::uint64_t bytes);

/// Returns `s` with every invalid UTF-8 sequence replaced by U+FFFD, so that the
/// result is always safe to embed in JSON or hand to Cocoa.
[[nodiscard]] std::string sanitizeUtf8(std::string_view s);

/// 64-bit FNV-1a, used for compact path-set fingerprints.
[[nodiscard]] std::uint64_t fnv1a64(std::string_view s) noexcept;

}  // namespace vortex::str
