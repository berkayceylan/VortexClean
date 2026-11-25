// Tests for live process inspection: libproc snapshots, attribution of
// processes to a bundle (including Gatekeeper-translocated copies), and the
// SIGTERM -> grace period -> SIGKILL escalation, exercised on real child processes.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "BundleInspector.hpp"
#include "PathUtil.hpp"
#include "ProcessInspector.hpp"
#include "TestSupport.hpp"

#ifndef VORTEX_TEST_CHILD_PATH
#error "VORTEX_TEST_CHILD_PATH must point at the process_child executable"
#endif

using namespace vortex;

namespace {

/// Starts the child executable found at `executable` and waits until it reports "ready".
pid_t launch(const std::string& executable, const char* mode) {
    int channel[2];
    if (::pipe(channel) != 0) return -1;

    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, channel[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, channel[0]);
    ::posix_spawn_file_actions_addclose(&actions, channel[1]);

    std::string argv0 = executable;
    std::string argv1 = mode != nullptr ? mode : "";
    char* const argv[] = {argv0.data(), mode != nullptr ? argv1.data() : nullptr, nullptr};
    char* const envp[] = {nullptr};
    pid_t pid = -1;
    const int error = ::posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv, envp);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(channel[1]);
    if (error != 0) {
        ::close(channel[0]);
        errno = error;
        return -1;
    }
    char buffer[16] = {};
    ssize_t got = 0;
    do {
        got = ::read(channel[0], buffer, sizeof(buffer) - 1);
    } while (got < 0 && errno == EINTR);
    ::close(channel[0]);
    return got > 0 ? pid : -1;
}

/// Reaps a child that the test terminated, so no zombie is left behind.
void reap(pid_t pid) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

std::string installChild(const std::string& bundle, const std::string& executableName) {
    const std::string destination = bundle + "/Contents/MacOS/" + executableName;
    std::error_code ec;
    std::filesystem::remove(destination, ec);
    std::filesystem::copy_file(VORTEX_TEST_CHILD_PATH, destination, ec);
    ::chmod(destination.c_str(), 0755);
    return ec ? std::string() : destination;
}

AppInfo appAt(const std::string& bundle, const std::string& bundleId, const std::string& executable) {
    AppInfo app;
    app.bundlePath = bundle;
    app.bundleId = bundleId;
    app.displayName = "Fake";
    app.executableName = executable;
    app.executablePath = bundle + "/Contents/MacOS/" + executable;
    return app;
}

RunningProcess fakeProcess(pid_t pid, const std::string& executablePath) {
    RunningProcess process;
    process.pid = pid;
    process.executablePath = executablePath;
    process.name = std::string(path::fileName(executablePath));
    return process;
}

bool containsPid(const std::vector<pid_t>& list, pid_t pid) {
    return std::find(list.begin(), list.end(), pid) != list.end();
}

}  // namespace

VT_TEST(snapshot_lists_this_process) {
    const std::vector<RunningProcess> all = ProcessInspector::snapshot();
    VT_REQUIRE(!all.empty());
    const pid_t self = ::getpid();
    const auto found = std::find_if(all.begin(), all.end(), [self](const RunningProcess& p) { return p.pid == self; });
    VT_REQUIRE(found != all.end());
    VT_CHECK(found->executablePath.front() == '/');
    VT_CHECK(found->executablePath.find("test_process_inspector") != std::string::npos);
    VT_CHECK_EQ(found->uid, ::getuid());
    VT_CHECK_EQ(found->parentPid, ::getppid());
    VT_CHECK(found->startSeconds > 1'600'000'000);
    VT_CHECK(ProcessInspector::isSameProcessAlive(*found));

    // The same pid with a different identity is recognised as a different process.
    RunningProcess impostor = *found;
    impostor.startSeconds -= 100;
    VT_CHECK(!ProcessInspector::isSameProcessAlive(impostor));
    impostor = *found;
    impostor.executablePath = "/usr/bin/true";
    VT_CHECK(!ProcessInspector::isSameProcessAlive(impostor));
    VT_CHECK(!ProcessInspector::isSameProcessAlive(fakeProcess(0x7FFFFFF0, "/nonexistent")));

    VT_CHECK(std::is_sorted(all.begin(), all.end(),
                            [](const RunningProcess& a, const RunningProcess& b) { return a.pid < b.pid; }));
    vt::note(std::to_string(all.size()) + " processes visible through libproc");
}

VT_TEST(processes_are_attributed_to_their_bundle) {
    const vt::TempDir temp("vortex-proc");
    const std::string bundle = vt::makeAppBundle(temp.path(), "Fake.app", "com.acme.fake", "Fake");
    const AppInfo app = appAt(bundle, "com.acme.fake", "Fake");

    // A translocated copy of the same app, and a translocated app that merely shares the file name.
    const std::string translocated = temp.sub("T/AppTranslocation/1111-2222/d");
    vt::makeAppBundle(translocated, "Fake.app", "com.acme.fake", "Fake");
    const std::string foreign = temp.sub("T/AppTranslocation/3333-4444/d");
    vt::makeAppBundle(foreign, "Fake.app", "org.other.fake", "Fake");

    const std::vector<RunningProcess> all{
        fakeProcess(100, bundle + "/Contents/MacOS/Fake"),
        fakeProcess(101, bundle + "/Contents/Frameworks/Fake Helper.app/Contents/MacOS/Fake Helper"),
        fakeProcess(102, bundle + "/Contents/XPCServices/Worker.xpc/Contents/MacOS/Worker"),
        fakeProcess(103, temp.sub("Fake.app.backup/Contents/MacOS/Fake")),      // look-alike directory
        fakeProcess(104, temp.sub("Other.app/Contents/MacOS/Fake")),            // same name, other bundle
        fakeProcess(105, translocated + "/Fake.app/Contents/MacOS/Fake"),
        fakeProcess(106, foreign + "/Fake.app/Contents/MacOS/Fake"),
        fakeProcess(107, "/Library/PrivilegedHelperTools/com.acme.faked"),
        fakeProcess(108, "/usr/libexec/unrelated"),
        fakeProcess(::getpid(), bundle + "/Contents/MacOS/Fake"),                // never report ourselves
    };
    const std::vector<RunningProcess> related =
        ProcessInspector::filterRelated(all, app, {"/Library/PrivilegedHelperTools/com.acme.faked", ""});

    std::vector<pid_t> pids;
    for (const RunningProcess& process : related) pids.push_back(process.pid);
    VT_CHECK_EQ(pids, (std::vector<pid_t>{100, 105, 101, 102, 107}));  // main executables first

    const auto byPid = [&](pid_t pid) -> const RunningProcess& {
        return *std::find_if(related.begin(), related.end(), [pid](const RunningProcess& p) { return p.pid == pid; });
    };
    VT_CHECK(byPid(100).insideBundle && byPid(100).isMainExecutable && !byPid(100).translocated);
    VT_CHECK(byPid(101).insideBundle && !byPid(101).isMainExecutable);
    VT_CHECK(byPid(105).insideBundle && byPid(105).translocated && byPid(105).isMainExecutable);
    VT_CHECK(!byPid(107).insideBundle && !byPid(107).isMainExecutable);

    VT_CHECK(ProcessInspector::filterRelated(all, AppInfo{}, {}).empty());
}

VT_TEST(sigterm_stops_a_cooperative_process) {
    const vt::TempDir temp("vortex-proc");
    const std::string bundle = vt::makeAppBundle(temp.path(), "Fake.app", "com.acme.fake", "Fake");
    const std::string executable = installChild(bundle, "Fake");
    VT_REQUIRE(!executable.empty());
    const AppInfo app = appAt(bundle, "com.acme.fake", "Fake");

    const pid_t pid = launch(executable, nullptr);
    VT_REQUIRE(pid > 0);

    const std::vector<RunningProcess> related = ProcessInspector::findRelated(app);
    VT_REQUIRE(related.size() == 1);
    VT_CHECK_EQ(related.front().pid, pid);
    VT_CHECK_EQ(related.front().executablePath, executable);
    VT_CHECK(related.front().isMainExecutable);
    VT_CHECK_EQ(related.front().uid, ::getuid());
    VT_CHECK_EQ(related.front().parentPid, ::getpid());

    const auto started = std::chrono::steady_clock::now();
    const auto outcome = ProcessInspector::signalAndWait(related, SIGTERM, std::chrono::seconds(10));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    VT_CHECK(containsPid(outcome.exited, pid));
    VT_CHECK(outcome.stillRunning.empty());
    VT_CHECK(outcome.notPermitted.empty());
    VT_CHECK(ms < 5000.0);  // woken by the kernel's exit notification, not by a timeout
    reap(pid);
    VT_CHECK(ProcessInspector::findRelated(app).empty());
    vt::note("SIGTERM honoured after " + std::to_string(ms) + " ms");

    // Signalling a process that is already gone is reported as "exited", not as an error.
    const auto again = ProcessInspector::signalAndWait(related, SIGTERM, std::chrono::milliseconds(200));
    VT_CHECK(containsPid(again.exited, pid));
    VT_CHECK(again.stillRunning.empty());
}

VT_TEST(a_stubborn_process_survives_sigterm_and_needs_escalation) {
    const vt::TempDir temp("vortex-proc");
    const std::string bundle = vt::makeAppBundle(temp.path(), "Fake.app", "com.acme.fake", "Fake");
    const std::string executable = installChild(bundle, "Fake");
    const std::string helper = installChild(bundle, "FakeHelper");
    VT_REQUIRE(!executable.empty() && !helper.empty());
    const AppInfo app = appAt(bundle, "com.acme.fake", "Fake");

    const pid_t stubborn = launch(executable, "stubborn");
    const pid_t cooperative = launch(helper, nullptr);
    VT_REQUIRE(stubborn > 0 && cooperative > 0);

    const std::vector<RunningProcess> related = ProcessInspector::findRelated(app);
    VT_REQUIRE(related.size() == 2);
    VT_CHECK_EQ(related.front().pid, stubborn);  // the main executable is listed first

    // Phase 1 — graceful: the helper quits, the main process ignores the request.
    const auto graceful = ProcessInspector::signalAndWait(related, SIGTERM, std::chrono::milliseconds(700));
    VT_CHECK(containsPid(graceful.exited, cooperative));
    VT_CHECK(containsPid(graceful.stillRunning, stubborn));
    VT_CHECK(!containsPid(graceful.exited, stubborn));
    reap(cooperative);
    VT_CHECK(ProcessInspector::isSameProcessAlive(related.front()));

    // Waiting without a signal reports the same state and returns when the grace period ends.
    const auto idle = ProcessInspector::signalAndWait({related.front()}, 0, std::chrono::milliseconds(150));
    VT_CHECK(containsPid(idle.stillRunning, stubborn));

    // Phase 2 — the escalation the UI only performs after explicit confirmation.
    const auto forced = ProcessInspector::signalAndWait({related.front()}, SIGKILL, std::chrono::seconds(10));
    VT_CHECK(containsPid(forced.exited, stubborn));
    VT_CHECK(forced.stillRunning.empty());
    reap(stubborn);
    VT_CHECK(ProcessInspector::findRelated(app).empty());
}

VT_TEST(signalling_refuses_recycled_and_protected_pids) {
    const vt::TempDir temp("vortex-proc");
    const std::string bundle = vt::makeAppBundle(temp.path(), "Fake.app", "com.acme.fake", "Fake");
    const std::string executable = installChild(bundle, "Fake");
    VT_REQUIRE(!executable.empty());
    const AppInfo app = appAt(bundle, "com.acme.fake", "Fake");
    const pid_t pid = launch(executable, nullptr);
    VT_REQUIRE(pid > 0);
    const std::vector<RunningProcess> related = ProcessInspector::findRelated(app);
    VT_REQUIRE(related.size() == 1);

    // A record whose identity no longer matches the pid ("the process that used
    // to have this pid") is reported as gone and is never signalled.
    RunningProcess stale = related.front();
    stale.startSeconds -= 1000;
    const auto recycled = ProcessInspector::signalAndWait({stale}, SIGKILL, std::chrono::milliseconds(100));
    VT_CHECK(containsPid(recycled.exited, pid));
    VT_CHECK(ProcessInspector::isSameProcessAlive(related.front()));  // the real process was not killed

    RunningProcess renamed = related.front();
    renamed.executablePath = "/usr/bin/true";
    const auto mismatched = ProcessInspector::signalAndWait({renamed}, SIGKILL, std::chrono::milliseconds(100));
    VT_CHECK(containsPid(mismatched.exited, pid));
    VT_CHECK(ProcessInspector::isSameProcessAlive(related.front()));

    const auto stopped = ProcessInspector::signalAndWait(related, SIGTERM, std::chrono::seconds(10));
    VT_CHECK(containsPid(stopped.exited, pid));
    reap(pid);

    // The calling process and launchd (pid 1) are skipped outright.
    const std::vector<RunningProcess> all = ProcessInspector::snapshot();
    const pid_t self = ::getpid();
    const auto me = std::find_if(all.begin(), all.end(), [self](const RunningProcess& p) { return p.pid == self; });
    VT_REQUIRE(me != all.end());
    RunningProcess launchd;
    launchd.pid = 1;
    launchd.executablePath = "/sbin/launchd";
    const auto skipped = ProcessInspector::signalAndWait({*me, launchd}, SIGKILL, std::chrono::milliseconds(100));
    VT_CHECK(skipped.exited.empty());
    VT_CHECK(skipped.stillRunning.empty());
    VT_CHECK(skipped.notPermitted.empty());

    if (::geteuid() != 0) {
        // A process owned by root cannot be signalled by a normal user: reported, not hidden.
        const auto rootOwned = std::find_if(all.begin(), all.end(), [](const RunningProcess& p) {
            return p.uid == 0 && p.pid > 1 && p.startSeconds != 0;
        });
        if (rootOwned != all.end()) {
            const auto denied = ProcessInspector::signalAndWait({*rootOwned}, SIGTERM, std::chrono::milliseconds(50));
            VT_CHECK(denied.exited.empty());
            VT_CHECK(containsPid(denied.notPermitted, rootOwned->pid));
            VT_CHECK(ProcessInspector::isSameProcessAlive(*rootOwned));
        }
    }
}
