#include "ProcessInspector.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <libproc.h>
#include <sys/event.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

#include "PathUtil.hpp"
#include "StringUtil.hpp"

namespace vortex {

namespace {

constexpr std::string_view kTranslocationMarker = "/AppTranslocation/";

/// Fills in the identity fields of one process; false when the pid is gone or opaque.
bool describe(pid_t pid, RunningProcess& out) {
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> pathBuffer{};
    const int pathLength = ::proc_pidpath(pid, pathBuffer.data(), static_cast<std::uint32_t>(pathBuffer.size()));
    if (pathLength <= 0) return false;

    out = RunningProcess{};
    out.pid = pid;
    out.executablePath.assign(pathBuffer.data(), static_cast<std::size_t>(pathLength));
    out.name = std::string(path::fileName(out.executablePath));

    struct proc_bsdinfo info {};
    if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, static_cast<int>(sizeof(info))) ==
        static_cast<int>(sizeof(info))) {
        out.parentPid = static_cast<pid_t>(info.pbi_ppid);
        out.uid = info.pbi_uid;
        out.startSeconds = static_cast<std::int64_t>(info.pbi_start_tvsec);
        out.startMicroseconds = static_cast<std::int64_t>(info.pbi_start_tvusec);
        return true;
    }

    // Fallback for processes whose BSD info is not exposed through libproc.
    std::array<int, 4> mib{CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    struct kinfo_proc kinfo {};
    std::size_t size = sizeof(kinfo);
    if (::sysctl(mib.data(), static_cast<u_int>(mib.size()), &kinfo, &size, nullptr, 0) == 0 &&
        size >= sizeof(kinfo)) {
        out.parentPid = kinfo.kp_eproc.e_ppid;
        out.uid = kinfo.kp_eproc.e_ucred.cr_uid;
        out.startSeconds = static_cast<std::int64_t>(kinfo.kp_proc.p_starttime.tv_sec);
        out.startMicroseconds = static_cast<std::int64_t>(kinfo.kp_proc.p_starttime.tv_usec);
    }
    return true;
}

/// For ".../AppTranslocation/<UUID>/d/Foo.app/Contents/MacOS/Foo" returns ".../d/Foo.app".
std::string translocatedBundleOf(std::string_view executablePath) {
    const std::size_t marker = executablePath.find(kTranslocationMarker);
    if (marker == std::string_view::npos) return {};
    const std::size_t app = executablePath.find(".app/", marker);
    if (app == std::string_view::npos) return {};
    return std::string(executablePath.substr(0, app + 4));
}

}  // namespace

std::vector<RunningProcess> ProcessInspector::snapshot() {
    std::vector<RunningProcess> processes;
    const int estimate = ::proc_listallpids(nullptr, 0);
    if (estimate <= 0) return processes;

    // Leave headroom for processes that start between the two calls.
    std::vector<pid_t> pids(static_cast<std::size_t>(estimate) + 128);
    const int count = ::proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (count <= 0) return processes;
    pids.resize(std::min(pids.size(), static_cast<std::size_t>(count)));

    processes.reserve(pids.size());
    for (const pid_t pid : pids) {
        if (pid <= 0) continue;
        RunningProcess process;
        if (describe(pid, process)) processes.push_back(std::move(process));
    }
    std::sort(processes.begin(), processes.end(),
              [](const RunningProcess& a, const RunningProcess& b) { return a.pid < b.pid; });
    return processes;
}

std::vector<RunningProcess> ProcessInspector::filterRelated(const std::vector<RunningProcess>& all,
                                                            const AppInfo& app,
                                                            const std::vector<std::string>& extraExecutables) {
    std::vector<RunningProcess> related;
    if (app.bundlePath.empty()) return related;
    const pid_t self = ::getpid();
    const std::string_view bundleFileName = path::fileName(app.bundlePath);
    // Translocated bundles need their Info.plist read; cache the verdict per bundle path.
    std::unordered_map<std::string, bool> translocationVerdicts;

    for (const RunningProcess& candidate : all) {
        if (candidate.pid == self) continue;
        RunningProcess process = candidate;
        process.insideBundle = path::iIsStrictlyWithin(process.executablePath, app.bundlePath);

        if (!process.insideBundle) {
            const std::string translocated = translocatedBundleOf(process.executablePath);
            if (!translocated.empty() && str::iequals(path::fileName(translocated), bundleFileName)) {
                auto found = translocationVerdicts.find(translocated);
                if (found == translocationVerdicts.end()) {
                    // Same file name is not enough: the identifiers must agree too.
                    const bool same = !app.bundleId.empty() &&
                                      str::iequals(BundleInspector::bundleIdentifier(translocated), app.bundleId);
                    found = translocationVerdicts.emplace(translocated, same).first;
                }
                if (found->second) {
                    process.insideBundle = true;
                    process.translocated = true;
                }
            }
        }

        bool isExtra = false;
        if (!process.insideBundle) {
            isExtra = std::any_of(extraExecutables.begin(), extraExecutables.end(),
                                  [&process](const std::string& extra) {
                                      return !extra.empty() && str::iequals(extra, process.executablePath);
                                  });
        }
        if (!process.insideBundle && !isExtra) continue;

        if (process.insideBundle) {
            process.isMainExecutable =
                process.translocated ? (process.name == app.executableName)
                                     : str::iequals(process.executablePath, app.executablePath);
        }
        related.push_back(std::move(process));
    }

    // Main executable first, then by pid.
    std::stable_sort(related.begin(), related.end(), [](const RunningProcess& a, const RunningProcess& b) {
        if (a.isMainExecutable != b.isMainExecutable) return a.isMainExecutable;
        return a.pid < b.pid;
    });
    return related;
}

std::vector<RunningProcess> ProcessInspector::findRelated(const AppInfo& app,
                                                          const std::vector<std::string>& extraExecutables) {
    return filterRelated(snapshot(), app, extraExecutables);
}

bool ProcessInspector::isSameProcessAlive(const RunningProcess& process) {
    RunningProcess current;
    if (!describe(process.pid, current)) return false;
    if (current.executablePath != process.executablePath) return false;
    // Start times are only compared when both sides actually know them.
    const bool haveBoth = (process.startSeconds != 0) && (current.startSeconds != 0);
    return !haveBoth || (current.startSeconds == process.startSeconds &&
                         current.startMicroseconds == process.startMicroseconds);
}

ProcessInspector::SignalOutcome ProcessInspector::signalAndWait(const std::vector<RunningProcess>& processes,
                                                                int signal, std::chrono::milliseconds grace,
                                                                const CancellationToken& token) {
    SignalOutcome outcome;
    const int queue = ::kqueue();
    std::unordered_set<pid_t> watched;    // exit will be reported through the kqueue
    std::unordered_set<pid_t> unwatched;  // could not be registered: checked with isSameProcessAlive
    std::unordered_map<pid_t, const RunningProcess*> byPid;

    for (const RunningProcess& process : processes) {
        if (process.pid <= 1 || process.pid == ::getpid()) continue;  // never launchd, never ourselves
        if (!isSameProcessAlive(process)) {
            outcome.exited.push_back(process.pid);
            continue;
        }
        byPid[process.pid] = &process;

        // Register for the exit notification *before* signalling so it cannot be missed.
        bool registered = false;
        if (queue >= 0) {
            struct kevent change {};
            EV_SET(&change, static_cast<uintptr_t>(process.pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
                   nullptr);
            if (::kevent(queue, &change, 1, nullptr, 0, nullptr) == 0) {
                registered = true;
            } else if (errno == ESRCH) {
                outcome.exited.push_back(process.pid);
                continue;
            }
        }

        if (signal != 0 && ::kill(process.pid, signal) != 0) {
            const int err = errno;
            if (err == ESRCH) {
                outcome.exited.push_back(process.pid);
                continue;
            }
            if (err == EPERM) {
                outcome.notPermitted.push_back(process.pid);
                continue;
            }
        }
        (registered ? watched : unwatched).insert(process.pid);
    }

    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (!watched.empty() || !unwatched.empty()) {
        for (auto it = unwatched.begin(); it != unwatched.end();) {
            if (!isSameProcessAlive(*byPid[*it])) {
                outcome.exited.push_back(*it);
                it = unwatched.erase(it);
            } else {
                ++it;
            }
        }
        if (watched.empty() && unwatched.empty()) break;
        if (token.isCancelled()) break;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        // Sleep in the kernel until a watched process exits. The wait is sliced so
        // that cancellation and unwatched processes are noticed promptly; it only
        // runs for the bounded grace period of an explicit user action.
        const auto slice = std::min<std::chrono::nanoseconds>(deadline - now, std::chrono::milliseconds(200));
        struct timespec timeout {};
        timeout.tv_sec = static_cast<time_t>(std::chrono::duration_cast<std::chrono::seconds>(slice).count());
        timeout.tv_nsec = static_cast<long>((slice % std::chrono::seconds(1)).count());

        if (queue < 0 || watched.empty()) {
            ::nanosleep(&timeout, nullptr);
            continue;
        }
        std::array<struct kevent, 16> events{};
        const int ready = ::kevent(queue, nullptr, 0, events.data(), static_cast<int>(events.size()), &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < ready; ++i) {
            const auto pid = static_cast<pid_t>(events[static_cast<std::size_t>(i)].ident);
            if (watched.erase(pid) != 0) outcome.exited.push_back(pid);
        }
    }
    if (queue >= 0) ::close(queue);

    for (const pid_t pid : watched) {
        (isSameProcessAlive(*byPid[pid]) ? outcome.stillRunning : outcome.exited).push_back(pid);
    }
    for (const pid_t pid : unwatched) {
        (isSameProcessAlive(*byPid[pid]) ? outcome.stillRunning : outcome.exited).push_back(pid);
    }
    std::sort(outcome.exited.begin(), outcome.exited.end());
    std::sort(outcome.stillRunning.begin(), outcome.stillRunning.end());
    std::sort(outcome.notPermitted.begin(), outcome.notPermitted.end());
    return outcome;
}

}  // namespace vortex
