#import "AppDelegate.h"

#import <ImageIO/ImageIO.h>

#include <cstdio>
#include <cstdlib>

#import "MainWindowController.h"
#import "StorageBreakdownView.h"
#import "VCEngine.h"

namespace {

constexpr int64_t kLaunchCheckTimeoutSeconds = 120;
constexpr CGFloat kSnapshotCornerRadius = 14.0;

NSView *FindDescendant(NSView *root, Class wanted) {
    if ([root isKindOfClass:wanted]) return root;
    for (NSView *child in root.subviews) {
        NSView *found = FindDescendant(child, wanted);
        if (found != nil) return found;
    }
    return nil;
}

NSMenuItem *AddItem(NSMenu *menu, NSString *title, SEL action, NSString *key) {
    NSMenuItem *item = [menu addItemWithTitle:title action:action keyEquivalent:key];
    return item;
}

}  // namespace

@implementation AppDelegate {
    MainWindowController *_windowController;
    BOOL _launchCheckReported;
    id<NSObject> _launchCheckActivity;
}

- (instancetype)init {
    if ((self = [super init])) _hoverSegment = -1;
    return self;
}

// ---------------------------------------------------------------------------
#pragma mark - Launch

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    if ([self.forcedAppearance isEqualToString:@"dark"]) {
        NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    } else if ([self.forcedAppearance isEqualToString:@"light"]) {
        NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    }
    [self buildMainMenu];

    _windowController = [[MainWindowController alloc] init];
    if (self.snapshotPath.length > 0) _windowController.sidebar.rendersOffscreen = YES;
    [_windowController showWindow:nil];
    [_windowController.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    __weak AppDelegate *weakSelf = self;
    if (self.launchCheck) {
        // An automated check often runs with nobody looking at the screen. Declare
        // the activity so App Nap does not defer the check's work and its deadline.
        _launchCheckActivity = [NSProcessInfo.processInfo
            beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                              reason:@"VortexClean launch check"];
        [self armLaunchCheckTimeout];
    }
    [_windowController refreshApplicationsWithCompletion:^{
        AppDelegate *delegate = weakSelf;
        if (delegate != nil && delegate.launchCheck) [delegate runLaunchCheck];
    }];
    if (self.initialApplicationPath.length > 0) {
        [_windowController analyzeApplicationAtPath:self.initialApplicationPath];
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)application {
    (void)application;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    [VCEngine.sharedEngine shutdown];
}

/// Applications dropped on the Dock icon, or opened with `open -a VortexClean Foo.app`.
- (void)application:(NSApplication *)application openURLs:(NSArray<NSURL *> *)urls {
    (void)application;
    for (NSURL *url in urls) {
        if (url.isFileURL && [url.pathExtension caseInsensitiveCompare:@"app"] == NSOrderedSame) {
            if (_windowController != nil) {
                [_windowController analyzeApplicationAtPath:url.path];
            } else {
                self.initialApplicationPath = url.path;  // still launching
            }
            return;
        }
    }
}

// ---------------------------------------------------------------------------
#pragma mark - Menu bar

- (void)buildMainMenu {
    NSString *appName = @"VortexClean";
    NSMenu *mainMenu = [[NSMenu alloc] initWithTitle:@"Main Menu"];

    // ---- Application menu
    NSMenuItem *appItem = [mainMenu addItemWithTitle:appName action:nullptr keyEquivalent:@""];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:appName];
    AddItem(appMenu, [@"About " stringByAppendingString:appName], @selector(orderFrontStandardAboutPanel:), @"");
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *servicesItem = AddItem(appMenu, @"Services", nullptr, @"");
    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    servicesItem.submenu = servicesMenu;
    NSApp.servicesMenu = servicesMenu;
    [appMenu addItem:[NSMenuItem separatorItem]];
    AddItem(appMenu, [@"Hide " stringByAppendingString:appName], @selector(hide:), @"h");
    AddItem(appMenu, @"Hide Others", @selector(hideOtherApplications:), @"h").keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    AddItem(appMenu, @"Show All", @selector(unhideAllApplications:), @"");
    [appMenu addItem:[NSMenuItem separatorItem]];
    AddItem(appMenu, [@"Quit " stringByAppendingString:appName], @selector(terminate:), @"q");
    appItem.submenu = appMenu;

    // ---- File
    NSMenuItem *fileItem = [mainMenu addItemWithTitle:@"File" action:nullptr keyEquivalent:@""];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    AddItem(fileMenu, @"Open Application…", @selector(openApplication:), @"o");
    AddItem(fileMenu, @"Show Audit Reports", @selector(revealAuditLogs:), @"l").keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [fileMenu addItem:[NSMenuItem separatorItem]];
    AddItem(fileMenu, @"Clean Selected…", @selector(cleanSelected:), @"\b");
    [fileMenu addItem:[NSMenuItem separatorItem]];
    AddItem(fileMenu, @"Close Window", @selector(performClose:), @"w");
    fileItem.submenu = fileMenu;

    // ---- Edit
    NSMenuItem *editItem = [mainMenu addItemWithTitle:@"Edit" action:nullptr keyEquivalent:@""];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    AddItem(editMenu, @"Undo", @selector(undo:), @"z");
    AddItem(editMenu, @"Redo", @selector(redo:), @"Z");
    [editMenu addItem:[NSMenuItem separatorItem]];
    AddItem(editMenu, @"Cut", @selector(cut:), @"x");
    AddItem(editMenu, @"Copy", @selector(copy:), @"c");
    AddItem(editMenu, @"Paste", @selector(paste:), @"v");
    AddItem(editMenu, @"Select All", @selector(selectAll:), @"a");
    [editMenu addItem:[NSMenuItem separatorItem]];
    AddItem(editMenu, @"Copy Path", @selector(copySelectedPaths:), @"c").keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    AddItem(editMenu, @"Find", @selector(focusSearch:), @"f");
    editItem.submenu = editMenu;

    // ---- View
    NSMenuItem *viewItem = [mainMenu addItemWithTitle:@"View" action:nullptr keyEquivalent:@""];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    AddItem(viewMenu, @"Show Sidebar", @selector(toggleSidebar:), @"s").keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagControl;
    AddItem(viewMenu, @"Refresh", @selector(refreshApplications:), @"r");
    AddItem(viewMenu, @"Scan Again", @selector(rescan:), @"r").keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:[NSMenuItem separatorItem]];
    AddItem(viewMenu, @"Reveal in Finder", @selector(revealSelectionInFinder:), @"r").keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    AddItem(viewMenu, @"Quick Look", @selector(quickLookSelection:), @"y");
    [viewMenu addItem:[NSMenuItem separatorItem]];
    AddItem(viewMenu, @"Enter Full Screen", @selector(toggleFullScreen:), @"f").keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagControl;
    viewItem.submenu = viewMenu;

    // ---- Window
    NSMenuItem *windowItem = [mainMenu addItemWithTitle:@"Window" action:nullptr keyEquivalent:@""];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    AddItem(windowMenu, @"Minimize", @selector(performMiniaturize:), @"m");
    AddItem(windowMenu, @"Zoom", @selector(performZoom:), @"");
    [windowMenu addItem:[NSMenuItem separatorItem]];
    AddItem(windowMenu, @"Bring All to Front", @selector(arrangeInFront:), @"");
    windowItem.submenu = windowMenu;
    NSApp.windowsMenu = windowMenu;

    // ---- Help
    NSMenuItem *helpItem = [mainMenu addItemWithTitle:@"Help" action:nullptr keyEquivalent:@""];
    NSMenu *helpMenu = [[NSMenu alloc] initWithTitle:@"Help"];
    AddItem(helpMenu, @"Open Full Disk Access Settings…", @selector(openFullDiskAccessSettings:), @"").target = self;
    helpItem.submenu = helpMenu;
    NSApp.helpMenu = helpMenu;

    NSApp.mainMenu = mainMenu;
}

- (void)openFullDiskAccessSettings:(id)sender {
    (void)sender;
    NSURL *url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"];
    if (url != nil) [[NSWorkspace sharedWorkspace] openURL:url];
}

// ---------------------------------------------------------------------------
#pragma mark - Launch check (--launch-check)

- (void)armLaunchCheckTimeout {
    __weak AppDelegate *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kLaunchCheckTimeoutSeconds * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        AppDelegate *delegate = weakSelf;
        if (delegate == nil || delegate->_launchCheckReported) return;
        std::fprintf(stdout, "{\"launch_check\":\"timeout\"}\n");
        std::fflush(stdout);
        std::exit(3);
    });
}

/// Exercises the real UI path end to end: window on screen, sidebar populated,
/// one application selected through the normal code path, its scan completed.
- (void)runLaunchCheck {
    NSArray<VCApp *> *applications = _windowController.applications;
    if (self.launchCheckEmptyState) {
        [self finishLaunchCheckWithApplication:nil result:nil];
        return;
    }
    __weak AppDelegate *weakSelf = self;
    if (self.initialApplicationPath.length > 0) {
        // An application named on the command line is the one to check, or nothing is:
        // quietly checking some other application would hide a problem.
        [VCEngine.sharedEngine inspectApplicationAtPath:self.initialApplicationPath
                                             completion:^(VCApp *app, NSString *error) {
            if (app == nil) {
                std::fprintf(stdout, "{\"launch_check\":\"cannot open the application\"}\n");
                std::fprintf(stderr, "VortexClean: %s\n", error.UTF8String);
                std::fflush(stdout);
                std::exit(4);
            }
            [weakSelf launchCheckApplication:app];
        }];
        return;
    }
    VCApp *target = nil;
    // Otherwise prefer the smallest third-party application, so the check stays quick.
    for (VCApp *candidate in applications) {
        if (candidate.isSipProtected || candidate.isAppleApp || candidate.bundleIdentifier.length == 0) continue;
        if (target == nil || candidate.allocatedBytes < target.allocatedBytes) target = candidate;
    }
    if (target == nil) target = applications.firstObject;
    if (target == nil) {
        [self finishLaunchCheckWithApplication:nil result:nil];
        return;
    }
    [self launchCheckApplication:target];
}

- (void)launchCheckApplication:(VCApp *)target {
    __weak AppDelegate *weakSelf = self;
    _windowController.detail.scanDidFinish = ^(VCScanResult *result) {
        [weakSelf finishLaunchCheckWithApplication:target result:result];
    };
    [_windowController selectApplication:target];
}

- (void)finishLaunchCheckWithApplication:(VCApp *)application result:(VCScanResult *)result {
    if (_launchCheckReported) return;
    _launchCheckReported = YES;
    NSWindow *window = _windowController.window;
    uint64_t bytes = 0;
    for (VCRemnant *remnant in result.remnants) bytes += remnant.allocatedBytes;
    NSDictionary<NSString *, id> *summary = @{
        @"launch_check": @"ok",
        @"window_visible": @(window.isVisible),
        @"window_size": @[@(window.frame.size.width), @(window.frame.size.height)],
        @"applications": @(_windowController.applications.count),
        @"scanned": application != nil ? static_cast<id>(application.displayName) : [NSNull null],
        @"scanned_bundle_id": application != nil ? static_cast<id>(application.bundleIdentifier) : [NSNull null],
        @"items_found": @(result.remnants.count),
        @"bytes_found": @(bytes),
        @"scan_seconds": @(result.elapsed),
        @"scan_cancelled": @(result.cancelled),
    };
    __block NSInteger hovered = -1;
    void (^report)(BOOL) = ^(BOOL snapshotWritten) {
        NSMutableDictionary<NSString *, id> *output = [summary mutableCopy];
        if (self.snapshotPath.length > 0) output[@"snapshot_written"] = @(snapshotWritten);
        if (self.hoverSegment >= 0) output[@"hover_highlighted_segment"] = @(hovered);
        NSData *json = [NSJSONSerialization dataWithJSONObject:output options:NSJSONWritingSortedKeys error:nil];
        std::fwrite(json.bytes, 1, json.length, stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        // Leave through the normal termination path so shutdown is exercised too.
        [NSApp terminate:nil];
    };
    if (self.snapshotPath.length == 0) {
        dispatch_async(dispatch_get_main_queue(), ^{
            report(NO);
        });
        return;
    }
    // Give layout, icon loading and the bar animation a moment to settle, once.
    [window makeFirstResponder:nil];  // no caret or focus ring in the picture
    if (self.hoverSegment >= 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.7 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            hovered = [self hoverOverStorageSegment:self.hoverSegment inWindow:window];
        });
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(1.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        report([self writeSnapshotOfWindow:window toPath:self.snapshotPath]);
    });
}

/// Delivers a pointer-moved event to the storage bar over the middle of one of
/// its segments (the same call AppKit makes for the view's tracking area) and
/// returns the segment the view then reports as highlighted.
- (NSInteger)hoverOverStorageSegment:(NSInteger)segment inWindow:(NSWindow *)window {
    StorageBreakdownView *bar =
        static_cast<StorageBreakdownView *>(FindDescendant(window.contentView, [StorageBreakdownView class]));
    if (bar == nil || segment < 0) return -1;
    [bar layoutSubtreeIfNeeded];
    const NSRect frame = [bar frameOfSegmentAtIndex:static_cast<NSUInteger>(segment)];
    if (NSIsEmptyRect(frame)) return -1;
    const NSPoint location = [bar convertPoint:NSMakePoint(NSMidX(frame), NSMidY(frame)) toView:nil];
    NSEvent *event = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                        location:location
                                   modifierFlags:0
                                       timestamp:NSProcessInfo.processInfo.systemUptime
                                    windowNumber:window.windowNumber
                                         context:nil
                                     eventNumber:0
                                      clickCount:0
                                        pressure:0.0f];
    if (event == nil) return -1;
    [bar mouseMoved:event];
    return bar.highlightedIndex;
}

/// Renders the window's own view hierarchy (title bar included) into a PNG file:
/// sRGB, with the window's rounded corners left transparent.
- (BOOL)writeSnapshotOfWindow:(NSWindow *)window toPath:(NSString *)path {
    NSView *root = window.contentView.superview != nil ? window.contentView.superview : window.contentView;
    if (root == nil) return NO;
    [root layoutSubtreeIfNeeded];
    [root displayIfNeeded];
    NSBitmapImageRep *bitmap = [root bitmapImageRepForCachingDisplayInRect:root.bounds];
    if (bitmap == nil) return NO;
    [root cacheDisplayInRect:root.bounds toBitmapImageRep:bitmap];

    const auto width = static_cast<size_t>(bitmap.pixelsWide);
    const auto height = static_cast<size_t>(bitmap.pixelsHigh);
    const CGFloat scale = static_cast<CGFloat>(width) / MAX(root.bounds.size.width, 1.0);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(nullptr, width, height, 8, 0, space,
                                                 static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
    CGColorSpaceRelease(space);
    if (context == nullptr) return NO;
    const CGRect frame = CGRectMake(0.0, 0.0, static_cast<CGFloat>(width), static_cast<CGFloat>(height));
    CGPathRef outline =
        CGPathCreateWithRoundedRect(frame, kSnapshotCornerRadius * scale, kSnapshotCornerRadius * scale, nullptr);
    CGContextAddPath(context, outline);
    CGContextClip(context);
    CGPathRelease(outline);
    CGContextDrawImage(context, frame, bitmap.CGImage);
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    if (image == nullptr) return NO;

    BOOL written = NO;
    CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, nullptr);
    if (destination != nullptr) {
        CGImageDestinationAddImage(destination, image, nullptr);
        written = CGImageDestinationFinalize(destination);
        CFRelease(destination);
    }
    CGImageRelease(image);
    return written;
}

@end
