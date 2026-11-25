#include "LaunchControl.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <unistd.h>

#include "FileUtil.hpp"

namespace vortex {

namespace {

constexpr const char* kLaunchctlPath = "/bin/launchctl";

// launchctl exit statuses that mean "there was nothing to unload".
constexpr int kExitNoSuchProcess = 3;     // ESRCH: service not loaded in that domain
constexpr int kExitServiceNotFound = 113; // "Could not find specified service"
constexpr int kExitInProgress = 36;       // EINPROGRESS: boot-out already under way

}  // namespace

bool LaunchctlResult::serviceIsGone() const noexcept {
    return spawned && !timedOut &&
           (exitStatus == 0 || exitStatus == kExitNoSuchProcess || exitStatus == kExitServiceNotFound ||
            exitStatus == kExitInProgress);
}

bool LaunchControl::isValidLabel(std::string_view label) noexcept {
    if (label.empty() || label.size() > 255) return false;
    if (label.front() == '-' || label.front() == '.') return false;
    for (const char ch : label) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                        ch == '.' || ch == '-' || ch == '_';
        if (!ok) return false;
    }
    return true;
}

std::string LaunchControl::userServiceTarget(uid_t uid, std::string_view label) {
    return "gui/" + std::to_string(uid) + "/" + std::string(label);
}

std::string LaunchControl::systemServiceTarget(std::string_view label) {
    return "system/" + std::string(label);
}

LaunchctlResult LaunchControl::bootout(const std::string& serviceTarget, std::chrono::milliseconds timeout) {
    LaunchctlResult result;
    result.serviceTarget = serviceTarget;

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        result.detail = "cannot prepare launchctl";
        return result;
    }
    // launchctl's chatter is not needed; give it /dev/null on all three standard descriptors.
    ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    std::string argv0 = "launchctl";
    std::string argv1 = "bootout";
    std::string argv2 = serviceTarget;
    char* const argv[] = {argv0.data(), argv1.data(), argv2.data(), nullptr};
    std::string environmentPath = "PATH=/usr/bin:/bin:/usr/sbin:/sbin";
    char* const envp[] = {environmentPath.data(), nullptr};

    pid_t child = 0;
    const int spawnError = ::posix_spawn(&child, kLaunchctlPath, &actions, nullptr, argv, envp);
    ::posix_spawn_file_actions_destroy(&actions);
    if (spawnError != 0) {
        result.detail = "cannot start launchctl: " + io::errnoMessage(spawnError);
        return result;
    }
    result.spawned = true;

    // Wait for the child with a kqueue so a hung launchctl can be abandoned after the timeout.
    bool exited = false;
    const int queue = ::kqueue();
    if (queue >= 0) {
        struct kevent change {};
        EV_SET(&change, static_cast<uintptr_t>(child), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        if (::kevent(queue, &change, 1, nullptr, 0, nullptr) == 0) {
            struct timespec limit {};
            limit.tv_sec = static_cast<time_t>(timeout.count() / 1000);
            limit.tv_nsec = static_cast<long>((timeout.count() % 1000) * 1000000);
            struct kevent event {};
            int ready = 0;
            do {
                ready = ::kevent(queue, nullptr, 0, &event, 1, &limit);
            } while (ready < 0 && errno == EINTR);
            exited = ready > 0;
        } else {
            exited = true;  // ESRCH: it already finished; waitpid below collects it
        }
        ::close(queue);
    } else {
        exited = true;  // no kqueue: fall back to a plain blocking wait
    }

    if (!exited) {
        ::kill(child, SIGKILL);
        result.timedOut = true;
    }
    int status = 0;
    pid_t waited = 0;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (result.timedOut) {
        result.detail = "launchctl bootout timed out";
    } else if (waited == child && WIFEXITED(status)) {
        result.exitStatus = WEXITSTATUS(status);
        if (result.exitStatus == 0) {
            result.detail = "service booted out";
        } else if (result.exitStatus == kExitNoSuchProcess || result.exitStatus == kExitServiceNotFound) {
            result.detail = "service was not loaded";
        } else if (result.exitStatus == kExitInProgress) {
            result.detail = "boot-out already in progress";
        } else {
            result.detail = "launchctl bootout exited with status " + std::to_string(result.exitStatus);
        }
    } else {
        result.detail = "launchctl terminated abnormally";
    }
    return result;
}

}  // namespace vortex
