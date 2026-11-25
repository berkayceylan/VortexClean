// VortexClean core — lexical path helpers. Nothing here touches the disk except
// `canonical()` and `homeDirectory()`.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vortex::path {

[[nodiscard]] std::string join(std::string_view base, std::string_view component);

/// Last path component ("" for "/").
[[nodiscard]] std::string_view fileName(std::string_view p) noexcept;

/// Everything before the last component ("/" for top-level entries, "" for bare names).
[[nodiscard]] std::string parent(std::string_view p);

/// Extension without the dot ("" when there is none or the name is a dotfile).
[[nodiscard]] std::string_view extension(std::string_view name) noexcept;
[[nodiscard]] std::string_view stem(std::string_view name) noexcept;

/// Purely lexical normalisation: collapses repeated separators, drops "."
/// components and resolves ".." without ever climbing above the root.
[[nodiscard]] std::string normalize(std::string_view p);

[[nodiscard]] std::vector<std::string_view> components(std::string_view p);
[[nodiscard]] std::size_t depth(std::string_view p) noexcept;
[[nodiscard]] bool hasParentReference(std::string_view p) noexcept;

/// Component-aware containment tests on normalised paths. The `i` variants
/// compare ASCII case-insensitively, which is what the default (case-insensitive)
/// APFS volume does and what every safety check must assume.
[[nodiscard]] bool isWithinOrEqual(std::string_view p, std::string_view ancestor) noexcept;
[[nodiscard]] bool isStrictlyWithin(std::string_view p, std::string_view ancestor) noexcept;
[[nodiscard]] bool iIsWithinOrEqual(std::string_view p, std::string_view ancestor) noexcept;
[[nodiscard]] bool iIsStrictlyWithin(std::string_view p, std::string_view ancestor) noexcept;

/// "/Users/me/Library/Caches/x" -> "~/Library/Caches/x".
[[nodiscard]] std::string abbreviateHome(std::string_view p, std::string_view home);

/// Home directory from the password database (falls back to $HOME).
[[nodiscard]] std::string homeDirectory();

/// realpath(3); nullopt when the path cannot be resolved.
[[nodiscard]] std::optional<std::string> canonical(const std::string& p);

/// Rewrites the well-known root aliases to their real location so that policy
/// checks see one spelling: /etc, /var, /tmp -> /private/..., and the
/// /System/Volumes/Data firmlink prefix is stripped.
[[nodiscard]] std::string resolveSystemAliases(std::string_view p);

}  // namespace vortex::path
