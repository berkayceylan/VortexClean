// VortexClean UI — shared formatting, colours, symbols and small view factories.
#import <AppKit/AppKit.h>

#import "VCModels.h"

NS_ASSUME_NONNULL_BEGIN

/// "1.24 GB" — the same style Finder uses.
NSString *VCFormatBytes(uint64_t bytes);
/// "3 items" / "1 item".
NSString *VCFormatCount(NSUInteger count, NSString *singular, NSString *plural);

/// Semantic colours: they follow Light/Dark mode and accessibility settings.
NSColor *VCColorForBucket(VCStorageBucket bucket);
NSColor *VCColorForConfidence(VCConfidence confidence);

/// An SF Symbol, or nil when the symbol does not exist on this system.
NSImage *_Nullable VCSymbol(NSString *name, CGFloat pointSize, NSFontWeight weight);

/// A static, non-editable text label configured for Auto Layout.
NSTextField *VCMakeLabel(NSString *text, NSFont *font, NSColor *color);

/// Loads application and file icons off the main thread and caches them.
@interface VCIconCache : NSObject
@property (class, nonatomic, readonly, strong) VCIconCache *sharedCache;
/// The icon if it is already in memory; never touches the disk.
- (nullable NSImage *)cachedIconForPath:(NSString *)path;
/// Loads the icon on a background queue and calls back on the main queue.
- (void)loadIconForPath:(NSString *)path completion:(void (^)(NSImage *icon))completion;
- (void)removeIconForPath:(NSString *)path;
@end

/// A rounded "pill" badge with a tinted background.
@interface VCPillView : NSView
@property (nonatomic, copy) NSString *text;
@property (nonatomic, strong) NSColor *tintColor;
@property (nonatomic, strong, nullable) NSImage *image;
@end

NS_ASSUME_NONNULL_END
