// VortexClean core — unloading launchd services before their property lists are removed.
//
// launchd has no public C API for removing a job, so this is the one place the
// engine runs an external program: /bin/launchctl, started directly with
// posix_spawn(2) and an explicit argument vector — no shell is ever involved.
#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace vortex {

struct LaunchctlResult {
    bool spawned = false;     ///< launchctl was started
    int exitStatus = -1;      ///< its exit status (-1 when it did not run or timed out)
    bool timedOut = false;
    std::string serviceTarget;
    std::string detail;       ///< short human readable summary

    /// True when the service is gone afterwards: it was booted out, or was not loaded to begin with.
    [[nodiscard]] bool serviceIsGone() const noexcept;
};

class LaunchControl {
public:
    /// Labels are restricted to a conservative character set before they are
    /// ever placed on a command line.
    [[nodiscard]] static bool isValidLabel(std::string_view label) noexcept;

    [[nodiscard]] static std::string userServiceTarget(uid_t uid, std::string_view label);  ///< "gui/501/<label>"
    [[nodiscard]] static std::string systemServiceTarget(std::string_view label);           ///< "system/<label>"

    /// Runs `launchctl bootout <serviceTarget>` and waits (event-driven) for it to finish.
    [[nodiscard]] static LaunchctlResult bootout(const std::string& serviceTarget,
                                                 std::chrono::milliseconds timeout = std::chrono::seconds(15));
};

}  // namespace vortex
