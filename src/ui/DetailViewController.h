// VortexClean UI — the detail pane: header card, storage visualiser, the
// hierarchical remnant inspector and the action bar.
#import <AppKit/AppKit.h>

#import "VCModels.h"

NS_ASSUME_NONNULL_BEGIN

@class DetailViewController;

@protocol DetailViewControllerDelegate <NSObject>
/// The user asked to pick an application with an open panel.
- (void)detailViewControllerDidRequestOpenPanel:(DetailViewController *)controller;
/// The application's bundle was removed: it should leave the sidebar.
- (void)detailViewController:(DetailViewController *)controller didUninstallApplication:(VCApp *)application;
@end

@interface DetailViewController : NSViewController

@property (nonatomic, weak, nullable) id<DetailViewControllerDelegate> delegate;
@property (nonatomic, readonly, nullable) VCApp *application;
@property (nonatomic, readonly) BOOL isBusy;
/// Invoked every time a scan finishes (used by the automated launch check).
@property (nonatomic, copy, nullable) void (^scanDidFinish)(VCScanResult *result);

/// Shows and scans an application; nil returns to the empty state.
- (void)showApplication:(nullable VCApp *)application;
/// Lets the header pick up a size that was measured after the app was selected.
- (void)applicationsDidChange:(NSArray<VCApp *> *)applications;
- (void)setDropHighlighted:(BOOL)highlighted;

- (IBAction)rescan:(nullable id)sender;
- (IBAction)revealAuditLogs:(nullable id)sender;
- (IBAction)revealSelectionInFinder:(nullable id)sender;
- (IBAction)copySelectedPaths:(nullable id)sender;
- (IBAction)quickLookSelection:(nullable id)sender;
- (IBAction)toggleSelectedRows:(nullable id)sender;
- (IBAction)cleanSelected:(nullable id)sender;

@end

NS_ASSUME_NONNULL_END
