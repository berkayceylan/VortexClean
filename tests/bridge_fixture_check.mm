// Manual end-to-end check of the Objective-C++ bridge against a DISPOSABLE
// fixture application. It drives exactly the code path the interface uses
// (VCEngine: inspect -> scan -> dry run -> real clean), including the real
// NSFileManager Trash provider and the real /bin/launchctl, without any UI.
//
//   bridge_fixture_check <Fixture.app> trash|permanent
//
// This program really removes things, so it is deliberately not part of CTest
// and it refuses to continue unless every selected item is recognisably part of
// the fixture (its path must contain the fixture's bundle identifier or name).
#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#import "VCEngine.h"

#include "LaunchControl.hpp"

namespace {

int gFailures = 0;

void Check(bool condition, NSString *what) {
    std::printf("  [%s] %s\n", condition ? " ok " : "FAIL", what.UTF8String);
    if (!condition) ++gFailures;
}

[[noreturn]] void Finish() {
    std::printf("\n%s (%d failed check%s)\n", gFailures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", gFailures,
                gFailures == 1 ? "" : "s");
    std::fflush(stdout);
    std::exit(gFailures == 0 ? 0 : 1);
}

BOOL Exists(NSString *path) {
    return [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil] != nil;
}

}  // namespace

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3) {
            std::fprintf(stderr, "usage: %s <Fixture.app> trash|permanent\n", argv[0]);
            return 2;
        }
        NSString *fixturePath = [NSString stringWithUTF8String:argv[1]];
        NSString *modeName = [NSString stringWithUTF8String:argv[2]];
        const VCDeleteMode mode = [modeName isEqualToString:@"permanent"] ? VCDeleteModePermanent : VCDeleteModeTrash;
        VCEngine *engine = VCEngine.sharedEngine;

        std::printf("== environment\n");
        Check(engine.auditLogDirectory.length > 0,
              [NSString stringWithFormat:@"audit directory is %@", engine.auditLogDirectory]);
        std::printf("  [info] administrator elevation available on this system: %s\n",
                    engine.elevationAvailable ? "yes" : "no");

        // The real launchctl, asked to boot out a label that cannot exist: it must
        // run, report "not loaded", and that must be understood as "service is gone".
        std::printf("== launchctl bootout (non-existent label)\n");
        const std::string target =
            vortex::LaunchControl::userServiceTarget(::getuid(), "com.berkayceylan.VortexClean.fixture.no-such-service");
        const vortex::LaunchctlResult booted = vortex::LaunchControl::bootout(target);
        Check(booted.spawned, @"launchctl was started with posix_spawn");
        Check(!booted.timedOut && booted.serviceIsGone(),
              [NSString stringWithFormat:@"exit status %d is understood as: %s", booted.exitStatus,
                                         booted.detail.c_str()]);

        std::printf("== inspect %s\n", fixturePath.lastPathComponent.UTF8String);
        [engine inspectApplicationAtPath:fixturePath
                              completion:^(VCApp *app, NSString *error) {
            if (app == nil) {
                Check(false, [NSString stringWithFormat:@"inspect failed: %@", error]);
                Finish();
            }
            Check(app.bundleIdentifier.length > 0, [NSString stringWithFormat:@"bundle identifier %@", app.bundleIdentifier]);
            Check(app.isExternal, @"recognised as an application outside the Applications folders");
            NSString *identifier = app.bundleIdentifier;
            NSString *name = app.displayName;

            std::printf("== scan\n");
            [engine scanApplication:app
                discovered:^(NSUInteger scanID, VCScanResult *partial) {
                    (void)scanID;
                    std::printf("  [info] discovered %lu items\n", static_cast<unsigned long>(partial.remnants.count));
                }
                sized:^(NSUInteger scanID, NSArray<VCRemnant *> *updated) {
                    (void)scanID;
                    (void)updated;
                }
                completion:^(NSUInteger scanID, VCScanResult *result) {
                    (void)scanID;
                    NSMutableArray<VCRemnant *> *selection = [NSMutableArray array];
                    BOOL everythingIsFixture = YES;
                    for (VCRemnant *remnant in result.remnants) {
                        std::printf("  %s %-7s %-26s %s\n", remnant.defaultSelected ? "[x]" : "[ ]",
                                    remnant.confidenceName.UTF8String, remnant.reason.UTF8String,
                                    remnant.displayPath.UTF8String);
                        if (!remnant.defaultSelected) continue;
                        [selection addObject:remnant];
                        // Safety interlock: nothing but the fixture may ever be selected here.
                        if ([remnant.path rangeOfString:identifier].location == NSNotFound &&
                            [remnant.path rangeOfString:name].location == NSNotFound) {
                            everythingIsFixture = NO;
                        }
                    }
                    Check(!result.cancelled, @"scan completed");
                    Check(selection.count >= 2, [NSString stringWithFormat:@"%lu items selected by default",
                                                                           static_cast<unsigned long>(selection.count)]);
                    Check(everythingIsFixture, @"every selected item belongs to the fixture");
                    if (!everythingIsFixture || selection.count == 0) Finish();  // never delete anything else

                    std::printf("== dry run\n");
                    [engine cleanRemnants:selection
                            ofApplication:app
                                     mode:mode
                                   dryRun:YES
                                 progress:^(NSUInteger completed, NSUInteger total) {
                                     (void)completed;
                                     (void)total;
                                 }
                               completion:^(VCCleanReport *dry) {
                        Check(dry.dryRun, @"report is marked as a dry run");
                        Check([dry countWithStatus:VCItemStatusDryRun] == selection.count,
                              @"every item was validated and reported as dry_run");
                        BOOL intact = YES;
                        for (VCRemnant *remnant in selection) intact = intact && Exists(remnant.path);
                        Check(intact, @"nothing was touched");
                        Check(Exists(dry.auditLogPath), [NSString stringWithFormat:@"audit report %@", dry.auditLogPath.lastPathComponent]);

                        std::printf("== real clean (%s)\n", modeName.UTF8String);
                        [engine cleanRemnants:selection
                                ofApplication:app
                                         mode:mode
                                       dryRun:NO
                                     progress:^(NSUInteger completed, NSUInteger total) {
                                         (void)completed;
                                         (void)total;
                                     }
                                   completion:^(VCCleanReport *report) {
                            const VCItemStatus expected =
                                mode == VCDeleteModeTrash ? VCItemStatusTrashed : VCItemStatusDeleted;
                            NSUInteger removed = 0;
                            NSMutableArray<NSString *> *trashed = [NSMutableArray array];
                            for (VCCleanItemResult *item in report.items) {
                                std::printf("  %-18s %s%s%s\n", item.statusName.UTF8String, item.path.UTF8String,
                                            item.errorMessage.length > 0 ? "  -- " : "", item.errorMessage.UTF8String);
                                if (item.status == expected) ++removed;
                                if (item.trashPath.length > 0) [trashed addObject:item.trashPath];
                            }
                            Check(removed == selection.count,
                                  [NSString stringWithFormat:@"%lu of %lu items %@", static_cast<unsigned long>(removed),
                                                             static_cast<unsigned long>(selection.count),
                                                             mode == VCDeleteModeTrash ? @"moved to the Trash" : @"deleted"]);
                            BOOL gone = YES;
                            for (VCRemnant *remnant in selection) gone = gone && !Exists(remnant.path);
                            Check(gone, @"every selected item is gone from its original location");
                            Check(report.reclaimedBytes > 0,
                                  [NSString stringWithFormat:@"%llu bytes reported as reclaimed", report.reclaimedBytes]);
                            Check(Exists(report.auditLogPath), [NSString stringWithFormat:@"audit report %@", report.auditLogPath.lastPathComponent]);
                            Check(!report.privacyDenied, @"no privacy denial");

                            if (mode == VCDeleteModeTrash) {
                                Check(trashed.count == selection.count, @"every item reports where it went in the Trash");
                                BOOL recoverable = YES;
                                for (NSString *path in trashed) recoverable = recoverable && Exists(path);
                                Check(recoverable, @"every trashed item exists at the reported location (recoverable)");
                                // Tidy up: these are test files; remove them from the Trash again.
                                NSUInteger purged = 0;
                                for (NSString *path in trashed) {
                                    if ([[NSFileManager defaultManager] removeItemAtPath:path error:nil]) ++purged;
                                }
                                std::printf("  [info] removed %lu of %lu fixture items from the Trash afterwards\n",
                                            static_cast<unsigned long>(purged), static_cast<unsigned long>(trashed.count));
                                for (NSString *path in trashed) {
                                    if (Exists(path)) std::printf("  [info] still in the Trash: %s\n", path.UTF8String);
                                }
                            }
                            Finish();
                        }];
                    }];
                }];
        }];
        dispatch_main();
    }
}
