// VortexClean bridge — immutable Objective-C value objects.
//
// These are the only types the AppKit layer ever sees. They are created from
// the engine's C++ value types on a background queue, never mutated afterwards,
// and are therefore safe to hand to the main thread.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, VCConfidence) {
    VCConfidenceExact = 0,
    VCConfidenceHigh = 1,
    VCConfidenceMedium = 2,
};

typedef NS_ENUM(NSInteger, VCDeleteMode) {
    VCDeleteModeTrash = 0,
    VCDeleteModePermanent = 1,
};

typedef NS_ENUM(NSInteger, VCItemStatus) {
    VCItemStatusTrashed = 0,
    VCItemStatusDeleted,
    VCItemStatusSkippedSip,
    VCItemStatusSkippedPermission,
    VCItemStatusDryRun,
    VCItemStatusError,
    VCItemStatusSkippedMissing,
};

/// The coarse groups shown by the storage visualiser.
typedef NS_ENUM(NSInteger, VCStorageBucket) {
    VCStorageBucketAppBundle = 0,
    VCStorageBucketApplicationSupport,
    VCStorageBucketCaches,
    VCStorageBucketContainers,
    VCStorageBucketPreferences,
    VCStorageBucketLogs,
    VCStorageBucketSystem,
    VCStorageBucketCount,
};

FOUNDATION_EXPORT NSString *VCStorageBucketTitle(VCStorageBucket bucket);

// ---------------------------------------------------------------------------

@interface VCApp : NSObject
@property (nonatomic, readonly, copy) NSString *bundlePath;
@property (nonatomic, readonly, copy) NSString *bundleIdentifier;
@property (nonatomic, readonly, copy) NSString *displayName;
@property (nonatomic, readonly, copy) NSString *version;
@property (nonatomic, readonly, copy) NSString *build;
@property (nonatomic, readonly, copy) NSString *executableName;
@property (nonatomic, readonly, copy) NSString *teamIdentifier;
@property (nonatomic, readonly, copy) NSString *signingSummary;
@property (nonatomic, readonly) BOOL isSigned;
@property (nonatomic, readonly) BOOL isAdHocSigned;
@property (nonatomic, readonly) BOOL isAppleApp;
@property (nonatomic, readonly) BOOL isSipProtected;
@property (nonatomic, readonly) BOOL isAppStore;
/// Analysed because the user dropped or opened it; not found in an Applications folder.
@property (nonatomic, readonly) BOOL isExternal;
@property (nonatomic, readonly, nullable) NSDate *lastUsedDate;
@property (nonatomic, readonly) BOOL usageKnown;
@property (nonatomic, readonly) uint64_t allocatedBytes;
@property (nonatomic, readonly) uint64_t logicalBytes;

- (instancetype)init NS_UNAVAILABLE;
@end

// ---------------------------------------------------------------------------

@interface VCRemnant : NSObject
/// Position in the scan result; stable for the lifetime of one scan.
@property (nonatomic, readonly) NSUInteger index;
@property (nonatomic, readonly, copy) NSString *path;
/// The path with the home directory abbreviated to "~".
@property (nonatomic, readonly, copy) NSString *displayPath;
@property (nonatomic, readonly) NSInteger category;
@property (nonatomic, readonly, copy) NSString *categoryName;
@property (nonatomic, readonly) VCStorageBucket bucket;
@property (nonatomic, readonly) VCConfidence confidence;
@property (nonatomic, readonly, copy) NSString *confidenceName;
@property (nonatomic, readonly, copy) NSString *reason;
@property (nonatomic, readonly) BOOL isApplicationBundle;
@property (nonatomic, readonly) BOOL isDirectory;
@property (nonatomic, readonly) BOOL isSystemDomain;
@property (nonatomic, readonly) BOOL isShared;
@property (nonatomic, readonly, copy) NSArray<NSString *> *sharedWith;
@property (nonatomic, readonly, copy) NSString *warning;
@property (nonatomic, readonly) BOOL sipProtected;
@property (nonatomic, readonly) BOOL requiresRoot;
@property (nonatomic, readonly) BOOL permissionDenied;
@property (nonatomic, readonly) BOOL defaultSelected;
@property (nonatomic, readonly) BOOL usageKnown;
@property (nonatomic, readonly) uint64_t allocatedBytes;
@property (nonatomic, readonly) uint64_t logicalBytes;

- (instancetype)init NS_UNAVAILABLE;
@end

// ---------------------------------------------------------------------------

@interface VCScanResult : NSObject
@property (nonatomic, readonly, strong) VCApp *app;
@property (nonatomic, readonly, copy) NSArray<VCRemnant *> *remnants;
@property (nonatomic, readonly, copy) NSArray<NSString *> *notes;
@property (nonatomic, readonly, copy) NSArray<NSString *> *deniedPaths;
@property (nonatomic, readonly) BOOL permissionDenied;
@property (nonatomic, readonly) BOOL cancelled;
@property (nonatomic, readonly) NSTimeInterval elapsed;

- (instancetype)init NS_UNAVAILABLE;
@end

// ---------------------------------------------------------------------------

@interface VCProcess : NSObject
@property (nonatomic, readonly) pid_t pid;
@property (nonatomic, readonly, copy) NSString *name;
@property (nonatomic, readonly, copy) NSString *executablePath;
@property (nonatomic, readonly) BOOL isMainExecutable;
@property (nonatomic, readonly) BOOL insideBundle;
@property (nonatomic, readonly) BOOL ownedByCurrentUser;

- (instancetype)init NS_UNAVAILABLE;
@end

// ---------------------------------------------------------------------------

@interface VCCleanItemResult : NSObject
@property (nonatomic, readonly, copy) NSString *path;
@property (nonatomic, readonly) VCItemStatus status;
@property (nonatomic, readonly, copy) NSString *statusName;
@property (nonatomic, readonly) uint64_t allocatedBytes;
@property (nonatomic, readonly) BOOL requiresRoot;
@property (nonatomic, readonly) BOOL elevated;
@property (nonatomic, readonly, copy) NSString *errorMessage;
@property (nonatomic, readonly, copy) NSString *note;
@property (nonatomic, readonly, copy) NSString *trashPath;

- (instancetype)init NS_UNAVAILABLE;
@end

@interface VCCleanReport : NSObject
@property (nonatomic, readonly, copy) NSArray<VCCleanItemResult *> *items;
@property (nonatomic, readonly) VCDeleteMode mode;
@property (nonatomic, readonly) BOOL dryRun;
@property (nonatomic, readonly) BOOL cancelled;
@property (nonatomic, readonly) uint64_t reclaimedBytes;
@property (nonatomic, readonly) BOOL privacyDenied;
@property (nonatomic, readonly) BOOL elevationAttempted;
@property (nonatomic, readonly) BOOL elevationAuthorized;
@property (nonatomic, readonly, copy) NSString *elevationError;
@property (nonatomic, readonly, copy) NSString *auditLogPath;
@property (nonatomic, readonly, copy) NSString *auditLogError;

- (NSUInteger)countWithStatus:(VCItemStatus)status;
- (instancetype)init NS_UNAVAILABLE;
@end

NS_ASSUME_NONNULL_END
