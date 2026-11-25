// VortexClean bridge — the asynchronous façade the AppKit layer talks to.
//
// Threading contract:
//   * every method is called on the main thread and returns immediately;
//   * all disk I/O, BOM parsing, hashing and process inspection run on
//     background queues / the engine's C++ thread pools;
//   * every block is invoked on the main queue.
// Nothing here polls: results are pushed when they exist.
#import <Foundation/Foundation.h>

#import "VCModels.h"

NS_ASSUME_NONNULL_BEGIN

@interface VCEngine : NSObject

@property (class, nonatomic, readonly, strong) VCEngine *sharedEngine;

@property (nonatomic, readonly, copy) NSString *homeDirectory;

/// Works on a synthetic disk instead of the real one (see vortex::ScanRoots::sandbox).
/// Call before the first use of sharedEngine. Returns an error message, or nil.
+ (nullable NSString *)useSandbox:(NSString *)directory;
/// ~/Library/Logs/VortexClean — where audit reports are written.
@property (nonatomic, readonly, copy) NSString *auditLogDirectory;
/// Whether root-required items can be escalated on this system.
@property (nonatomic, readonly) BOOL elevationAvailable;

/// Called whenever the list of known applications changes: after discovery,
/// as sizes arrive, when an app is dropped in, and after an uninstall.
@property (nonatomic, copy, nullable) void (^applicationsDidChange)(NSArray<VCApp *> *applications);

// ---- Discovery ------------------------------------------------------------

/// Scans /Applications and ~/Applications, reads metadata, then measures every bundle.
- (void)refreshApplicationsWithCompletion:(nullable void (^)(void))completion;

/// Analyses an arbitrary bundle (dropped from Finder, opened from the menu).
- (void)inspectApplicationAtPath:(NSString *)path
                      completion:(void (^)(VCApp *_Nullable app, NSString *_Nullable errorMessage))completion;

// ---- Remnant scan ---------------------------------------------------------

/// Starts a scan and cancels any scan still in flight. The returned identifier
/// is passed back to every block so that late results of an abandoned scan can
/// be recognised and ignored.
///   discovered — once, when all items are known (sizes still pending)
///   sized      — repeatedly, with batches of items whose size is now known
///   completion — once, with the final result
- (NSUInteger)scanApplication:(VCApp *)app
                   discovered:(void (^)(NSUInteger scanID, VCScanResult *result))discovered
                        sized:(void (^)(NSUInteger scanID, NSArray<VCRemnant *> *updated))sized
                   completion:(void (^)(NSUInteger scanID, VCScanResult *result))completion;
- (void)cancelScan;

// ---- Processes ------------------------------------------------------------

/// Finds running processes that live in the app's bundle, plus any of the given helper executables.
- (void)findProcessesForApplication:(VCApp *)app
                  helperExecutables:(NSArray<NSString *> *)helperExecutables
                         completion:(void (^)(NSArray<VCProcess *> *processes))completion;

/// Graceful (force == NO): applications are asked to quit, other processes get
/// SIGTERM, and both are given a grace period. Forced (force == YES, only ever
/// after the user confirmed): SIGTERM, then SIGKILL for whatever is left.
- (void)quitProcesses:(NSArray<VCProcess *> *)processes
                force:(BOOL)force
           completion:(void (^)(NSArray<VCProcess *> *stillRunning, NSArray<VCProcess *> *notPermitted))completion;

// ---- Cleaning -------------------------------------------------------------

- (void)cleanRemnants:(NSArray<VCRemnant *> *)remnants
        ofApplication:(VCApp *)app
                 mode:(VCDeleteMode)mode
               dryRun:(BOOL)dryRun
             progress:(void (^)(NSUInteger completed, NSUInteger total))progress
           completion:(void (^)(VCCleanReport *report))completion;
- (void)cancelClean;

/// Cancels everything in flight. Called when the application terminates.
- (void)shutdown;

@end

NS_ASSUME_NONNULL_END
