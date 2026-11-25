#import "MainWindowController.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "VCEngine.h"
#import "VCFormat.h"

namespace {

NSToolbarItemIdentifier const kOpenItem = @"com.berkayceylan.VortexClean.toolbar.open";
NSToolbarItemIdentifier const kRefreshItem = @"com.berkayceylan.VortexClean.toolbar.refresh";
NSToolbarItemIdentifier const kAuditItem = @"com.berkayceylan.VortexClean.toolbar.audit";

constexpr CGFloat kMinimumWindowWidth = 940.0;
constexpr CGFloat kMinimumWindowHeight = 580.0;

}  // namespace

@interface MainWindowController () <NSWindowDelegate,
                                    NSToolbarDelegate,
                                    NSDraggingDestination,
                                    SidebarViewControllerDelegate,
                                    DetailViewControllerDelegate>
@end

@implementation MainWindowController {
    NSSplitViewController *_splitViewController;
    BOOL _refreshing;
}

- (instancetype)init {
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 740)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView
                    backing:NSBackingStoreBuffered
                      defer:YES];
    if ((self = [super initWithWindow:window])) {
        _applications = @[];
        window.title = @"VortexClean";
        window.subtitle = @"Looking for applications…";
        window.toolbarStyle = NSWindowToolbarStyleUnified;
        window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleAutomatic;
        window.tabbingMode = NSWindowTabbingModeDisallowed;
        window.releasedWhenClosed = NO;
        window.delegate = self;

        _sidebar = [[SidebarViewController alloc] init];
        _sidebar.delegate = self;
        _detail = [[DetailViewController alloc] init];
        _detail.delegate = self;

        _splitViewController = [[NSSplitViewController alloc] init];
        NSSplitViewItem *sidebarItem = [NSSplitViewItem sidebarWithViewController:_sidebar];
        sidebarItem.minimumThickness = 250.0;
        sidebarItem.maximumThickness = 420.0;
        sidebarItem.canCollapse = YES;
        NSSplitViewItem *detailItem = [NSSplitViewItem splitViewItemWithViewController:_detail];
        detailItem.minimumThickness = 640.0;
        [_splitViewController addSplitViewItem:sidebarItem];
        [_splitViewController addSplitViewItem:detailItem];
        window.contentViewController = _splitViewController;

        NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"com.berkayceylan.VortexClean.toolbar"];
        toolbar.delegate = self;
        toolbar.displayMode = NSToolbarDisplayModeIconOnly;
        toolbar.allowsUserCustomization = NO;
        window.toolbar = toolbar;

        window.contentMinSize = NSMakeSize(kMinimumWindowWidth, kMinimumWindowHeight);
        [window setContentSize:NSMakeSize(1180, 740)];
        [window center];
        window.frameAutosaveName = @"VortexCleanMainWindow";

        // Dropping an application anywhere on the window analyses it.
        [window registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

        __weak MainWindowController *weakSelf = self;
        VCEngine.sharedEngine.applicationsDidChange = ^(NSArray<VCApp *> *applications) {
            [weakSelf applicationsDidChange:applications];
        };
    }
    return self;
}

// ---------------------------------------------------------------------------
#pragma mark - Application list

- (void)applicationsDidChange:(NSArray<VCApp *> *)applications {
    _applications = [applications copy];
    [_sidebar setApplications:applications];
    [_detail applicationsDidChange:applications];
    [self updateSubtitle];

    // The application being shown may have disappeared (uninstalled elsewhere, then refreshed).
    VCApp *shown = _detail.application;
    if (shown != nil && !_detail.isBusy) {
        BOOL stillListed = NO;
        for (VCApp *candidate in applications) {
            if ([candidate.bundlePath isEqualToString:shown.bundlePath]) stillListed = YES;
        }
        if (!stillListed) {
            [_sidebar clearSelection];
            [_detail showApplication:nil];
        }
    }
}

- (void)updateSubtitle {
    if (_applications.count == 0) {
        self.window.subtitle = _refreshing ? @"Looking for applications…" : @"No applications found";
        return;
    }
    uint64_t total = 0;
    for (VCApp *application in _applications) total += application.allocatedBytes;
    self.window.subtitle = [NSString
        stringWithFormat:@"%@ · %@", VCFormatCount(_applications.count, @"application", @"applications"),
                         VCFormatBytes(total)];
}

- (void)refreshApplicationsWithCompletion:(void (^)(void))completion {
    _refreshing = YES;
    [_sidebar setLoading:YES];
    [self updateSubtitle];
    __weak MainWindowController *weakSelf = self;
    [VCEngine.sharedEngine refreshApplicationsWithCompletion:^{
        MainWindowController *controller = weakSelf;
        if (controller != nil) {
            controller->_refreshing = NO;
            [controller->_sidebar setLoading:NO];
            [controller updateSubtitle];
        }
        if (completion != nil) completion();
    }];
}

- (IBAction)refreshApplications:(id)sender {
    (void)sender;
    [self refreshApplicationsWithCompletion:nil];
    [_detail rescan:nil];
}

- (void)selectApplication:(VCApp *)application {
    [_sidebar selectApplicationWithBundlePath:application.bundlePath];
    [_detail showApplication:application];
}

- (void)analyzeApplicationAtPath:(NSString *)path {
    __weak MainWindowController *weakSelf = self;
    [VCEngine.sharedEngine
        inspectApplicationAtPath:path
                      completion:^(VCApp *application, NSString *errorMessage) {
                          MainWindowController *controller = weakSelf;
                          if (controller == nil) return;
                          if (application == nil) {
                              NSAlert *alert = [[NSAlert alloc] init];
                              alert.alertStyle = NSAlertStyleWarning;
                              alert.messageText = [NSString
                                  stringWithFormat:@"“%@” cannot be analyzed", path.lastPathComponent];
                              alert.informativeText =
                                  errorMessage != nil ? errorMessage : @"It does not look like an application bundle.";
                              [alert beginSheetModalForWindow:controller.window completionHandler:nil];
                              return;
                          }
                          // The engine has already published the list that contains this application.
                          [controller selectApplication:application];
                          [controller.window makeKeyAndOrderFront:nil];
                      }];
}

- (IBAction)openApplication:(id)sender {
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Choose an Application";
    panel.prompt = @"Analyze";
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.treatsFilePackagesAsDirectories = NO;
    panel.allowedContentTypes = @[UTTypeApplicationBundle];
    panel.directoryURL = [NSURL fileURLWithPath:@"/Applications" isDirectory:YES];
    __weak MainWindowController *weakSelf = self;
    [panel beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse response) {
                      NSString *path = panel.URL.path;
                      if (response == NSModalResponseOK && path.length > 0) [weakSelf analyzeApplicationAtPath:path];
                  }];
}

- (IBAction)focusSearch:(id)sender {
    (void)sender;
    for (NSSplitViewItem *item in _splitViewController.splitViewItems) {
        if (item.viewController == _sidebar && item.isCollapsed) item.animator.collapsed = NO;
    }
    [_sidebar focusSearchField];
}

// ---------------------------------------------------------------------------
#pragma mark - Sidebar / detail delegates

- (void)sidebar:(SidebarViewController *)sidebar didSelectApplication:(VCApp *)application {
    (void)sidebar;
    [_detail showApplication:application];
}

- (void)detailViewControllerDidRequestOpenPanel:(DetailViewController *)controller {
    (void)controller;
    [self openApplication:nil];
}

- (void)detailViewController:(DetailViewController *)controller didUninstallApplication:(VCApp *)application {
    (void)application;
    [_sidebar clearSelection];
    [controller showApplication:nil];
}

// ---------------------------------------------------------------------------
#pragma mark - Drag and drop (the window forwards these to its delegate)

/// The first application bundle among the dragged files, or nil.
- (nullable NSString *)applicationPathFromDrag:(id<NSDraggingInfo>)info {
    NSArray<NSURL *> *urls =
        [info.draggingPasteboard readObjectsForClasses:@[[NSURL class]]
                                               options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    for (NSURL *url in urls) {
        if ([url.pathExtension caseInsensitiveCompare:@"app"] == NSOrderedSame) return url.path;
    }
    return nil;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    if ([self applicationPathFromDrag:sender] == nil || _detail.isBusy) return NSDragOperationNone;
    [_detail setDropHighlighted:YES];
    return NSDragOperationCopy;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    return ([self applicationPathFromDrag:sender] != nil && !_detail.isBusy) ? NSDragOperationCopy
                                                                              : NSDragOperationNone;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    [_detail setDropHighlighted:NO];
}

- (void)draggingEnded:(id<NSDraggingInfo>)sender {
    (void)sender;
    [_detail setDropHighlighted:NO];
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    return [self applicationPathFromDrag:sender] != nil;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSString *path = [self applicationPathFromDrag:sender];
    [_detail setDropHighlighted:NO];
    if (path == nil) return NO;
    [self analyzeApplicationAtPath:path];
    return YES;
}

// ---------------------------------------------------------------------------
#pragma mark - Toolbar

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar {
    (void)toolbar;
    return @[
        NSToolbarToggleSidebarItemIdentifier,
        NSToolbarSidebarTrackingSeparatorItemIdentifier,
        NSToolbarFlexibleSpaceItemIdentifier,
        kOpenItem,
        kRefreshItem,
        kAuditItem,
    ];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar {
    return [self toolbarDefaultItemIdentifiers:toolbar];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
     itemForItemIdentifier:(NSToolbarItemIdentifier)identifier
 willBeInsertedIntoToolbar:(BOOL)flag {
    (void)toolbar;
    (void)flag;
    NSString *label = nil;
    NSString *symbol = nil;
    NSString *tip = nil;
    SEL action = nullptr;
    id target = self;
    if ([identifier isEqualToString:kOpenItem]) {
        label = @"Open Application";
        symbol = @"plus";
        tip = @"Analyze an application from anywhere on disk";
        action = @selector(openApplication:);
    } else if ([identifier isEqualToString:kRefreshItem]) {
        label = @"Refresh";
        symbol = @"arrow.clockwise";
        tip = @"Scan the Applications folders again";
        action = @selector(refreshApplications:);
    } else if ([identifier isEqualToString:kAuditItem]) {
        label = @"Audit Reports";
        symbol = @"doc.text.magnifyingglass";
        tip = @"Show the JSON audit reports in Finder";
        action = @selector(revealAuditLogs:);
        target = _detail;
    } else {
        return nil;
    }
    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:identifier];
    item.label = label;
    item.paletteLabel = label;
    item.toolTip = tip;
    item.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:label];
    item.bordered = YES;
    item.target = target;
    item.action = action;
    return item;
}

// ---------------------------------------------------------------------------
#pragma mark - Responder chain

/// Menu commands that belong to the detail pane (Scan Again, Clean Selected,
/// Quick Look, Copy Path ...) must work wherever the keyboard focus is — also
/// when it sits in the sidebar's search field, where the detail view controller
/// is not part of the responder chain. The window controller always is, and it
/// offers the detail pane as an additional target.
- (id)supplementalTargetForAction:(SEL)action sender:(id)sender {
    if ([_detail respondsToSelector:action]) return _detail;
    return [super supplementalTargetForAction:action sender:sender];
}

// ---------------------------------------------------------------------------
#pragma mark - Window delegate

- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    [VCEngine.sharedEngine cancelScan];
}

@end
