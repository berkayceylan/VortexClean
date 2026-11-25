// VortexClean UI — application delegate: menu bar, main window, and opening
// applications that are dropped on the Dock icon.
#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface AppDelegate : NSObject <NSApplicationDelegate>

/// Launch check for automated verification: bring up the real interface, wait
/// for discovery, scan one application, print a one-line JSON summary and quit.
@property (nonatomic) BOOL launchCheck;
/// An application to analyse right after launch (optional).
@property (nonatomic, copy, nullable) NSString *initialApplicationPath;
/// With the launch check: render the window into this PNG before quitting. The
/// application draws its own views into a bitmap — nothing on screen is captured.
@property (nonatomic, copy, nullable) NSString *snapshotPath;
/// With the launch check: leave nothing selected, to exercise the empty state.
@property (nonatomic) BOOL launchCheckEmptyState;
/// With the launch check: deliver a pointer-moved event over this segment of
/// the storage bar before the snapshot, to exercise the hover tooltip. -1 = none.
@property (nonatomic) NSInteger hoverSegment;
/// "dark" or "light" forces that appearance; anything else follows the system.
@property (nonatomic, copy, nullable) NSString *forcedAppearance;

@end

NS_ASSUME_NONNULL_END
