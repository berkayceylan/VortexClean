// VortexClean core — the safety policy every deletion candidate must pass.
//
// Two independent layers use this class: the scanner (so protected items are
// shown locked) and the deletion engine (so a path can never be removed just
// because some caller asked for it).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vortex {

enum class Protection {
    None,           ///< may be removed
    SipProtected,   ///< System Integrity Protection: restricted flag or a protected system location
    ProtectedRoot,  ///< a shared standard directory (/Applications, ~/Library/Caches, ...) that is never removed
    Invalid,        ///< not an absolute, fully normalised path
};

[[nodiscard]] const char* toString(Protection protection) noexcept;

class PathPolicy {
public:
    /// `systemRoot` is the directory that plays the role of "/". It is "/" in
    /// production; the test-suite passes a scratch directory so that the real
    /// rules for /Library, /System, /usr ... can be exercised on a synthetic tree.
    explicit PathPolicy(std::string homeDirectory, std::string systemRoot = "/");

    /// Classification from the path text alone — no disk access.
    [[nodiscard]] Protection classifyLexically(std::string_view absolutePath) const;

    /// Lexical classification plus the SIP "restricted" file flag of the item
    /// itself and of the directory that contains it.
    [[nodiscard]] Protection classify(const std::string& absolutePath) const;

    /// True for locations that SIP guards regardless of file flags. Expects a
    /// normalised path in which the root aliases have already been resolved.
    [[nodiscard]] static bool isSipLocation(std::string_view normalizedPath) noexcept;

    /// isSipLocation() for a path on disk, honouring the configured system root.
    [[nodiscard]] bool isSystemLocation(std::string_view absolutePath) const;

    /// True for directories that are shared infrastructure and must never be
    /// deleted (/Applications, ~/Library/Caches, ...), honouring the system root.
    [[nodiscard]] bool isProtectedRoot(std::string_view absolutePath) const;

    /// SF_RESTRICTED or SF_NOUNLINK in the item's own st_flags: it may not be removed.
    [[nodiscard]] static bool hasRestrictedFlag(std::uint32_t stFlags) noexcept;

    /// True when the directory holding `absolutePath` is SIP-restricted, which
    /// protects everything directly inside it.
    [[nodiscard]] static bool containerIsRestricted(const std::string& absolutePath);

    /// SIP verdict for one path: a protected location, or the restricted flag on
    /// the item or on the directory holding it. Touches the disk (two lstat calls).
    [[nodiscard]] static bool isSipProtected(const std::string& absolutePath);

    /// True when removing the entry needs privileges the calling user lacks:
    /// the parent directory is not writable, a sticky parent holds someone
    /// else's entry, or the entry is a directory the user cannot modify.
    [[nodiscard]] bool requiresRoot(const std::string& absolutePath) const;

    /// The home directory as given (a real on-disk path).
    [[nodiscard]] const std::string& home() const noexcept { return actualHome_; }

    /// Maps an on-disk path into the logical namespace rooted at "/" (i.e. with
    /// the system root stripped) and resolves the /etc, /var, /tmp and
    /// data-volume aliases.
    [[nodiscard]] std::string logicalPath(std::string_view absolutePath) const;

private:
    [[nodiscard]] bool isProtectedLogical(std::string_view logicalPath) const;

    std::string actualHome_;
    std::string systemRoot_;
    std::string home_;  ///< home directory in the logical namespace
};

}  // namespace vortex
