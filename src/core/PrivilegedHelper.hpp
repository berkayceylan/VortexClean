// VortexClean core — the privileged side of root-required removals.
//
// Locally built, ad-hoc signed apps cannot use SMJobBless/SMAppService daemons
// (those require a Developer ID signature), so elevation works like this:
//
//   1. the app asks Authorization Services for the admin right (password dialog),
//   2. it re-launches its own executable as root with `--privileged-helper`,
//   3. app and helper exchange exactly one request and one response as
//      length-prefixed JSON over the pipe that connects them.
//
// The helper never trusts the request: every path is re-validated against the
// same PathPolicy the app uses plus a fixed allow-list of locations, paths that
// contain a symlink are refused (O_NOFOLLOW_ANY), and nothing is ever chown'ed.
// This file is pure C++ so the whole protocol and policy can be unit-tested
// without privileges against a scratch directory.
#pragma once

#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "DeletionEngine.hpp"
#include "Json.hpp"
#include "LaunchControl.hpp"
#include "Result.hpp"

namespace vortex {

struct PrivilegedRequest {
    DeleteMode mode = DeleteMode::Trash;
    uid_t invokerUid = 0;
    std::vector<DeletionItem> items;
};

struct PrivilegedResponse {
    pid_t helperPid = 0;
    std::string error;  ///< protocol-level failure; empty when the request was executed
    std::vector<ItemOutcome> outcomes;
};

namespace privileged {

inline constexpr const char* kHelperArgument = "--privileged-helper";
inline constexpr std::size_t kMaxFrameBytes = std::size_t{32} << 20;

[[nodiscard]] JsonValue encodeRequest(const PrivilegedRequest& request);
[[nodiscard]] Result<PrivilegedRequest> decodeRequest(const JsonValue& json);
[[nodiscard]] JsonValue encodeResponse(const PrivilegedResponse& response);
[[nodiscard]] Result<PrivilegedResponse> decodeResponse(const JsonValue& json);

/// Framing used on the pipe: "<decimal byte count>\n" followed by that many bytes.
[[nodiscard]] Status writeFrame(std::FILE* stream, std::string_view payload);
[[nodiscard]] Result<std::string> readFrame(std::FILE* stream, std::size_t maxBytes = kMaxFrameBytes);

/// Where the helper is allowed to operate.
struct HelperPolicy {
    std::string systemRoot = "/";  ///< "/" in production; a scratch directory in tests
    std::string invokerHome;       ///< home directory of the user who authorised the operation
    uid_t invokerUid = 0;

    /// Policy for the real system, derived from the password database.
    [[nodiscard]] static Result<HelperPolicy> production(uid_t invokerUid);
    [[nodiscard]] std::string trashDirectory() const;
};

struct HelperHooks {
    /// Defaults to LaunchControl::bootout when unset.
    std::function<LaunchctlResult(const std::string& serviceTarget)> bootout;
};

/// True when `absolutePath` lies strictly inside one of the locations the helper
/// may modify: /Applications, /Library, /usr/local, /opt, /Users/Shared,
/// /private/var/db/receipts, and the invoking user's own Library and Applications.
[[nodiscard]] bool isLocationAllowed(const HelperPolicy& policy, const std::string& absolutePath);

/// Executes a request with the privileges of the calling process.
[[nodiscard]] std::vector<ItemOutcome> execute(const PrivilegedRequest& request, const HelperPolicy& policy,
                                               const HelperHooks& hooks = {});

/// Body of `VortexClean --privileged-helper`: reads one request frame from `in`,
/// executes it, writes one response frame to `out`. Returns the exit status.
[[nodiscard]] int runHelperMain(std::FILE* in, std::FILE* out);

}  // namespace privileged
}  // namespace vortex
