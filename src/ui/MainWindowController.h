// VortexClean UI — the main window: a split view with the application sidebar
// and the detail pane, a unified toolbar, and window-wide drag and drop.
#import <AppKit/AppKit.h>

#import "DetailViewController.h"
#import "SidebarViewController.h"

NS_ASSUME_NONNULL_BEGIN

@interface MainWindowController : NSWindowController

@property (nonatomic, readonly, strong) SidebarViewController *sidebar;
@property (nonatomic, readonly, strong) DetailViewController *detail;
/// The applications currently listed.
@property (nonatomic, readonly, copy) NSArray<VCApp *> *applications;

/// Starts discovery of installed applications. `completion` runs when every bundle has been measured.
- (void)refreshApplicationsWithCompletion:(nullable void (^)(void))completion;
/// Analyses any application bundle, wherever it lives, and selects it.
- (void)analyzeApplicationAtPath:(NSString *)path;
/// Selects an application that is already listed.
- (void)selectApplication:(VCApp *)application;

- (IBAction)openApplication:(nullable id)sender;
- (IBAction)refreshApplications:(nullable id)sender;
- (IBAction)focusSearch:(nullable id)sender;

@end

NS_ASSUME_NONNULL_END
