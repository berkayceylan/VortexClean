// VortexClean core — live process inspection and controlled termination.
//
// Built on libproc (proc_listallpids / proc_pidpath / proc_pidinfo) for
// discovery and on kqueue EVFILT_PROC for waiting, so nothing here polls.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

#include "BundleInspector.hpp"
#include "Cancellation.hpp"

namespace vortex {

struct RunningProcess {
    pid_t pid = 0;
    pid_t parentPid = 0;
    uid_t uid = 0;
    std::string executablePath;
    std::string name;
    /// Start time; together with the pid and path this identifies one process
    /// instance, so a recycled pid is never signalled by mistake.
    std::int64_t startSeconds = 0;
    std::int64_t startMicroseconds = 0;

    bool insideBundle = false;      ///< the executable lives in the target's .app
    bool isMainExecutable = false;  ///< it is the bundle's CFBundleExecutable
    bool translocated = false;      ///< running from a Gatekeeper App Translocation mount
};

class ProcessInspector {
public:
    /// Every process whose executable path can be read.
    [[nodiscard]] static std::vector<RunningProcess> snapshot();

    /// Selects, from `all`, the processes that belong to `app`: executables inside
    /// its bundle (including a translocated copy of it) and any executable listed
    /// in `extraExecutables` (helper tools referenced by its launchd jobs).
    /// The calling process itself is never returned.
    [[nodiscard]] static std::vector<RunningProcess> filterRelated(const std::vector<RunningProcess>& all,
                                                                   const AppInfo& app,
                                                                   const std::vector<std::string>& extraExecutables);

    /// snapshot() + filterRelated().
    [[nodiscard]] static std::vector<RunningProcess> findRelated(const AppInfo& app,
                                                                 const std::vector<std::string>& extraExecutables = {});

    struct SignalOutcome {
        std::vector<pid_t> exited;        ///< gone (or already gone) by the end of the grace period
        std::vector<pid_t> stillRunning;  ///< survived the grace period
        std::vector<pid_t> notPermitted;  ///< owned by another user: needs root
    };

    /// Sends `signal` (0 = just wait) to every process that is still the same
    /// instance that was inspected, then waits up to `grace` for them to exit.
    [[nodiscard]] static SignalOutcome signalAndWait(const std::vector<RunningProcess>& processes, int signal,
                                                     std::chrono::milliseconds grace,
                                                     const CancellationToken& token = {});

    /// True while the pid still refers to the process instance described by `process`.
    [[nodiscard]] static bool isSameProcessAlive(const RunningProcess& process);
};

}  // namespace vortex
