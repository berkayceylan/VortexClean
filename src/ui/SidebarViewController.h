// VortexClean UI — the sidebar: every installed application, with live search
// and sorting, on a translucent sidebar material.
#import <AppKit/AppKit.h>

#import "VCModels.h"

NS_ASSUME_NONNULL_BEGIN

@class SidebarViewController;

typedef NS_ENUM(NSInteger, VCSidebarSort) {
    VCSidebarSortSize = 0,
    VCSidebarSortName = 1,
    VCSidebarSortLastUsed = 2,
};

@protocol SidebarViewControllerDelegate <NSObject>
/// The selection changed because of a user action. `application` is nil when nothing is selected.
- (void)sidebar:(SidebarViewController *)sidebar didSelectApplication:(nullable VCApp *)application;
@end

@interface SidebarViewController : NSViewController

@property (nonatomic, weak, nullable) id<SidebarViewControllerDelegate> delegate;
@property (nonatomic, readonly, nullable) VCApp *selectedApplication;
@property (nonatomic, readonly) NSUInteger applicationCount;

/// Replaces the list. The selection is kept as long as the selected bundle is still present.
- (void)setApplications:(NSArray<VCApp *> *)applications;
/// Selects (and scrolls to) an application without notifying the delegate.
- (void)selectApplicationWithBundlePath:(NSString *)bundlePath;
- (void)clearSelection;
- (void)setLoading:(BOOL)loading;
- (void)focusSearchField;

/// For snapshots: the system draws a sidebar selection with a material that only
/// exists on screen, so it would be black in a bitmap. With this set, the selected
/// row draws a plain rounded rectangle instead.
@property (nonatomic) BOOL rendersOffscreen;

@end

NS_ASSUME_NONNULL_END
