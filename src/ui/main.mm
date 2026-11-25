// VortexClean — process entry point.
//
// One executable, three roles:
//   1. `--privileged-helper`  the root-side half of an elevated removal (see
//                             core/PrivilegedHelper.hpp); no AppKit, no window.
//   2. headless commands      `--version`, `--list`, `--scan`, `--dry-run`.
//      (with `--root <dir>` in front, on a synthetic disk)
//   3. everything else        the AppKit application.
#import <AppKit/AppKit.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#import "AppDelegate.h"
#import "VCEngine.h"

#include "PrivilegedHelper.hpp"
#include "VCCommandLine.hpp"

int main(int argc, const char *argv[]) {
    // (1) Must be decided before anything else runs: in this mode the process is root.
    if (argc >= 2 && std::strcmp(argv[1], vortex::privileged::kHelperArgument) == 0) {
        return vortex::privileged::runHelperMain(stdin, stdout);
    }

    // `--root <dir>` comes first and applies to everything that follows it.
    vortex::ScanRoots roots = vortex::ScanRoots::systemDefault();
    std::string sandbox;
    if (argc >= 2 && std::strcmp(argv[1], "--root") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "VortexClean: --root needs a directory\n");
            return 2;
        }
        auto synthetic = vortex::ScanRoots::sandbox(argv[2]);
        if (!synthetic) {
            std::fprintf(stderr, "VortexClean: %s\n", synthetic.error().message.c_str());
            return 2;
        }
        roots = std::move(synthetic.value());
        sandbox = argv[2];
        argc -= 2;
        argv += 2;  // argv[0] is now the directory; only the arguments after it matter
    }

    // (2) Headless commands never touch the window server.
    int exitStatus = 0;
    if (vcbridge::runCommandLine(argc, argv, roots, exitStatus)) return exitStatus;

    // (3) The application.
    @autoreleasepool {
        if (!sandbox.empty()) [VCEngine useSandbox:[NSString stringWithUTF8String:sandbox.c_str()]];
        static AppDelegate *delegate;  // NSApplication holds its delegate weakly
        delegate = [[AppDelegate alloc] init];
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--launch-check") {
                delegate.launchCheck = YES;
            } else if (argument == "--empty") {
                delegate.launchCheckEmptyState = YES;
            } else if (argument == "--snapshot" && i + 1 < argc) {
                delegate.snapshotPath = [NSString stringWithUTF8String:argv[++i]];
            } else if (argument == "--hover" && i + 1 < argc) {
                delegate.hoverSegment = static_cast<NSInteger>(std::strtol(argv[++i], nullptr, 10));
            } else if (argument == "--appearance" && i + 1 < argc) {
                delegate.forcedAppearance = [NSString stringWithUTF8String:argv[++i]];
            } else if (argument.size() > 4 && argument.front() == '/' &&
                       argument.compare(argument.size() - 4, 4, ".app") == 0) {
                delegate.initialApplicationPath = [NSString stringWithUTF8String:argument.c_str()];
            }
        }
        NSApplication *application = [NSApplication sharedApplication];
        application.delegate = delegate;
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        [application run];
    }
    return 0;
}
