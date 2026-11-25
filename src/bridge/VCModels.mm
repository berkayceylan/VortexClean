#import "VCModels+Private.h"

#include <unistd.h>

#include "PathUtil.hpp"
#include "StringUtil.hpp"

NSString *VCStringFromStd(std::string_view text) {
    NSString *string = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    if (string != nil) return string;
    const std::string repaired = vortex::str::sanitizeUtf8(text);
    string = [[NSString alloc] initWithBytes:repaired.data() length:repaired.size() encoding:NSUTF8StringEncoding];
    return string != nil ? string : @"";
}

std::string VCStdFromString(NSString *string) {
    if (string.length == 0) return {};
    const char *representation = string.fileSystemRepresentation;
    return representation != nullptr ? std::string(representation) : std::string();
}

NSString *VCStorageBucketTitle(VCStorageBucket bucket) {
    switch (bucket) {
        case VCStorageBucketAppBundle: return @"App Bundle";
        case VCStorageBucketApplicationSupport: return @"App Support";
        case VCStorageBucketCaches: return @"Caches";
        case VCStorageBucketContainers: return @"Containers";
        case VCStorageBucketPreferences: return @"Preferences & State";
        case VCStorageBucketLogs: return @"Logs";
        case VCStorageBucketSystem: return @"System & Daemons";
        case VCStorageBucketCount: break;
    }
    return @"Other";
}

static VCStorageBucket VCBucketForCategory(vortex::RemnantCategory category) {
    using C = vortex::RemnantCategory;
    switch (category) {
        case C::AppBundle: return VCStorageBucketAppBundle;
        case C::ApplicationSupport: return VCStorageBucketApplicationSupport;
        case C::Caches: return VCStorageBucketCaches;
        case C::Containers:
        case C::GroupContainers:
        case C::ApplicationScripts: return VCStorageBucketContainers;
        case C::Preferences:
        case C::SavedState:
        case C::WebKit:
        case C::HTTPStorages:
        case C::Cookies: return VCStorageBucketPreferences;
        case C::Logs: return VCStorageBucketLogs;
        case C::LaunchAgents:
        case C::LaunchDaemons:
        case C::PrivilegedHelperTools:
        case C::Receipts:
        case C::PackagePayload:
        case C::QuickLook:
        case C::PreferencePanes:
        case C::Frameworks: return VCStorageBucketSystem;
    }
    return VCStorageBucketSystem;
}

static NSArray<NSString *> *VCStringArray(const std::vector<std::string> &values) {
    NSMutableArray<NSString *> *array = [NSMutableArray arrayWithCapacity:values.size()];
    for (const std::string &value : values) [array addObject:VCStringFromStd(value)];
    return [array copy];
}

// ---------------------------------------------------------------------------
#pragma mark - VCApp

@implementation VCApp {
    vortex::AppInfo _info;
}

- (instancetype)initWithAppInfo:(const vortex::AppInfo &)info external:(BOOL)external {
    if ((self = [super init])) {
        _info = info;
        _bundlePath = [VCStringFromStd(info.bundlePath) copy];
        _bundleIdentifier = [VCStringFromStd(info.bundleId) copy];
        _displayName = [VCStringFromStd(info.displayName) copy];
        _version = [VCStringFromStd(info.shortVersion) copy];
        _build = [VCStringFromStd(info.bundleVersion) copy];
        _executableName = [VCStringFromStd(info.executableName) copy];
        _teamIdentifier = [VCStringFromStd(info.signing.teamIdentifier) copy];
        _signingSummary = [VCStringFromStd(info.signing.summary()) copy];
        _isSigned = info.signing.isSigned;
        _isAdHocSigned = info.signing.isAdHoc;
        _isAppleApp = info.isAppleApp;
        _isSipProtected = info.isSipProtected;
        _isAppStore = info.isAppStore;
        _isExternal = external;
        _lastUsedDate = info.lastUsedEpoch > 0
                            ? [NSDate dateWithTimeIntervalSince1970:static_cast<NSTimeInterval>(info.lastUsedEpoch)]
                            : nil;
        _usageKnown = info.usageKnown;
        _allocatedBytes = info.usage.allocatedBytes;
        _logicalBytes = info.usage.logicalBytes;
    }
    return self;
}

- (const vortex::AppInfo &)appInfo {
    return _info;
}

- (NSString *)description {
    return [NSString stringWithFormat:@"<VCApp %@ (%@)>", _displayName, _bundleIdentifier];
}

@end

// ---------------------------------------------------------------------------
#pragma mark - VCRemnant

@implementation VCRemnant {
    vortex::Remnant _remnant;
}

- (instancetype)initWithRemnant:(const vortex::Remnant &)remnant
                          index:(NSUInteger)index
                          roots:(const vortex::ScanRoots &)roots {
    if ((self = [super init])) {
        _remnant = remnant;
        _index = index;
        _path = [VCStringFromStd(remnant.path) copy];
        _displayPath = [VCStringFromStd(roots.displayPath(remnant.path)) copy];
        _category = static_cast<NSInteger>(remnant.category);
        _categoryName = [VCStringFromStd(vortex::displayName(remnant.category)) copy];
        _bucket = VCBucketForCategory(remnant.category);
        _confidence = static_cast<VCConfidence>(remnant.confidence);
        _confidenceName = [VCStringFromStd(vortex::displayName(remnant.confidence)) copy];
        _reason = [VCStringFromStd(vortex::displayName(remnant.reason)) copy];
        _isApplicationBundle = remnant.category == vortex::RemnantCategory::AppBundle;
        _isDirectory = remnant.isDirectory;
        _isSystemDomain = remnant.domain == vortex::Domain::System;
        _isShared = remnant.isShared;
        _sharedWith = VCStringArray(remnant.sharedWith);
        _warning = [VCStringFromStd(remnant.warning) copy];
        _sipProtected = remnant.sipProtected;
        _requiresRoot = remnant.requiresRoot;
        _permissionDenied = remnant.permissionDenied;
        _defaultSelected = remnant.defaultSelected;
        _usageKnown = remnant.usageKnown;
        _allocatedBytes = remnant.usage.allocatedBytes;
        _logicalBytes = remnant.usage.logicalBytes;
    }
    return self;
}

- (const vortex::Remnant &)remnant {
    return _remnant;
}

@end

// ---------------------------------------------------------------------------
#pragma mark - VCScanResult

@implementation VCScanResult

- (instancetype)initWithResult:(const vortex::ScanResult &)result app:(VCApp *)app roots:(const vortex::ScanRoots &)roots {
    if ((self = [super init])) {
        _app = app;
        NSMutableArray<VCRemnant *> *remnants = [NSMutableArray arrayWithCapacity:result.remnants.size()];
        for (std::size_t i = 0; i < result.remnants.size(); ++i) {
            [remnants addObject:[[VCRemnant alloc] initWithRemnant:result.remnants[i] index:i roots:roots]];
        }
        _remnants = [remnants copy];
        _notes = VCStringArray(result.notes);
        _deniedPaths = VCStringArray(result.deniedPaths);
        _permissionDenied = result.permissionDenied;
        _cancelled = result.cancelled;
        _elapsed = result.elapsedSeconds;
    }
    return self;
}

@end

// ---------------------------------------------------------------------------
#pragma mark - VCProcess

@implementation VCProcess {
    vortex::RunningProcess _process;
}

- (instancetype)initWithProcess:(const vortex::RunningProcess &)process {
    if ((self = [super init])) {
        _process = process;
        _pid = process.pid;
        _name = [VCStringFromStd(process.name) copy];
        _executablePath = [VCStringFromStd(process.executablePath) copy];
        _isMainExecutable = process.isMainExecutable;
        _insideBundle = process.insideBundle;
        _ownedByCurrentUser = process.uid == ::getuid();
    }
    return self;
}

- (const vortex::RunningProcess &)process {
    return _process;
}

@end

// ---------------------------------------------------------------------------
#pragma mark - VCCleanItemResult / VCCleanReport

@interface VCCleanItemResult ()
- (instancetype)initWithOutcome:(const vortex::ItemOutcome &)outcome;
@end

@implementation VCCleanItemResult

- (instancetype)initWithOutcome:(const vortex::ItemOutcome &)outcome {
    if ((self = [super init])) {
        _path = [VCStringFromStd(outcome.item.path) copy];
        _status = static_cast<VCItemStatus>(outcome.status);
        _statusName = [VCStringFromStd(vortex::jsonKey(outcome.status)) copy];
        _allocatedBytes = outcome.allocatedBytes;
        _requiresRoot = outcome.requiresRoot;
        _elevated = outcome.elevated;
        _errorMessage = [VCStringFromStd(outcome.errorMessage) copy];
        _note = [VCStringFromStd(outcome.note) copy];
        _trashPath = [VCStringFromStd(outcome.trashPath) copy];
    }
    return self;
}

@end

@implementation VCCleanReport

- (instancetype)initWithReport:(const vortex::DeletionReport &)report {
    if ((self = [super init])) {
        NSMutableArray<VCCleanItemResult *> *items = [NSMutableArray arrayWithCapacity:report.outcomes.size()];
        for (const vortex::ItemOutcome &outcome : report.outcomes) {
            [items addObject:[[VCCleanItemResult alloc] initWithOutcome:outcome]];
        }
        _items = [items copy];
        _mode = report.mode == vortex::DeleteMode::Trash ? VCDeleteModeTrash : VCDeleteModePermanent;
        _dryRun = report.dryRun;
        _cancelled = report.cancelled;
        _reclaimedBytes = report.reclaimedBytes;
        _privacyDenied = report.privacyDenied;
        _elevationAttempted = report.elevationAttempted;
        _elevationAuthorized = report.elevationAuthorized;
        _elevationError = [VCStringFromStd(report.elevationError) copy];
        _auditLogPath = [VCStringFromStd(report.auditLogPath) copy];
        _auditLogError = [VCStringFromStd(report.auditLogError) copy];
    }
    return self;
}

- (NSUInteger)countWithStatus:(VCItemStatus)status {
    NSUInteger count = 0;
    for (VCCleanItemResult *item in _items) {
        if (item.status == status) ++count;
    }
    return count;
}

@end
