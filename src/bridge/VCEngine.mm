#import "VCEngine.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#import "VCModels+Private.h"
#import "VCSystemHooks.h"

#include "BundleInspector.hpp"
#include "Cancellation.hpp"
#include "DeletionEngine.hpp"
#include "DiskAnalyzer.hpp"
#include "MatchEngine.hpp"
#include "PathUtil.hpp"
#include "ProcessInspector.hpp"
#include "RemnantScanner.hpp"
#include "ThreadPool.hpp"

namespace {

constexpr auto kPublishInterval = std::chrono::milliseconds(120);
constexpr auto kQuitGracePeriod = std::chrono::seconds(5);
constexpr auto kTerminateGracePeriod = std::chrono::seconds(2);

/// Set once, before the shared engine exists (see +useSandbox:).
std::optional<vortex::ScanRoots> gSandbox;

/// Sized remnants waiting to be delivered to the main queue in one batch.
struct SizedBatch {
    std::mutex mutex;
    NSMutableArray<VCRemnant *> *items = [NSMutableArray array];
    bool flushScheduled = false;
};

}  // namespace

@implementation VCEngine {
    std::unique_ptr<vortex::ThreadPool> _scanPool;       // user-initiated: the scan the user is looking at
    std::unique_ptr<vortex::ThreadPool> _discoveryPool;  // utility: sizing the whole application list
    std::unique_ptr<vortex::RemnantScanner> _scanner;
    vortex::ScanRoots _roots;

    dispatch_queue_t _workQueue;       // concurrent; orchestrates scans, cleans, process work
    dispatch_queue_t _discoveryQueue;  // serial; one refresh at a time

    std::mutex _stateMutex;  // guards everything below
    std::vector<vortex::AppInfo> _installed;  // found in the Applications folders
    std::vector<vortex::AppInfo> _external;   // dropped in / opened by the user
    std::vector<vortex::AppInfo> _system;     // /System/Applications: used for cross-checks only
    vortex::CancellationToken _scanToken;
    vortex::CancellationToken _cleanToken;
    vortex::CancellationToken _discoveryToken;
    std::chrono::steady_clock::time_point _lastPublish;

    NSUInteger _scanGeneration;  // main thread only
}

+ (VCEngine *)sharedEngine {
    static VCEngine *engine;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        engine = [[VCEngine alloc] init];
    });
    return engine;
}

- (instancetype)init {
    if ((self = [super init])) {
        _roots = gSandbox.has_value() ? *gSandbox : vortex::ScanRoots::systemDefault();
        _scanPool = std::make_unique<vortex::ThreadPool>(0, vortex::ThreadPool::Priority::UserInitiated, "vortex.scan");
        _discoveryPool =
            std::make_unique<vortex::ThreadPool>(0, vortex::ThreadPool::Priority::Utility, "vortex.discovery");
        _scanner = std::make_unique<vortex::RemnantScanner>(_roots, *_scanPool);
        _workQueue = dispatch_queue_create(
            "com.berkayceylan.VortexClean.work",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0));
        _discoveryQueue = dispatch_queue_create(
            "com.berkayceylan.VortexClean.discovery",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
        _homeDirectory = [VCStringFromStd(_roots.homeDir) copy];
        _auditLogDirectory = [VCStringFromStd(vortex::DeletionEngine::defaultAuditDirectory(_roots.homeDir)) copy];
    }
    return self;
}

+ (NSString *)useSandbox:(NSString *)directory {
    auto sandbox = vortex::ScanRoots::sandbox(VCStdFromString(directory));
    if (!sandbox) return VCStringFromStd(sandbox.error().message);
    gSandbox = std::move(sandbox.value());
    return nil;
}

- (BOOL)elevationAvailable {
    return !_roots.isSandbox() && vcbridge::isElevationAvailable();
}

// ---------------------------------------------------------------------------
#pragma mark - Publishing the application list

/// Snapshot of every application the UI should list. Safe on any thread.
- (NSArray<VCApp *> *)applicationSnapshot {
    std::vector<vortex::AppInfo> installed;
    std::vector<vortex::AppInfo> external;
    {
        const std::lock_guard<std::mutex> lock(_stateMutex);
        installed = _installed;
        external = _external;
    }
    NSMutableArray<VCApp *> *apps = [NSMutableArray arrayWithCapacity:installed.size() + external.size()];
    for (const vortex::AppInfo &info : installed) [apps addObject:[[VCApp alloc] initWithAppInfo:info external:NO]];
    for (const vortex::AppInfo &info : external) [apps addObject:[[VCApp alloc] initWithAppInfo:info external:YES]];
    return [apps copy];
}

/// Delivers the current list to the UI. With `force == NO` updates are rate
/// limited so that sizing hundreds of bundles does not flood the main queue.
- (void)publishApplications:(BOOL)force {
    {
        const std::lock_guard<std::mutex> lock(_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - _lastPublish < kPublishInterval) return;
        _lastPublish = now;
    }
    NSArray<VCApp *> *snapshot = [self applicationSnapshot];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.applicationsDidChange != nil) self.applicationsDidChange(snapshot);
    });
}

/// Records a freshly measured bundle size. Returns NO when the app is no longer listed.
- (BOOL)storeUsage:(const vortex::DiskUsage &)usage forBundle:(const std::string &)bundlePath {
    const std::lock_guard<std::mutex> lock(_stateMutex);
    for (std::vector<vortex::AppInfo> *list : {&_installed, &_external}) {
        for (vortex::AppInfo &info : *list) {
            if (info.bundlePath == bundlePath) {
                info.usage = usage;
                info.usageKnown = true;
                return YES;
            }
        }
    }
    return NO;
}

// ---------------------------------------------------------------------------
#pragma mark - Discovery

- (void)refreshApplicationsWithCompletion:(void (^)(void))completion {
    const vortex::CancellationToken token;
    {
        const std::lock_guard<std::mutex> lock(_stateMutex);
        _discoveryToken.cancel();
        _discoveryToken = token;
    }
    dispatch_async(_discoveryQueue, ^{
        const std::string home = self->_roots.homeDir;
        const std::string systemRoot = self->_roots.installRoot;
        vortex::ThreadPool &pool = *self->_discoveryPool;

        // ---- Find and inspect every bundle (metadata only, in parallel).
        const std::vector<std::string> paths =
            vortex::BundleInspector::discoverBundles(vortex::BundleInspector::defaultSearchRoots(home, systemRoot), token);
        std::vector<vortex::AppInfo> inspected(paths.size());
        std::vector<char> succeeded(paths.size(), 0);
        {
            vortex::TaskGroup group(pool);
            for (std::size_t i = 0; i < paths.size(); ++i) {
                group.run([&paths, &inspected, &succeeded, &token, i] {
                    if (token.isCancelled()) return;
                    auto result = vortex::BundleInspector::inspect(paths[i]);
                    if (result) {
                        inspected[i] = std::move(result.value());
                        succeeded[i] = 1;
                    }
                });
            }
            group.wait();
        }
        std::vector<vortex::AppInfo> apps;
        apps.reserve(inspected.size());
        for (std::size_t i = 0; i < inspected.size(); ++i) {
            if (succeeded[i] != 0) apps.push_back(std::move(inspected[i]));
        }

        // ---- System applications and installed helper apps: never listed, but known
        // ---- to the cross-checks so that their data is not attributed to someone else.
        std::vector<vortex::AppInfo> systemApps =
            vortex::BundleInspector::inspectAuxiliaryApplications(token, systemRoot);
        if (token.isCancelled()) {
            if (completion != nil) dispatch_async(dispatch_get_main_queue(), completion);
            return;
        }

        // ---- Keep sizes that are still valid, so the list does not flicker on refresh.
        std::vector<std::string> toMeasure;
        {
            const std::lock_guard<std::mutex> lock(self->_stateMutex);
            std::unordered_map<std::string, const vortex::AppInfo *> previous;
            for (const vortex::AppInfo &info : self->_installed) previous.emplace(info.bundlePath, &info);
            for (vortex::AppInfo &info : apps) {
                const auto found = previous.find(info.bundlePath);
                if (found != previous.end() && found->second->usageKnown &&
                    found->second->modifiedEpoch == info.modifiedEpoch) {
                    info.usage = found->second->usage;
                    info.usageKnown = true;
                } else {
                    toMeasure.push_back(info.bundlePath);
                }
            }
            self->_installed = std::move(apps);
            self->_system = std::move(systemApps);
            // An app the user dropped in earlier may now be found by discovery.
            std::erase_if(self->_external, [self](const vortex::AppInfo &external) {
                return std::any_of(self->_installed.begin(), self->_installed.end(),
                                   [&external](const vortex::AppInfo &installed) {
                                       return installed.bundlePath == external.bundlePath;
                                   });
            });
        }
        [self publishApplications:YES];

        // ---- Measure the bundles; results are published as they arrive.
        {
            vortex::TaskGroup group(pool);
            for (const std::string &bundlePath : toMeasure) {
                group.run([self, &token, bundlePath] {
                    if (token.isCancelled()) return;
                    const vortex::DiskUsage usage = vortex::DiskAnalyzer::measure(bundlePath, token);
                    if (usage.cancelled) return;
                    if ([self storeUsage:usage forBundle:bundlePath]) [self publishApplications:NO];
                });
            }
            group.wait();
        }
        [self publishApplications:YES];
        if (completion != nil) dispatch_async(dispatch_get_main_queue(), completion);
    });
}

- (void)inspectApplicationAtPath:(NSString *)path
                      completion:(void (^)(VCApp *_Nullable, NSString *_Nullable))completion {
    const std::string bundlePath = VCStdFromString(path);
    dispatch_async(_workQueue, ^{
        auto result = vortex::BundleInspector::inspect(bundlePath);
        if (!result) {
            NSString *message = VCStringFromStd(result.error().message);
            dispatch_async(dispatch_get_main_queue(), ^{
                completion(nil, message);
            });
            return;
        }
        vortex::AppInfo info = std::move(result.value());
        BOOL external = YES;
        {
            const std::lock_guard<std::mutex> lock(self->_stateMutex);
            bool known = false;
            for (const vortex::AppInfo &installed : self->_installed) {
                if (installed.bundlePath == info.bundlePath) {
                    info = installed;  // keep the size that was already measured
                    known = true;
                    external = NO;
                    break;
                }
            }
            if (!known) {
                bool listed = false;
                for (vortex::AppInfo &existing : self->_external) {
                    if (existing.bundlePath == info.bundlePath) {
                        info.usage = existing.usage;
                        info.usageKnown = existing.usageKnown;
                        existing = info;
                        listed = true;
                        break;
                    }
                }
                if (!listed) self->_external.push_back(info);
            }
        }
        VCApp *app = [[VCApp alloc] initWithAppInfo:info external:external];
        [self publishApplications:YES];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(app, nil);
        });
    });
}

// ---------------------------------------------------------------------------
#pragma mark - Remnant scan

- (NSUInteger)scanApplication:(VCApp *)app
                   discovered:(void (^)(NSUInteger, VCScanResult *))discovered
                        sized:(void (^)(NSUInteger, NSArray<VCRemnant *> *))sized
                   completion:(void (^)(NSUInteger, VCScanResult *))completion {
    const NSUInteger scanID = ++_scanGeneration;
    const vortex::CancellationToken token;
    {
        const std::lock_guard<std::mutex> lock(_stateMutex);
        _scanToken.cancel();  // the previous scan stops within milliseconds
        _scanToken = token;
    }

    dispatch_async(_workQueue, ^{
        vortex::AppInfo target = app.appInfo;
        std::vector<vortex::AppInfo> everything;
        {
            const std::lock_guard<std::mutex> lock(self->_stateMutex);
            everything.reserve(self->_installed.size() + self->_external.size() + self->_system.size());
            for (const std::vector<vortex::AppInfo> *list : {&self->_installed, &self->_external, &self->_system}) {
                for (const vortex::AppInfo &info : *list) {
                    // Prefer the freshest size we have for the bundle being scanned.
                    if (info.bundlePath == target.bundlePath && info.usageKnown) {
                        target.usage = info.usage;
                        target.usageKnown = true;
                    }
                    everything.push_back(info);
                }
            }
        }
        const vortex::InstalledAppIndex index(everything);
        const vortex::ScanRoots &roots = self->_roots;
        const auto batch = std::make_shared<SizedBatch>();

        vortex::ScanCallbacks callbacks;
        // The blocks below run on the main queue after this function has returned,
        // so they must own what they use: a block copies a C++ reference as a
        // reference, which would dangle. Hence the explicit local copies.
        callbacks.onDiscovered = [&target, &roots, app, discovered, scanID](const std::vector<vortex::Remnant> &remnants) {
            vortex::ScanResult partial;
            partial.app = target;
            partial.remnants = remnants;
            VCScanResult *result = [[VCScanResult alloc] initWithResult:partial app:app roots:roots];
            void (^deliver)(NSUInteger, VCScanResult *) = discovered;
            const NSUInteger identifier = scanID;
            dispatch_async(dispatch_get_main_queue(), ^{
                deliver(identifier, result);
            });
        };
        callbacks.onSized = [&roots, batch, sized, scanID](std::size_t position, const vortex::Remnant &remnant) {
            void (^deliver)(NSUInteger, NSArray<VCRemnant *> *) = sized;
            const NSUInteger identifier = scanID;
            const std::shared_ptr<SizedBatch> pending = batch;
            VCRemnant *updated = [[VCRemnant alloc] initWithRemnant:remnant index:position roots:roots];
            bool schedule = false;
            {
                const std::lock_guard<std::mutex> lock(pending->mutex);
                [pending->items addObject:updated];
                schedule = !pending->flushScheduled;
                pending->flushScheduled = true;
            }
            if (!schedule) return;
            // One main-queue hop delivers everything that accumulates until it runs.
            dispatch_async(dispatch_get_main_queue(), ^{
                NSArray<VCRemnant *> *ready = nil;
                {
                    const std::lock_guard<std::mutex> lock(pending->mutex);
                    ready = [pending->items copy];
                    [pending->items removeAllObjects];
                    pending->flushScheduled = false;
                }
                if (ready.count > 0) deliver(identifier, ready);
            });
        };

        const vortex::ScanResult result =
            self->_scanner->scan(target, index, token, vortex::ScanOptions{}, callbacks);

        // Remember the bundle size the scan measured, so the sidebar shows it too.
        if (!result.cancelled && !result.remnants.empty() && result.remnants.front().usageKnown && !target.usageKnown) {
            if ([self storeUsage:result.remnants.front().usage forBundle:target.bundlePath]) {
                [self publishApplications:YES];
            }
        }
        VCScanResult *final = [[VCScanResult alloc] initWithResult:result app:app roots:roots];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(scanID, final);
        });
    });
    return scanID;
}

- (void)cancelScan {
    const std::lock_guard<std::mutex> lock(_stateMutex);
    _scanToken.cancel();
}

// ---------------------------------------------------------------------------
#pragma mark - Processes

- (void)findProcessesForApplication:(VCApp *)app
                  helperExecutables:(NSArray<NSString *> *)helperExecutables
                         completion:(void (^)(NSArray<VCProcess *> *))completion {
    std::vector<std::string> extras;
    extras.reserve(helperExecutables.count);
    for (NSString *path in helperExecutables) extras.push_back(VCStdFromString(path));

    dispatch_async(_workQueue, ^{
        const std::vector<vortex::RunningProcess> related = vortex::ProcessInspector::findRelated(app.appInfo, extras);
        NSMutableArray<VCProcess *> *processes = [NSMutableArray arrayWithCapacity:related.size()];
        for (const vortex::RunningProcess &process : related) {
            [processes addObject:[[VCProcess alloc] initWithProcess:process]];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            completion([processes copy]);
        });
    });
}

- (void)quitProcesses:(NSArray<VCProcess *> *)processes
                force:(BOOL)force
           completion:(void (^)(NSArray<VCProcess *> *, NSArray<VCProcess *> *))completion {
    dispatch_async(_workQueue, ^{
        std::vector<vortex::RunningProcess> all;
        all.reserve(processes.count);
        for (VCProcess *process in processes) all.push_back(process.process);

        std::vector<pid_t> stillRunning;
        std::vector<pid_t> notPermitted;
        const auto absorb = [&](const vortex::ProcessInspector::SignalOutcome &outcome) {
            stillRunning.insert(stillRunning.end(), outcome.stillRunning.begin(), outcome.stillRunning.end());
            notPermitted.insert(notPermitted.end(), outcome.notPermitted.begin(), outcome.notPermitted.end());
        };

        if (!force) {
            // Applications are asked to quit the Cocoa way first: unlike a bare
            // SIGTERM this lets them prompt for unsaved documents. Everything
            // else (helpers, agents, daemons) gets SIGTERM.
            std::vector<vortex::RunningProcess> asked;
            std::vector<vortex::RunningProcess> signalled;
            for (const vortex::RunningProcess &process : all) {
                NSRunningApplication *running =
                    [NSRunningApplication runningApplicationWithProcessIdentifier:process.pid];
                const bool isApplication =
                    running != nil && running.activationPolicy != NSApplicationActivationPolicyProhibited;
                if (isApplication && [running terminate]) {
                    asked.push_back(process);
                } else {
                    signalled.push_back(process);
                }
            }
            absorb(vortex::ProcessInspector::signalAndWait(signalled, SIGTERM, kQuitGracePeriod));
            absorb(vortex::ProcessInspector::signalAndWait(asked, 0, kQuitGracePeriod));
        } else {
            // The confirmed escalation: SIGTERM, a short grace period, then SIGKILL.
            const auto terminated = vortex::ProcessInspector::signalAndWait(all, SIGTERM, kTerminateGracePeriod);
            notPermitted = terminated.notPermitted;
            std::vector<vortex::RunningProcess> survivors;
            for (const vortex::RunningProcess &process : all) {
                if (std::find(terminated.stillRunning.begin(), terminated.stillRunning.end(), process.pid) !=
                    terminated.stillRunning.end()) {
                    survivors.push_back(process);
                }
            }
            const auto killed = vortex::ProcessInspector::signalAndWait(survivors, SIGKILL, kQuitGracePeriod);
            stillRunning = killed.stillRunning;
            notPermitted.insert(notPermitted.end(), killed.notPermitted.begin(), killed.notPermitted.end());
        }

        const auto select = [processes](const std::vector<pid_t> &pids) {
            NSMutableArray<VCProcess *> *selected = [NSMutableArray array];
            for (VCProcess *process in processes) {
                if (std::find(pids.begin(), pids.end(), process.pid) != pids.end()) [selected addObject:process];
            }
            return [selected copy];
        };
        NSArray<VCProcess *> *running = select(stillRunning);
        NSArray<VCProcess *> *denied = select(notPermitted);
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(running, denied);
        });
    });
}

// ---------------------------------------------------------------------------
#pragma mark - Cleaning

- (void)cleanRemnants:(NSArray<VCRemnant *> *)remnants
        ofApplication:(VCApp *)app
                 mode:(VCDeleteMode)mode
               dryRun:(BOOL)dryRun
             progress:(void (^)(NSUInteger, NSUInteger))progress
           completion:(void (^)(VCCleanReport *))completion {
    const vortex::CancellationToken token;
    {
        const std::lock_guard<std::mutex> lock(_stateMutex);
        _cleanToken = token;
    }

    dispatch_async(_workQueue, ^{
        vortex::DeletionRequest request;
        request.target = app.appInfo;
        request.mode = mode == VCDeleteModePermanent ? vortex::DeleteMode::Permanent : vortex::DeleteMode::Trash;
        request.dryRun = dryRun;
        request.items.reserve(remnants.count);
        for (VCRemnant *remnant in remnants) {
            request.items.push_back(vortex::DeletionItem::fromRemnant(remnant.remnant));
        }

        vortex::DeletionHooks hooks;
        if (self->_roots.isSandbox()) {
            hooks = vcbridge::sandboxHooks(self->_roots.homeDir);
        } else {
            hooks.moveToTrash = &vcbridge::moveToTrash;
            hooks.elevate = &vcbridge::runPrivilegedBatch;
        }
        auto lastProgress = std::make_shared<std::chrono::steady_clock::time_point>();
        hooks.onProgress = [progress, lastProgress](std::size_t completed, std::size_t total,
                                                    const vortex::ItemOutcome &) {
            // Rate limited, but the final step is always delivered.
            const auto now = std::chrono::steady_clock::now();
            if (completed != total && now - *lastProgress < std::chrono::milliseconds(50)) return;
            *lastProgress = now;
            dispatch_async(dispatch_get_main_queue(), ^{
                progress(completed, total);
            });
        };

        vortex::DeletionEngine::Config config;
        config.homeDirectory = self->_roots.homeDir;
        config.systemRoot = self->_roots.installRoot;
        const vortex::DeletionEngine engine(config, hooks);
        const vortex::DeletionReport report = engine.execute(request, token);

        // An application whose bundle is gone leaves the list.
        bool listChanged = false;
        if (!report.dryRun) {
            for (const vortex::ItemOutcome &outcome : report.outcomes) {
                const bool removed = outcome.status == vortex::ItemStatus::Trashed ||
                                     outcome.status == vortex::ItemStatus::Deleted;
                if (!removed || outcome.item.category != vortex::RemnantCategory::AppBundle) continue;
                const std::lock_guard<std::mutex> lock(self->_stateMutex);
                const auto matches = [&outcome](const vortex::AppInfo &info) {
                    return info.bundlePath == outcome.item.path;
                };
                listChanged = std::erase_if(self->_installed, matches) > 0 || listChanged;
                listChanged = std::erase_if(self->_external, matches) > 0 || listChanged;
            }
        }
        if (listChanged) [self publishApplications:YES];

        VCCleanReport *result = [[VCCleanReport alloc] initWithReport:report];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(result);
        });
    });
}

- (void)cancelClean {
    const std::lock_guard<std::mutex> lock(_stateMutex);
    _cleanToken.cancel();
}

- (void)shutdown {
    const std::lock_guard<std::mutex> lock(_stateMutex);
    _scanToken.cancel();
    _cleanToken.cancel();
    _discoveryToken.cancel();
}

@end
