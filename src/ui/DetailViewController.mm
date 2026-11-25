#import "DetailViewController.h"

#import <Quartz/Quartz.h>

#include <algorithm>

#import "StorageBreakdownView.h"
#import "VCDetailViews.h"
#import "VCEngine.h"
#import "VCFormat.h"

namespace {

/// Set when the user closes the Full Disk Access notice: a scan does not show it again.
bool gFullDiskAccessNoticeDismissed = false;

constexpr CGFloat kContentInset = 20.0;
constexpr CGFloat kGroupRowHeight = 28.0;
constexpr CGFloat kItemRowHeight = 24.0;
constexpr NSUInteger kAutoExpandLimit = 250;
constexpr NSInteger kCategoryPrivilegedHelperTools = 14;  // RemnantCategory::PrivilegedHelperTools

NSString *const kItemColumn = @"item";
NSString *const kMatchColumn = @"match";
NSString *const kSizeColumn = @"size";
NSString *const kFullDiskAccessURL = @"x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles";

}  // namespace

@interface DetailViewController () <NSOutlineViewDataSource,
                                    NSOutlineViewDelegate,
                                    NSMenuDelegate,
                                    QLPreviewPanelDataSource,
                                    QLPreviewPanelDelegate>
@end

@implementation DetailViewController {
    // ---- Views
    VCDropZoneView *_emptyView;
    NSView *_contentView;
    VCDropOverlayView *_dropOverlay;
    VCBannerView *_banner;
    NSImageView *_iconView;
    NSTextField *_nameLabel;
    NSTextField *_identifierLabel;
    NSTextField *_versionLabel;
    NSImageView *_signingIcon;
    NSTextField *_signingLabel;
    VCPillView *_runningPill;
    NSButton *_quitButton;
    NSTextField *_reclaimLabel;
    NSTextField *_reclaimCaption;
    NSTextField *_notesLabel;
    StorageBreakdownView *_storageView;
    VCOutlineView *_outlineView;
    NSScrollView *_scrollView;
    NSTextField *_statusLabel;
    NSProgressIndicator *_spinner;
    NSProgressIndicator *_progressBar;
    NSSwitch *_dryRunSwitch;
    NSSegmentedControl *_modeControl;
    NSButton *_cleanButton;

    // ---- State
    NSArray<VCRemnantGroup *> *_groups;
    NSMutableDictionary<NSNumber *, VCRemnantNode *> *_nodesByIndex;
    NSArray<VCProcess *> *_processes;
    NSArray<NSURL *> *_previewURLs;
    NSUInteger _scanID;
    BOOL _scanning;
    BOOL _cleaning;
    __weak NSButton *_permanentConfirmButton;  // enabled only once the acknowledgement box is ticked
    NSLayoutConstraint *_statusLeadingPlain;
    NSLayoutConstraint *_statusLeadingAfterSpinner;
}

// ---------------------------------------------------------------------------
#pragma mark - View construction

- (void)loadView {
    NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 820, 640)];
    self.view = root;
    _groups = @[];
    _nodesByIndex = [NSMutableDictionary dictionary];
    _processes = @[];
    _previewURLs = @[];

    _emptyView = [[VCDropZoneView alloc] initWithFrame:NSZeroRect];
    _emptyView.chooseButton.target = self;
    _emptyView.chooseButton.action = @selector(chooseApplication:);
    [root addSubview:_emptyView];

    _contentView = [[NSView alloc] initWithFrame:NSZeroRect];
    _contentView.translatesAutoresizingMaskIntoConstraints = NO;
    _contentView.hidden = YES;
    [root addSubview:_contentView];

    _dropOverlay = [[VCDropOverlayView alloc] initWithFrame:NSZeroRect];
    _dropOverlay.hidden = YES;
    [root addSubview:_dropOverlay];

    NSView *header = [self makeHeader];
    _banner = [[VCBannerView alloc] initWithSymbol:@"lock.shield"
                                              tint:NSColor.systemYellowColor
                                       actionTitle:@"Open System Settings"];
    _banner.message = @"Some items could not be read. Grant VortexClean Full Disk Access to inspect protected "
                      @"locations such as other apps’ containers.";
    _banner.actionButton.target = self;
    _banner.actionButton.action = @selector(openFullDiskAccessSettings:);
    _banner.closeButton.target = self;
    _banner.closeButton.action = @selector(dismissBanner:);
    _banner.hidden = YES;

    _notesLabel = VCMakeLabel(@"", [NSFont systemFontOfSize:11.5], NSColor.secondaryLabelColor);
    _notesLabel.lineBreakMode = NSLineBreakByWordWrapping;
    _notesLabel.maximumNumberOfLines = 3;
    _notesLabel.hidden = YES;

    _storageView = [[StorageBreakdownView alloc] initWithFrame:NSZeroRect];

    NSStackView *topStack = [NSStackView stackViewWithViews:@[_banner, header, _notesLabel, _storageView]];
    topStack.translatesAutoresizingMaskIntoConstraints = NO;
    topStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    topStack.alignment = NSLayoutAttributeLeading;
    topStack.distribution = NSStackViewDistributionFill;
    topStack.spacing = 14.0;
    topStack.detachesHiddenViews = YES;  // a hidden banner takes no space
    [_contentView addSubview:topStack];
    for (NSView *arranged in topStack.arrangedSubviews) {
        [arranged.widthAnchor constraintEqualToAnchor:topStack.widthAnchor].active = YES;
    }

    [self makeOutline];
    [_contentView addSubview:_scrollView];
    NSView *footer = [self makeFooter];
    [_contentView addSubview:footer];

    NSLayoutGuide *safeArea = root.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_emptyView.topAnchor constraintEqualToAnchor:safeArea.topAnchor],
        [_emptyView.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [_emptyView.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [_emptyView.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],

        [_contentView.topAnchor constraintEqualToAnchor:safeArea.topAnchor],
        [_contentView.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [_contentView.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [_contentView.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],

        [_dropOverlay.topAnchor constraintEqualToAnchor:root.topAnchor],
        [_dropOverlay.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [_dropOverlay.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [_dropOverlay.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],

        [topStack.topAnchor constraintEqualToAnchor:_contentView.topAnchor constant:14.0],
        [topStack.leadingAnchor constraintEqualToAnchor:_contentView.leadingAnchor constant:kContentInset],
        [topStack.trailingAnchor constraintEqualToAnchor:_contentView.trailingAnchor constant:-kContentInset],

        [_scrollView.topAnchor constraintEqualToAnchor:topStack.bottomAnchor constant:12.0],
        [_scrollView.leadingAnchor constraintEqualToAnchor:_contentView.leadingAnchor constant:8.0],
        [_scrollView.trailingAnchor constraintEqualToAnchor:_contentView.trailingAnchor constant:-8.0],
        [_scrollView.bottomAnchor constraintEqualToAnchor:footer.topAnchor],
        [_scrollView.heightAnchor constraintGreaterThanOrEqualToConstant:120.0],

        [footer.leadingAnchor constraintEqualToAnchor:_contentView.leadingAnchor],
        [footer.trailingAnchor constraintEqualToAnchor:_contentView.trailingAnchor],
        [footer.bottomAnchor constraintEqualToAnchor:_contentView.bottomAnchor],
        [footer.heightAnchor constraintEqualToConstant:54.0],
    ]];
}

- (NSView *)makeHeader {
    NSView *header = [[NSView alloc] initWithFrame:NSZeroRect];
    header.translatesAutoresizingMaskIntoConstraints = NO;

    _iconView = [[NSImageView alloc] initWithFrame:NSZeroRect];
    _iconView.translatesAutoresizingMaskIntoConstraints = NO;
    _iconView.imageScaling = NSImageScaleProportionallyUpOrDown;
    [header addSubview:_iconView];

    _nameLabel = VCMakeLabel(@"", [NSFont systemFontOfSize:22.0 weight:NSFontWeightBold], NSColor.labelColor);
    [header addSubview:_nameLabel];

    _runningPill = [[VCPillView alloc] initWithFrame:NSZeroRect];
    _runningPill.tintColor = NSColor.systemGreenColor;
    _runningPill.image = VCSymbol(@"circle.fill", 7.0, NSFontWeightRegular);
    _runningPill.hidden = YES;
    [header addSubview:_runningPill];

    _quitButton = [NSButton buttonWithTitle:@"Quit Processes" target:self action:@selector(quitProcesses:)];
    _quitButton.translatesAutoresizingMaskIntoConstraints = NO;
    _quitButton.bezelStyle = NSBezelStyleRounded;
    _quitButton.controlSize = NSControlSizeSmall;
    _quitButton.hidden = YES;
    [header addSubview:_quitButton];

    _identifierLabel = VCMakeLabel(@"", [NSFont monospacedSystemFontOfSize:11.0 weight:NSFontWeightRegular],
                                   NSColor.secondaryLabelColor);
    _identifierLabel.selectable = YES;
    [header addSubview:_identifierLabel];

    _versionLabel = VCMakeLabel(@"", [NSFont systemFontOfSize:12.0], NSColor.secondaryLabelColor);
    [header addSubview:_versionLabel];

    _signingIcon = [[NSImageView alloc] initWithFrame:NSZeroRect];
    _signingIcon.translatesAutoresizingMaskIntoConstraints = NO;
    [header addSubview:_signingIcon];
    _signingLabel = VCMakeLabel(@"", [NSFont systemFontOfSize:12.0], NSColor.secondaryLabelColor);
    [header addSubview:_signingLabel];

    NSTextField *reclaimTitle =
        VCMakeLabel(@"RECLAIMABLE", [NSFont systemFontOfSize:10.0 weight:NSFontWeightSemibold], NSColor.tertiaryLabelColor);
    reclaimTitle.alignment = NSTextAlignmentRight;
    [header addSubview:reclaimTitle];
    _reclaimLabel = VCMakeLabel(@"—", [NSFont monospacedDigitSystemFontOfSize:26.0 weight:NSFontWeightSemibold],
                                NSColor.labelColor);
    _reclaimLabel.alignment = NSTextAlignmentRight;
    [_reclaimLabel setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                            forOrientation:NSLayoutConstraintOrientationHorizontal];
    [header addSubview:_reclaimLabel];
    _reclaimCaption = VCMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:11.0 weight:NSFontWeightRegular],
                                  NSColor.secondaryLabelColor);
    _reclaimCaption.alignment = NSTextAlignmentRight;
    [_reclaimCaption setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                              forOrientation:NSLayoutConstraintOrientationHorizontal];
    [header addSubview:_reclaimCaption];

    [NSLayoutConstraint activateConstraints:@[
        [header.heightAnchor constraintEqualToConstant:84.0],
        [_iconView.leadingAnchor constraintEqualToAnchor:header.leadingAnchor],
        [_iconView.centerYAnchor constraintEqualToAnchor:header.centerYAnchor],
        [_iconView.widthAnchor constraintEqualToConstant:72.0],
        [_iconView.heightAnchor constraintEqualToConstant:72.0],

        [_nameLabel.leadingAnchor constraintEqualToAnchor:_iconView.trailingAnchor constant:14.0],
        [_nameLabel.topAnchor constraintEqualToAnchor:header.topAnchor constant:2.0],
        [_runningPill.leadingAnchor constraintEqualToAnchor:_nameLabel.trailingAnchor constant:10.0],
        [_runningPill.centerYAnchor constraintEqualToAnchor:_nameLabel.centerYAnchor constant:1.0],
        [_quitButton.leadingAnchor constraintEqualToAnchor:_runningPill.trailingAnchor constant:8.0],
        [_quitButton.centerYAnchor constraintEqualToAnchor:_runningPill.centerYAnchor],
        [_quitButton.trailingAnchor constraintLessThanOrEqualToAnchor:_reclaimLabel.leadingAnchor constant:-12.0],

        [_identifierLabel.leadingAnchor constraintEqualToAnchor:_nameLabel.leadingAnchor],
        [_identifierLabel.topAnchor constraintEqualToAnchor:_nameLabel.bottomAnchor constant:2.0],
        [_identifierLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_reclaimLabel.leadingAnchor constant:-12.0],

        [_versionLabel.leadingAnchor constraintEqualToAnchor:_nameLabel.leadingAnchor],
        [_versionLabel.topAnchor constraintEqualToAnchor:_identifierLabel.bottomAnchor constant:3.0],
        [_versionLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_reclaimCaption.leadingAnchor constant:-12.0],

        [_signingIcon.leadingAnchor constraintEqualToAnchor:_nameLabel.leadingAnchor],
        [_signingIcon.topAnchor constraintEqualToAnchor:_versionLabel.bottomAnchor constant:3.0],
        [_signingIcon.widthAnchor constraintEqualToConstant:14.0],
        [_signingIcon.heightAnchor constraintEqualToConstant:14.0],
        [_signingLabel.leadingAnchor constraintEqualToAnchor:_signingIcon.trailingAnchor constant:4.0],
        [_signingLabel.centerYAnchor constraintEqualToAnchor:_signingIcon.centerYAnchor],
        [_signingLabel.trailingAnchor constraintLessThanOrEqualToAnchor:header.trailingAnchor],

        [reclaimTitle.trailingAnchor constraintEqualToAnchor:header.trailingAnchor],
        [reclaimTitle.topAnchor constraintEqualToAnchor:header.topAnchor constant:4.0],
        [_reclaimLabel.trailingAnchor constraintEqualToAnchor:header.trailingAnchor],
        [_reclaimLabel.topAnchor constraintEqualToAnchor:reclaimTitle.bottomAnchor constant:0.0],
        [_reclaimCaption.trailingAnchor constraintEqualToAnchor:header.trailingAnchor],
        [_reclaimCaption.topAnchor constraintEqualToAnchor:_reclaimLabel.bottomAnchor constant:0.0],
    ]];
    [_nameLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow - 1
                                         forOrientation:NSLayoutConstraintOrientationHorizontal];
    return header;
}

- (void)makeOutline {
    _outlineView = [[VCOutlineView alloc] initWithFrame:NSZeroRect];
    _outlineView.style = NSTableViewStyleInset;
    _outlineView.rowSizeStyle = NSTableViewRowSizeStyleCustom;
    _outlineView.usesAlternatingRowBackgroundColors = NO;
    _outlineView.allowsMultipleSelection = YES;
    _outlineView.allowsColumnReordering = NO;
    _outlineView.allowsColumnResizing = YES;
    _outlineView.indentationPerLevel = 12.0;
    _outlineView.autoresizesOutlineColumn = NO;
    _outlineView.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;
    _outlineView.dataSource = self;
    _outlineView.delegate = self;
    _outlineView.target = self;
    _outlineView.doubleAction = @selector(revealSelectionInFinder:);
    [_outlineView setAccessibilityLabel:@"Items found"];

    NSTableColumn *item = [[NSTableColumn alloc] initWithIdentifier:kItemColumn];
    item.title = @"Item";
    item.minWidth = 240.0;
    item.width = 420.0;
    item.resizingMask = NSTableColumnAutoresizingMask | NSTableColumnUserResizingMask;
    [_outlineView addTableColumn:item];
    _outlineView.outlineTableColumn = item;

    NSTableColumn *match = [[NSTableColumn alloc] initWithIdentifier:kMatchColumn];
    match.title = @"Match";
    match.minWidth = 150.0;
    match.width = 236.0;
    match.resizingMask = NSTableColumnUserResizingMask;
    [_outlineView addTableColumn:match];

    NSTableColumn *size = [[NSTableColumn alloc] initWithIdentifier:kSizeColumn];
    size.title = @"Size on Disk";
    size.minWidth = 84.0;
    size.width = 96.0;
    size.resizingMask = NSTableColumnUserResizingMask;
    size.headerCell.alignment = NSTextAlignmentRight;
    [_outlineView addTableColumn:size];

    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Item"];
    menu.delegate = self;
    [menu addItemWithTitle:@"Reveal in Finder" action:@selector(revealSelectionInFinder:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"Copy Path" action:@selector(copySelectedPaths:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"Quick Look" action:@selector(quickLookSelection:) keyEquivalent:@""].target = self;
    _outlineView.menu = menu;

    _scrollView = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.documentView = _outlineView;
    _scrollView.hasVerticalScroller = YES;
    _scrollView.hasHorizontalScroller = NO;
    _scrollView.autohidesScrollers = YES;
    _scrollView.borderType = NSNoBorder;
    _scrollView.drawsBackground = NO;
}

- (NSView *)makeFooter {
    NSView *footer = [[NSView alloc] initWithFrame:NSZeroRect];
    footer.translatesAutoresizingMaskIntoConstraints = NO;

    NSBox *separator = [[NSBox alloc] initWithFrame:NSZeroRect];
    separator.translatesAutoresizingMaskIntoConstraints = NO;
    separator.boxType = NSBoxSeparator;
    [footer addSubview:separator];

    _spinner = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    _spinner.translatesAutoresizingMaskIntoConstraints = NO;
    _spinner.style = NSProgressIndicatorStyleSpinning;
    _spinner.controlSize = NSControlSizeSmall;
    _spinner.displayedWhenStopped = NO;
    [footer addSubview:_spinner];

    _statusLabel = VCMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:12.0 weight:NSFontWeightRegular],
                               NSColor.secondaryLabelColor);
    [footer addSubview:_statusLabel];

    _progressBar = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    _progressBar.translatesAutoresizingMaskIntoConstraints = NO;
    _progressBar.style = NSProgressIndicatorStyleBar;
    _progressBar.indeterminate = NO;
    _progressBar.minValue = 0.0;
    _progressBar.maxValue = 1.0;
    _progressBar.controlSize = NSControlSizeSmall;
    _progressBar.hidden = YES;
    [footer addSubview:_progressBar];

    NSTextField *dryRunLabel = VCMakeLabel(@"Dry Run", [NSFont systemFontOfSize:12.0], NSColor.labelColor);
    [dryRunLabel setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    dryRunLabel.toolTip = @"Simulate the clean-up: everything is validated and logged, nothing is touched.";
    [footer addSubview:dryRunLabel];
    _dryRunSwitch = [[NSSwitch alloc] initWithFrame:NSZeroRect];
    _dryRunSwitch.translatesAutoresizingMaskIntoConstraints = NO;
    _dryRunSwitch.controlSize = NSControlSizeSmall;
    _dryRunSwitch.target = self;
    _dryRunSwitch.action = @selector(optionsChanged:);
    _dryRunSwitch.toolTip = dryRunLabel.toolTip;
    [_dryRunSwitch setAccessibilityLabel:@"Dry run"];
    [footer addSubview:_dryRunSwitch];

    _modeControl = [NSSegmentedControl segmentedControlWithLabels:@[@"Move to Trash", @"Delete Permanently"]
                                                    trackingMode:NSSegmentSwitchTrackingSelectOne
                                                          target:self
                                                          action:@selector(optionsChanged:)];
    _modeControl.translatesAutoresizingMaskIntoConstraints = NO;
    _modeControl.selectedSegment = VCDeleteModeTrash;  // the safe default
    [_modeControl setToolTip:@"Items in the Trash can be put back until it is emptied." forSegment:0];
    [_modeControl setToolTip:@"Removes the items immediately. This cannot be undone." forSegment:1];
    [_modeControl setAccessibilityLabel:@"Delete mode"];
    [footer addSubview:_modeControl];

    _cleanButton = [NSButton buttonWithTitle:@"Clean Selected" target:self action:@selector(cleanSelected:)];
    _cleanButton.translatesAutoresizingMaskIntoConstraints = NO;
    _cleanButton.bezelStyle = NSBezelStyleRounded;
    _cleanButton.controlSize = NSControlSizeLarge;
    _cleanButton.hasDestructiveAction = YES;
    _cleanButton.enabled = NO;
    [_cleanButton setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                           forOrientation:NSLayoutConstraintOrientationHorizontal];
    [footer addSubview:_cleanButton];

    [NSLayoutConstraint activateConstraints:@[
        [separator.topAnchor constraintEqualToAnchor:footer.topAnchor],
        [separator.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor],
        [separator.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor],
        [separator.heightAnchor constraintEqualToConstant:1.0],

        [_spinner.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor constant:kContentInset],
        [_spinner.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
        [_statusLabel.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
        [_statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_progressBar.leadingAnchor constant:-12.0],

        [_progressBar.trailingAnchor constraintEqualToAnchor:dryRunLabel.leadingAnchor constant:-16.0],
        [_progressBar.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
        [_progressBar.widthAnchor constraintEqualToConstant:120.0],

        [dryRunLabel.trailingAnchor constraintEqualToAnchor:_dryRunSwitch.leadingAnchor constant:-6.0],
        [dryRunLabel.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
        [_dryRunSwitch.trailingAnchor constraintEqualToAnchor:_modeControl.leadingAnchor constant:-14.0],
        [_dryRunSwitch.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
        [_modeControl.trailingAnchor constraintEqualToAnchor:_cleanButton.leadingAnchor constant:-14.0],
        [_modeControl.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
        [_cleanButton.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor constant:-kContentInset],
        [_cleanButton.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
    ]];
    // The status text starts at the edge, or after the spinner while one is shown.
    _statusLeadingPlain = [_statusLabel.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor
                                                                      constant:kContentInset];
    _statusLeadingAfterSpinner = [_statusLabel.leadingAnchor constraintEqualToAnchor:_spinner.trailingAnchor
                                                                             constant:7.0];
    _statusLeadingPlain.active = YES;
    return footer;
}

- (void)setSpinnerVisible:(BOOL)visible {
    if (visible) {
        _statusLeadingPlain.active = NO;
        _statusLeadingAfterSpinner.active = YES;
        [_spinner startAnimation:nil];
    } else {
        _statusLeadingAfterSpinner.active = NO;
        _statusLeadingPlain.active = YES;
        [_spinner stopAnimation:nil];
    }
}

- (void)viewDidLoad {
    [super viewDidLoad];
    // Event driven: re-inspect processes when an application launches or quits.
    NSNotificationCenter *center = NSWorkspace.sharedWorkspace.notificationCenter;
    [center addObserver:self
               selector:@selector(workspaceApplicationsChanged:)
                   name:NSWorkspaceDidLaunchApplicationNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(workspaceApplicationsChanged:)
                   name:NSWorkspaceDidTerminateApplicationNotification
                 object:nil];
    [self updateControls];
}

- (void)dealloc {
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
}

// ---------------------------------------------------------------------------
#pragma mark - Showing an application

- (BOOL)isBusy {
    return _scanning || _cleaning;
}

- (void)showApplication:(VCApp *)application {
    (void)self.view;  // make sure the view hierarchy exists
    if ([QLPreviewPanel sharedPreviewPanelExists] && [QLPreviewPanel sharedPreviewPanel].isVisible) {
        [[QLPreviewPanel sharedPreviewPanel] orderOut:nil];
    }
    _application = application;
    _groups = @[];
    [_nodesByIndex removeAllObjects];
    _processes = @[];
    _banner.hidden = YES;
    _notesLabel.hidden = YES;
    [_outlineView reloadData];

    if (application == nil) {
        [VCEngine.sharedEngine cancelScan];
        _scanning = NO;
        _contentView.hidden = YES;
        _emptyView.hidden = NO;
        [self setSpinnerVisible:NO];
        [self updateControls];
        return;
    }
    _emptyView.hidden = YES;
    _contentView.hidden = NO;
    [self updateHeader];
    [_storageView setSegments:@[] animated:NO];
    [self startScan];
}

- (void)applicationsDidChange:(NSArray<VCApp *> *)applications {
    if (_application == nil) return;
    for (VCApp *candidate in applications) {
        if ([candidate.bundlePath isEqualToString:_application.bundlePath]) {
            _application = candidate;  // may carry a size that was measured meanwhile
            [self updateHeader];
            return;
        }
    }
}

- (void)updateHeader {
    VCApp *app = _application;
    if (app == nil) return;
    _nameLabel.stringValue = app.displayName;
    _identifierLabel.stringValue = app.bundleIdentifier.length > 0 ? app.bundleIdentifier : @"No bundle identifier";

    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    if (app.version.length > 0) {
        [parts addObject:app.build.length > 0 && ![app.build isEqualToString:app.version]
                             ? [NSString stringWithFormat:@"Version %@ (%@)", app.version, app.build]
                             : [NSString stringWithFormat:@"Version %@", app.version]];
    }
    if (app.usageKnown) [parts addObject:[NSString stringWithFormat:@"%@ on disk", VCFormatBytes(app.allocatedBytes)]];
    if (app.isAppStore) [parts addObject:@"Mac App Store"];
    if (app.isExternal) [parts addObject:@"Outside Applications"];
    _versionLabel.stringValue = [parts componentsJoinedByString:@" · "];
    _versionLabel.toolTip = app.bundlePath;

    NSString *symbol = @"checkmark.seal.fill";
    NSColor *tint = NSColor.systemGreenColor;
    NSString *signing = app.signingSummary;
    if (app.isSipProtected) {
        symbol = @"lock.fill";
        tint = NSColor.secondaryLabelColor;
        signing = [NSString stringWithFormat:@"%@ · protected by System Integrity Protection", app.signingSummary];
    } else if (!app.isSigned) {
        symbol = @"exclamationmark.triangle.fill";
        tint = NSColor.systemOrangeColor;
    } else if (app.isAdHocSigned) {
        symbol = @"seal";
        tint = NSColor.systemOrangeColor;
    } else if (app.teamIdentifier.length > 0) {
        signing = [NSString stringWithFormat:@"%@", app.signingSummary];
    }
    _signingIcon.image = VCSymbol(symbol, 12.0, NSFontWeightMedium);
    _signingIcon.contentTintColor = tint;
    _signingLabel.stringValue = signing;
    _signingLabel.toolTip = app.teamIdentifier.length > 0 ? [@"Team ID: " stringByAppendingString:app.teamIdentifier] : nil;

    NSString *path = app.bundlePath;
    NSImage *cached = [VCIconCache.sharedCache cachedIconForPath:path];
    _iconView.image = cached != nil ? cached : [NSImage imageNamed:NSImageNameApplicationIcon];
    if (cached == nil) {
        __weak DetailViewController *weakSelf = self;
        [VCIconCache.sharedCache loadIconForPath:path
                                      completion:^(NSImage *icon) {
                                          DetailViewController *controller = weakSelf;
                                          if (controller != nil &&
                                              [controller->_application.bundlePath isEqualToString:path]) {
                                              controller->_iconView.image = icon;
                                          }
                                      }];
    }
}

// ---------------------------------------------------------------------------
#pragma mark - Scanning

- (IBAction)rescan:(id)sender {
    (void)sender;
    if (_application != nil && !_cleaning) [self startScan];
}

- (void)startScan {
    VCApp *app = _application;
    if (app == nil) return;
    _scanning = YES;
    _storageView.measuring = YES;
    [self setSpinnerVisible:YES];
    [self updateControls];

    __weak DetailViewController *weakSelf = self;
    _scanID = [VCEngine.sharedEngine scanApplication:app
        discovered:^(NSUInteger scanID, VCScanResult *result) {
            DetailViewController *controller = weakSelf;
            if (controller == nil || scanID != controller->_scanID) return;  // a newer scan superseded this one
            [controller applyDiscovered:result];
        }
        sized:^(NSUInteger scanID, NSArray<VCRemnant *> *updated) {
            DetailViewController *controller = weakSelf;
            if (controller == nil || scanID != controller->_scanID) return;
            [controller applySized:updated];
        }
        completion:^(NSUInteger scanID, VCScanResult *result) {
            DetailViewController *controller = weakSelf;
            if (controller == nil || scanID != controller->_scanID) return;
            [controller applyCompleted:result];
        }];
}

/// Builds the tree from a fresh list of items (their sizes arrive later).
- (void)applyDiscovered:(VCScanResult *)result {
    NSMutableArray<VCRemnantGroup *> *groups = [NSMutableArray array];
    NSMutableDictionary<NSNumber *, VCRemnantGroup *> *byCategory = [NSMutableDictionary dictionary];
    [_nodesByIndex removeAllObjects];
    for (VCRemnant *remnant in result.remnants) {
        VCRemnantGroup *group = byCategory[@(remnant.category)];
        if (group == nil) {
            group = [[VCRemnantGroup alloc] init];
            group.category = remnant.category;
            group.title = remnant.categoryName;
            byCategory[@(remnant.category)] = group;
            [groups addObject:group];  // the engine already returns items ordered by category
        }
        VCRemnantNode *node = [[VCRemnantNode alloc] init];
        node.remnant = remnant;
        node.checked = remnant.defaultSelected && !remnant.sipProtected;
        node.group = group;
        [group.nodes addObject:node];
        _nodesByIndex[@(remnant.index)] = node;
    }
    _groups = [groups copy];
    [_outlineView reloadData];
    for (VCRemnantGroup *group in _groups) {
        if (group.nodes.count <= kAutoExpandLimit) [_outlineView expandItem:group];
    }
    [self updateSummary];
    [self refreshProcesses];
}

/// Folds measured sizes into the tree and redraws only the rows that changed.
- (void)applySized:(NSArray<VCRemnant *> *)updated {
    NSMutableIndexSet *rows = [NSMutableIndexSet indexSet];
    for (VCRemnant *remnant in updated) {
        VCRemnantNode *node = _nodesByIndex[@(remnant.index)];
        if (node == nil) continue;
        node.remnant = remnant;
        const NSInteger row = [_outlineView rowForItem:node];
        if (row >= 0) [rows addIndex:static_cast<NSUInteger>(row)];
        VCRemnantGroup *group = node.group;
        const NSInteger groupRow = group != nil ? [_outlineView rowForItem:group] : -1;
        if (groupRow >= 0) [rows addIndex:static_cast<NSUInteger>(groupRow)];
    }
    if (rows.count > 0) {
        [_outlineView reloadDataForRowIndexes:rows
                                columnIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, 3)]];
    }
    [self updateSummary];
}

- (void)applyCompleted:(VCScanResult *)result {
    _scanning = NO;
    _storageView.measuring = NO;
    [self setSpinnerVisible:NO];
    if (!result.cancelled) {
        // The final result is authoritative for flags that are only known after sizing.
        for (VCRemnant *remnant in result.remnants) {
            VCRemnantNode *node = _nodesByIndex[@(remnant.index)];
            if (node != nil) node.remnant = remnant;
        }
        [_outlineView reloadDataForRowIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, static_cast<NSUInteger>(_outlineView.numberOfRows))]
                                columnIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, 3)]];
        if (result.notes.count > 0) {
            _notesLabel.stringValue = [result.notes componentsJoinedByString:@"  "];
            _notesLabel.hidden = NO;
        }
        if (result.permissionDenied && !gFullDiskAccessNoticeDismissed) [self showBanner];
    }
    [self updateSummary];
    [self updateControls];
    if (self.scanDidFinish != nil) self.scanDidFinish(result);
}

// ---------------------------------------------------------------------------
#pragma mark - Selection summary and storage breakdown

/// Checked nodes with nested selections removed, so nothing is counted or deleted twice.
- (NSArray<VCRemnantNode *> *)effectiveSelection {
    NSMutableArray<VCRemnantNode *> *checked = [NSMutableArray array];
    for (VCRemnantGroup *group in _groups) {
        for (VCRemnantNode *node in group.nodes) {
            if (node.checked && node.selectable) [checked addObject:node];
        }
    }
    NSMutableSet<NSString *> *directories = [NSMutableSet set];
    for (VCRemnantNode *node in checked) {
        if (node.remnant.isDirectory) [directories addObject:node.remnant.path];
    }
    if (directories.count == 0) return checked;
    NSMutableArray<VCRemnantNode *> *effective = [NSMutableArray arrayWithCapacity:checked.count];
    for (VCRemnantNode *node in checked) {
        BOOL covered = NO;
        NSString *ancestor = node.remnant.path.stringByDeletingLastPathComponent;
        while (ancestor.length > 1) {
            if ([directories containsObject:ancestor]) {
                covered = YES;
                break;
            }
            ancestor = ancestor.stringByDeletingLastPathComponent;
        }
        if (!covered) [effective addObject:node];
    }
    return effective;
}

- (void)updateSummary {
    NSArray<VCRemnantNode *> *selection = [self effectiveSelection];
    uint64_t total = 0;
    BOOL complete = YES;
    uint64_t bucketBytes[VCStorageBucketCount] = {};
    NSUInteger bucketItems[VCStorageBucketCount] = {};
    for (VCRemnantNode *node in selection) {
        VCRemnant *remnant = node.remnant;
        if (remnant.usageKnown) {
            total += remnant.allocatedBytes;
            bucketBytes[remnant.bucket] += remnant.allocatedBytes;
        } else {
            complete = NO;
        }
        ++bucketItems[remnant.bucket];
    }

    BOOL bucketPresent[VCStorageBucketCount] = {};
    NSUInteger found = 0;
    NSUInteger checked = 0;
    for (VCRemnantGroup *group in _groups) {
        for (VCRemnantNode *node in group.nodes) {
            bucketPresent[node.remnant.bucket] = YES;
            ++found;
            if (node.checked && node.selectable) ++checked;
        }
    }

    // Every category that was found stays in the legend, even when nothing in it is selected.
    NSMutableArray<VCStorageSegment *> *segments = [NSMutableArray array];
    for (NSInteger bucket = 0; bucket < VCStorageBucketCount; ++bucket) {
        if (!bucketPresent[bucket]) continue;
        [segments addObject:[VCStorageSegment segmentWithTitle:VCStorageBucketTitle(static_cast<VCStorageBucket>(bucket))
                                                         bytes:bucketBytes[bucket]
                                                         color:VCColorForBucket(static_cast<VCStorageBucket>(bucket))
                                                     itemCount:bucketItems[bucket]]];
    }
    [_storageView setSegments:segments animated:YES];

    NSString *size = VCFormatBytes(total);
    _reclaimLabel.stringValue = found == 0 ? @"—" : (complete ? size : [size stringByAppendingString:@"+"]);
    _reclaimCaption.stringValue =
        found == 0 ? @"" : [NSString stringWithFormat:@"%lu of %@ selected", static_cast<unsigned long>(checked),
                                                      VCFormatCount(found, @"item", @"items")];
    if (_cleaning) return;
    if (_scanning && found == 0) {
        _statusLabel.stringValue = @"Scanning…";
    } else if (_scanning) {
        _statusLabel.stringValue = [NSString stringWithFormat:@"Measuring… %@", size];
    } else {
        _statusLabel.stringValue = [NSString stringWithFormat:@"%lu of %lu selected · %@",
                                                              static_cast<unsigned long>(checked),
                                                              static_cast<unsigned long>(found), size];
    }
    _statusLabel.toolTip = [NSString stringWithFormat:@"%lu of %@ selected · %@ to reclaim",
                                                      static_cast<unsigned long>(checked),
                                                      VCFormatCount(found, @"item", @"items"), size];
    [self updateControls];
}

- (void)updateControls {
    NSUInteger checked = 0;
    for (VCRemnantGroup *group in _groups) {
        for (VCRemnantNode *node in group.nodes) {
            if (node.checked && node.selectable) ++checked;
        }
    }
    const BOOL dryRun = _dryRunSwitch.state == NSControlStateValueOn;
    _cleanButton.enabled = _application != nil && checked > 0 && !_cleaning;
    _cleanButton.title = dryRun ? @"Simulate Clean" : @"Clean Selected";
    // Red for a real, destructive run; the neutral accent colour for a simulation.
    _cleanButton.bezelColor = dryRun ? nil : NSColor.systemRedColor;
    _modeControl.enabled = !_cleaning && !dryRun;
    _dryRunSwitch.enabled = !_cleaning;
    _outlineView.enabled = !_cleaning;
}

// ---------------------------------------------------------------------------
#pragma mark - Outline view

- (NSInteger)outlineView:(NSOutlineView *)outlineView numberOfChildrenOfItem:(id)item {
    (void)outlineView;
    if (item == nil) return static_cast<NSInteger>(_groups.count);
    if ([item isKindOfClass:[VCRemnantGroup class]]) {
        return static_cast<NSInteger>(static_cast<VCRemnantGroup *>(item).nodes.count);
    }
    return 0;
}

- (id)outlineView:(NSOutlineView *)outlineView child:(NSInteger)index ofItem:(id)item {
    (void)outlineView;
    if (item == nil) return _groups[static_cast<NSUInteger>(index)];
    return static_cast<VCRemnantGroup *>(item).nodes[static_cast<NSUInteger>(index)];
}

- (BOOL)outlineView:(NSOutlineView *)outlineView isItemExpandable:(id)item {
    (void)outlineView;
    return [item isKindOfClass:[VCRemnantGroup class]];
}

- (CGFloat)outlineView:(NSOutlineView *)outlineView heightOfRowByItem:(id)item {
    (void)outlineView;
    return [item isKindOfClass:[VCRemnantGroup class]] ? kGroupRowHeight : kItemRowHeight;
}

- (NSView *)outlineView:(NSOutlineView *)outlineView viewForTableColumn:(NSTableColumn *)column item:(id)item {
    const BOOL isGroup = [item isKindOfClass:[VCRemnantGroup class]];
    NSString *identifier = column.identifier;
    if ([identifier isEqualToString:kItemColumn]) {
        VCItemCellView *cell = [outlineView makeViewWithIdentifier:kItemColumn owner:self];
        if (cell == nil) {
            cell = [[VCItemCellView alloc] initWithFrame:NSZeroRect];
            cell.identifier = kItemColumn;
            cell.checkbox.target = self;
            cell.checkbox.action = @selector(checkboxClicked:);
        }
        if (isGroup) {
            [cell configureWithGroup:item];
        } else {
            [cell configureWithNode:item];
        }
        return cell;
    }
    if ([identifier isEqualToString:kMatchColumn]) {
        VCMatchCellView *cell = [outlineView makeViewWithIdentifier:kMatchColumn owner:self];
        if (cell == nil) {
            cell = [[VCMatchCellView alloc] initWithFrame:NSZeroRect];
            cell.identifier = kMatchColumn;
        }
        if (isGroup) {
            [cell configureWithGroup:item];
        } else {
            [cell configureWithNode:item];
        }
        return cell;
    }
    VCSizeCellView *cell = [outlineView makeViewWithIdentifier:kSizeColumn owner:self];
    if (cell == nil) {
        cell = [[VCSizeCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = kSizeColumn;
    }
    if (isGroup) {
        [cell configureWithGroup:item];
    } else {
        [cell configureWithNode:item];
    }
    return cell;
}

- (void)outlineViewSelectionDidChange:(NSNotification *)notification {
    (void)notification;
    if ([QLPreviewPanel sharedPreviewPanelExists] && [QLPreviewPanel sharedPreviewPanel].isVisible) {
        _previewURLs = [self urlsForNodes:[self actionNodes]];
        [[QLPreviewPanel sharedPreviewPanel] reloadData];
    }
}

/// A click on a check box: a group toggles all of its items, an item toggles itself.
- (void)checkboxClicked:(NSButton *)sender {
    const NSInteger row = [_outlineView rowForView:sender];
    if (row < 0) return;
    id item = [_outlineView itemAtRow:row];
    if ([item isKindOfClass:[VCRemnantGroup class]]) {
        VCRemnantGroup *group = item;
        // Mixed or off -> everything on; fully on -> everything off.
        const BOOL target = group.checkState != NSControlStateValueOn;
        for (VCRemnantNode *node in group.nodes) {
            if (node.selectable) node.checked = target;
        }
        [_outlineView reloadItem:group reloadChildren:YES];
    } else if ([item isKindOfClass:[VCRemnantNode class]]) {
        VCRemnantNode *node = item;
        if (!node.selectable) return;
        node.checked = sender.state == NSControlStateValueOn;
        VCRemnantGroup *group = node.group;
        if (group != nil) [_outlineView reloadItem:group reloadChildren:NO];
    }
    [self updateSummary];
}

- (IBAction)toggleSelectedRows:(id)sender {
    (void)sender;
    NSArray<VCRemnantNode *> *nodes = [self selectedNodes];
    if (nodes.count == 0 || _cleaning) return;
    BOOL allChecked = YES;
    for (VCRemnantNode *node in nodes) {
        if (node.selectable && !node.checked) allChecked = NO;
    }
    for (VCRemnantNode *node in nodes) {
        if (node.selectable) node.checked = !allChecked;
    }
    NSIndexSet *selection = _outlineView.selectedRowIndexes;
    [_outlineView reloadData];
    [_outlineView selectRowIndexes:selection byExtendingSelection:NO];
    [self updateSummary];
}

// ---------------------------------------------------------------------------
#pragma mark - Row actions (context menu, Quick Look)

- (NSArray<VCRemnantNode *> *)nodesForRows:(NSIndexSet *)rows {
    NSMutableArray<VCRemnantNode *> *nodes = [NSMutableArray array];
    [rows enumerateIndexesUsingBlock:^(NSUInteger row, BOOL *stop) {
        (void)stop;
        id item = [self->_outlineView itemAtRow:static_cast<NSInteger>(row)];
        if ([item isKindOfClass:[VCRemnantNode class]]) {
            [nodes addObject:item];
        } else if ([item isKindOfClass:[VCRemnantGroup class]]) {
            for (VCRemnantNode *node in static_cast<VCRemnantGroup *>(item).nodes) {
                if (![nodes containsObject:node]) [nodes addObject:node];
            }
        }
    }];
    return nodes;
}

- (NSArray<VCRemnantNode *> *)selectedNodes {
    return [self nodesForRows:_outlineView.selectedRowIndexes];
}

/// The rows an action applies to: the clicked row when it is outside the
/// selection (Finder's rule for contextual menus), otherwise the whole selection.
- (NSArray<VCRemnantNode *> *)actionNodes {
    const NSInteger clicked = _outlineView.clickedRow;
    if (clicked >= 0 && ![_outlineView.selectedRowIndexes containsIndex:static_cast<NSUInteger>(clicked)]) {
        return [self nodesForRows:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(clicked)]];
    }
    return [self selectedNodes];
}

- (NSArray<NSURL *> *)urlsForNodes:(NSArray<VCRemnantNode *> *)nodes {
    NSMutableArray<NSURL *> *urls = [NSMutableArray arrayWithCapacity:nodes.count];
    for (VCRemnantNode *node in nodes) {
        [urls addObject:[NSURL fileURLWithPath:node.remnant.path isDirectory:node.remnant.isDirectory]];
    }
    return urls;
}

- (void)menuNeedsUpdate:(NSMenu *)menu {
    const BOOL hasTarget = [self actionNodes].count > 0;
    for (NSMenuItem *item in menu.itemArray) item.enabled = hasTarget;
}

- (BOOL)validateMenuItem:(NSMenuItem *)menuItem {
    const SEL action = menuItem.action;
    if (action == @selector(revealSelectionInFinder:) || action == @selector(copySelectedPaths:) ||
        action == @selector(quickLookSelection:)) {
        return [self actionNodes].count > 0;
    }
    if (action == @selector(rescan:)) return _application != nil && !_cleaning;
    if (action == @selector(cleanSelected:)) return _cleanButton.enabled;
    return YES;
}

- (IBAction)revealSelectionInFinder:(id)sender {
    (void)sender;
    NSArray<NSURL *> *urls = [self urlsForNodes:[self actionNodes]];
    if (urls.count > 0) [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:urls];
}

- (IBAction)copySelectedPaths:(id)sender {
    (void)sender;
    NSArray<VCRemnantNode *> *nodes = [self actionNodes];
    if (nodes.count == 0) return;
    NSMutableArray<NSString *> *paths = [NSMutableArray arrayWithCapacity:nodes.count];
    for (VCRemnantNode *node in nodes) [paths addObject:node.remnant.path];
    NSPasteboard *pasteboard = NSPasteboard.generalPasteboard;
    [pasteboard clearContents];
    [pasteboard setString:[paths componentsJoinedByString:@"\n"] forType:NSPasteboardTypeString];
}

- (IBAction)quickLookSelection:(id)sender {
    (void)sender;
    QLPreviewPanel *panel = [QLPreviewPanel sharedPreviewPanel];
    if (panel.isVisible) {
        [panel orderOut:nil];
        return;
    }
    NSArray<NSURL *> *urls = [self urlsForNodes:[self actionNodes]];
    if (urls.count == 0) return;
    _previewURLs = urls;
    [self.view.window makeFirstResponder:_outlineView];
    [panel makeKeyAndOrderFront:nil];
    [panel reloadData];
}

// The Quick Look panel finds its controller by walking the responder chain.
- (BOOL)acceptsPreviewPanelControl:(QLPreviewPanel *)panel {
    (void)panel;
    return _application != nil;
}

- (void)beginPreviewPanelControl:(QLPreviewPanel *)panel {
    panel.dataSource = self;
    panel.delegate = self;
}

- (void)endPreviewPanelControl:(QLPreviewPanel *)panel {
    panel.dataSource = nil;
    panel.delegate = nil;
}

- (NSInteger)numberOfPreviewItemsInPreviewPanel:(QLPreviewPanel *)panel {
    (void)panel;
    return static_cast<NSInteger>(_previewURLs.count);
}

- (id<QLPreviewItem>)previewPanel:(QLPreviewPanel *)panel previewItemAtIndex:(NSInteger)index {
    (void)panel;
    return _previewURLs[static_cast<NSUInteger>(index)];
}

- (BOOL)previewPanel:(QLPreviewPanel *)panel handleEvent:(NSEvent *)event {
    (void)panel;
    // Arrow keys keep driving the list while the panel is open.
    if (event.type == NSEventTypeKeyDown) {
        [_outlineView keyDown:event];
        return YES;
    }
    return NO;
}

// ---------------------------------------------------------------------------
#pragma mark - Processes

- (void)workspaceApplicationsChanged:(NSNotification *)notification {
    (void)notification;
    if (_application != nil && _groups.count > 0) [self refreshProcesses];
}

- (void)refreshProcesses {
    VCApp *app = _application;
    if (app == nil) return;
    // Helper tools installed outside the bundle count as the app's processes too.
    NSMutableArray<NSString *> *helpers = [NSMutableArray array];
    for (VCRemnantGroup *group in _groups) {
        if (group.category != kCategoryPrivilegedHelperTools) continue;
        for (VCRemnantNode *node in group.nodes) [helpers addObject:node.remnant.path];
    }
    __weak DetailViewController *weakSelf = self;
    [VCEngine.sharedEngine findProcessesForApplication:app
                                     helperExecutables:helpers
                                            completion:^(NSArray<VCProcess *> *processes) {
                                                DetailViewController *controller = weakSelf;
                                                // Compare by path: the list republishes fresh VCApp
                                                // objects for the same bundle as sizes arrive.
                                                if (controller == nil ||
                                                    ![controller->_application.bundlePath isEqualToString:app.bundlePath]) {
                                                    return;
                                                }
                                                controller->_processes = processes;
                                                [controller updateRunningIndicator];
                                            }];
}

- (void)updateRunningIndicator {
    const NSUInteger count = _processes.count;
    _runningPill.hidden = count == 0;
    _quitButton.hidden = count == 0;
    if (count == 0) return;
    _runningPill.text = [NSString stringWithFormat:@"Running · %@", VCFormatCount(count, @"process", @"processes")];
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    for (VCProcess *process in _processes) {
        [lines addObject:[NSString stringWithFormat:@"%@ (pid %d)", process.name, process.pid]];
    }
    _runningPill.toolTip = [lines componentsJoinedByString:@"\n"];
}

- (void)quitProcesses:(id)sender {
    (void)sender;
    [self quitProcessesThen:nil];
}

/// Asks the processes to quit; escalates only after the user confirms. `continuation`
/// runs once nothing is left running.
- (void)quitProcessesThen:(void (^)(void))continuation {
    NSArray<VCProcess *> *processes = _processes;
    if (processes.count == 0) {
        if (continuation != nil) continuation();
        return;
    }
    _quitButton.enabled = NO;
    __weak DetailViewController *weakSelf = self;
    [VCEngine.sharedEngine quitProcesses:processes
                                   force:NO
                              completion:^(NSArray<VCProcess *> *stillRunning, NSArray<VCProcess *> *notPermitted) {
                                  DetailViewController *controller = weakSelf;
                                  if (controller == nil) return;
                                  controller->_quitButton.enabled = YES;
                                  [controller refreshProcesses];
                                  if (stillRunning.count == 0) {
                                      if (notPermitted.count > 0) [controller explainProtectedProcesses:notPermitted];
                                      if (continuation != nil) continuation();
                                      return;
                                  }
                                  [controller confirmForceQuit:stillRunning continuation:continuation];
                              }];
}

- (void)confirmForceQuit:(NSArray<VCProcess *> *)processes continuation:(void (^)(void))continuation {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = processes.count == 1
                            ? [NSString stringWithFormat:@"“%@” did not quit", processes.firstObject.name]
                            : [NSString stringWithFormat:@"%lu processes did not quit",
                                                         static_cast<unsigned long>(processes.count)];
    alert.informativeText = @"It may be waiting for you to save changes. Force quitting ends it immediately and "
                            @"discards anything that has not been saved.";
    [alert addButtonWithTitle:@"Force Quit"].hasDestructiveAction = YES;
    [alert addButtonWithTitle:@"Cancel"];
    __weak DetailViewController *weakSelf = self;
    [alert beginSheetModalForWindow:self.view.window
                  completionHandler:^(NSModalResponse response) {
                      DetailViewController *controller = weakSelf;
                      if (controller == nil || response != NSAlertFirstButtonReturn) return;
                      [VCEngine.sharedEngine quitProcesses:processes
                                                     force:YES
                                                completion:^(NSArray<VCProcess *> *stillRunning,
                                                             NSArray<VCProcess *> *notPermitted) {
                                                    (void)notPermitted;
                                                    [controller refreshProcesses];
                                                    if (stillRunning.count == 0 && continuation != nil) continuation();
                                                }];
                  }];
}

- (void)explainProtectedProcesses:(NSArray<VCProcess *> *)processes {
    _runningPill.toolTip = [NSString
        stringWithFormat:@"%@ run as another user and are stopped when their launch daemon is removed.",
                         VCFormatCount(processes.count, @"process", @"processes")];
}

// ---------------------------------------------------------------------------
#pragma mark - Full Disk Access banner

- (void)showBanner {
    if (!_banner.hidden) return;
    _banner.hidden = NO;
}

- (void)dismissBanner:(id)sender {
    (void)sender;
    gFullDiskAccessNoticeDismissed = YES;
    _banner.hidden = YES;
}

- (void)openFullDiskAccessSettings:(id)sender {
    (void)sender;
    NSURL *url = [NSURL URLWithString:kFullDiskAccessURL];
    if (url != nil) [[NSWorkspace sharedWorkspace] openURL:url];
}

// ---------------------------------------------------------------------------
#pragma mark - Cleaning

- (void)optionsChanged:(id)sender {
    (void)sender;
    [self updateControls];
}

- (IBAction)cleanSelected:(id)sender {
    (void)sender;
    if (_application == nil || _cleaning) return;
    NSArray<VCRemnantNode *> *selection = [self effectiveSelection];
    if (selection.count == 0) {
        NSBeep();
        return;
    }
    const BOOL dryRun = _dryRunSwitch.state == NSControlStateValueOn;
    const VCDeleteMode mode = static_cast<VCDeleteMode>(_modeControl.selectedSegment);

    // A simulation touches nothing, so it needs neither confirmation nor quitting the app.
    if (dryRun) {
        [self performCleanOfNodes:selection mode:mode dryRun:YES];
        return;
    }
    __weak DetailViewController *weakSelf = self;
    void (^confirm)(void) = ^{
        [weakSelf confirmCleanOfNodes:selection mode:mode];
    };

    BOOL removesBundle = NO;
    for (VCRemnantNode *node in selection) {
        if (node.remnant.isApplicationBundle) removesBundle = YES;
    }
    if (_processes.count == 0 || !removesBundle) {
        confirm();
        return;
    }
    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = [NSString stringWithFormat:@"“%@” is still running", _application.displayName];
    alert.informativeText = [NSString
        stringWithFormat:@"%@ must quit before the application can be removed.",
                         VCFormatCount(_processes.count, @"running process", @"running processes")];
    [alert addButtonWithTitle:@"Quit and Continue"];
    [alert addButtonWithTitle:@"Cancel"];
    [alert beginSheetModalForWindow:self.view.window
                  completionHandler:^(NSModalResponse response) {
                      if (response == NSAlertFirstButtonReturn) [weakSelf quitProcessesThen:confirm];
                  }];
}

- (void)confirmCleanOfNodes:(NSArray<VCRemnantNode *> *)nodes mode:(VCDeleteMode)mode {
    uint64_t bytes = 0;
    NSUInteger needsRoot = 0;
    NSUInteger shared = 0;
    for (VCRemnantNode *node in nodes) {
        bytes += node.remnant.allocatedBytes;
        if (node.remnant.requiresRoot) ++needsRoot;
        if (node.remnant.isShared) ++shared;
    }
    NSString *items = VCFormatCount(nodes.count, @"item", @"items");
    const BOOL permanent = mode == VCDeleteModePermanent;

    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = permanent ? NSAlertStyleCritical : NSAlertStyleWarning;
    alert.messageText = permanent ? [NSString stringWithFormat:@"Permanently delete %@?", items]
                                  : [NSString stringWithFormat:@"Move %@ to the Trash?", items];
    NSMutableString *text = [NSMutableString
        stringWithFormat:@"%@ belonging to “%@” will be %@.", VCFormatBytes(bytes), _application.displayName,
                         permanent ? @"erased immediately. This cannot be undone" : @"moved to the Trash, where they "
                                                                                    @"stay recoverable until you empty it"];
    if (needsRoot > 0) {
        [text appendFormat:@"\n\n%@ administrator privileges; you will be asked for your password.",
                           needsRoot == 1 ? @"1 item requires" : [NSString stringWithFormat:@"%lu items require",
                                                                                            static_cast<unsigned long>(needsRoot)]];
        if (!VCEngine.sharedEngine.elevationAvailable) {
            [text appendString:@" Elevation is not available on this system, so those items will be skipped and listed in the report."];
        }
    }
    if (shared > 0) {
        [text appendFormat:@"\n\n⚠︎ %@ shared with other installed software.",
                           shared == 1 ? @"1 selected item is" : [NSString stringWithFormat:@"%lu selected items are",
                                                                                           static_cast<unsigned long>(shared)]];
    }
    [text appendString:@"\n\nA JSON audit report of everything that was attempted is saved to ~/Library/Logs/VortexClean."];
    alert.informativeText = text;

    NSButton *confirmButton = [alert addButtonWithTitle:permanent ? @"Delete Permanently" : @"Move to Trash"];
    confirmButton.hasDestructiveAction = YES;
    [alert addButtonWithTitle:@"Cancel"];
    if (permanent) {
        // Permanent deletion is an explicit opt-in: the button stays disabled until the box is ticked.
        alert.showsSuppressionButton = YES;
        alert.suppressionButton.title = @"I understand that these items cannot be recovered";
        alert.suppressionButton.target = self;
        alert.suppressionButton.action = @selector(permanentAcknowledgementChanged:);
        confirmButton.enabled = NO;
        confirmButton.keyEquivalent = @"";  // Return must not trigger an irreversible action
        _permanentConfirmButton = confirmButton;
    }

    __weak DetailViewController *weakSelf = self;
    [alert beginSheetModalForWindow:self.view.window
                  completionHandler:^(NSModalResponse response) {
                      if (response != NSAlertFirstButtonReturn) return;
                      [weakSelf performCleanOfNodes:nodes mode:mode dryRun:NO];
                  }];
}

- (void)permanentAcknowledgementChanged:(NSButton *)sender {
    _permanentConfirmButton.enabled = sender.state == NSControlStateValueOn;
}

- (void)performCleanOfNodes:(NSArray<VCRemnantNode *> *)nodes mode:(VCDeleteMode)mode dryRun:(BOOL)dryRun {
    VCApp *app = _application;
    if (app == nil) return;
    NSMutableArray<VCRemnant *> *remnants = [NSMutableArray arrayWithCapacity:nodes.count];
    for (VCRemnantNode *node in nodes) [remnants addObject:node.remnant];

    _cleaning = YES;
    _progressBar.doubleValue = 0.0;
    _progressBar.hidden = NO;
    _statusLabel.stringValue = dryRun ? @"Simulating…" : (mode == VCDeleteModePermanent ? @"Deleting…" : @"Moving to Trash…");
    [self updateControls];

    __weak DetailViewController *weakSelf = self;
    [VCEngine.sharedEngine cleanRemnants:remnants
                           ofApplication:app
                                    mode:mode
                                  dryRun:dryRun
                                progress:^(NSUInteger completed, NSUInteger total) {
                                    DetailViewController *controller = weakSelf;
                                    if (controller == nil || total == 0) return;
                                    // The progress indicator animates between values on its own.
                                    controller->_progressBar.doubleValue =
                                        static_cast<double>(completed) / static_cast<double>(total);
                                    controller->_statusLabel.stringValue =
                                        [NSString stringWithFormat:@"%@ %lu of %lu…", dryRun ? @"Checked" : @"Processed",
                                                                   static_cast<unsigned long>(completed),
                                                                   static_cast<unsigned long>(total)];
                                }
                              completion:^(VCCleanReport *report) {
                                  [weakSelf cleanDidFinish:report application:app];
                              }];
}

- (void)cleanDidFinish:(VCCleanReport *)report application:(VCApp *)app {
    _cleaning = NO;
    _progressBar.hidden = YES;
    if (report.privacyDenied) {
        gFullDiskAccessNoticeDismissed = NO;  // something was actually blocked: say so again
        [self showBanner];
    }
    [self updateControls];
    [self updateSummary];

    BOOL bundleRemoved = NO;
    if (!report.dryRun) {
        for (VCCleanItemResult *item in report.items) {
            if ([item.path isEqualToString:app.bundlePath] &&
                (item.status == VCItemStatusTrashed || item.status == VCItemStatusDeleted)) {
                bundleRemoved = YES;
            }
        }
    }
    __weak DetailViewController *weakSelf = self;
    [self presentReport:report
            application:app
             completion:^{
                 DetailViewController *controller = weakSelf;
                 if (controller == nil || report.dryRun) return;
                 if (bundleRemoved) {
                     [VCIconCache.sharedCache removeIconForPath:app.bundlePath];
                     [controller.delegate detailViewController:controller didUninstallApplication:app];
                 } else if ([controller->_application.bundlePath isEqualToString:app.bundlePath]) {
                     [controller startScan];  // show what is left
                 }
             }];
}

- (void)presentReport:(VCCleanReport *)report application:(VCApp *)app completion:(void (^)(void))completion {
    const NSUInteger trashed = [report countWithStatus:VCItemStatusTrashed];
    const NSUInteger deleted = [report countWithStatus:VCItemStatusDeleted];
    const NSUInteger simulated = [report countWithStatus:VCItemStatusDryRun];
    const NSUInteger skippedSip = [report countWithStatus:VCItemStatusSkippedSip];
    const NSUInteger skippedPermission = [report countWithStatus:VCItemStatusSkippedPermission];
    const NSUInteger missing = [report countWithStatus:VCItemStatusSkippedMissing];
    const NSUInteger errors = [report countWithStatus:VCItemStatusError];
    const NSUInteger problems = skippedSip + skippedPermission + errors;

    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = problems > 0 ? NSAlertStyleWarning : NSAlertStyleInformational;
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    if (report.dryRun) {
        alert.messageText = [NSString stringWithFormat:@"Dry run finished for “%@”", app.displayName];
        [lines addObject:[NSString stringWithFormat:@"%@ would be removed, reclaiming %@. Nothing was changed.",
                                                    VCFormatCount(simulated, @"item", @"items"),
                                                    VCFormatBytes(report.reclaimedBytes)]];
    } else {
        alert.messageText = problems == 0 ? [NSString stringWithFormat:@"“%@” was cleaned", app.displayName]
                                          : [NSString stringWithFormat:@"“%@” was cleaned with exceptions", app.displayName];
        if (trashed > 0) {
            [lines addObject:[NSString stringWithFormat:@"%@ moved to the Trash.", VCFormatCount(trashed, @"item", @"items")]];
        }
        if (deleted > 0) {
            [lines addObject:[NSString stringWithFormat:@"%@ deleted permanently.", VCFormatCount(deleted, @"item", @"items")]];
        }
        [lines addObject:[NSString stringWithFormat:@"%@ reclaimed.", VCFormatBytes(report.reclaimedBytes)]];
    }
    if (skippedSip > 0) {
        [lines addObject:[NSString stringWithFormat:@"%@ protected by System Integrity Protection.",
                                                    VCFormatCount(skippedSip, @"item is", @"items are")]];
    }
    if (skippedPermission > 0) {
        [lines addObject:[NSString stringWithFormat:@"%@ skipped for lack of permission.",
                                                    VCFormatCount(skippedPermission, @"item was", @"items were")]];
    }
    if (missing > 0) {
        [lines addObject:[NSString stringWithFormat:@"%@ already gone.", VCFormatCount(missing, @"item was", @"items were")]];
    }
    if (errors > 0) {
        [lines addObject:[NSString stringWithFormat:@"%@ failed.", VCFormatCount(errors, @"item", @"items")]];
    }
    if (report.elevationAttempted && !report.elevationAuthorized && report.elevationError.length > 0) {
        [lines addObject:report.elevationError];
    }
    if (report.auditLogError.length > 0) {
        [lines addObject:[@"The audit report could not be written: " stringByAppendingString:report.auditLogError]];
    }
    alert.informativeText = [lines componentsJoinedByString:@"\n"];

    // List exactly what did not go through, with the reason for each.
    if (problems > 0) {
        NSMutableString *details = [NSMutableString string];
        for (VCCleanItemResult *item in report.items) {
            if (item.status != VCItemStatusSkippedSip && item.status != VCItemStatusSkippedPermission &&
                item.status != VCItemStatusError) {
                continue;
            }
            [details appendFormat:@"%@\n    %@ — %@\n", item.path, item.statusName,
                                  item.errorMessage.length > 0 ? item.errorMessage : @"no further details"];
        }
        NSScrollView *scroll = [NSTextView scrollableTextView];
        scroll.frame = NSMakeRect(0, 0, 440, 120);
        scroll.borderType = NSBezelBorder;
        NSTextView *textView = scroll.documentView;
        textView.editable = NO;
        textView.font = [NSFont monospacedSystemFontOfSize:10.5 weight:NSFontWeightRegular];
        textView.string = details;
        alert.accessoryView = scroll;
    }

    [alert addButtonWithTitle:@"Done"];
    if (report.auditLogPath.length > 0) [alert addButtonWithTitle:@"Show Audit Report"];
    [alert beginSheetModalForWindow:self.view.window
                  completionHandler:^(NSModalResponse response) {
                      if (response == NSAlertSecondButtonReturn && report.auditLogPath.length > 0) {
                          [[NSWorkspace sharedWorkspace]
                              activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:report.auditLogPath]]];
                      }
                      if (completion != nil) completion();
                  }];
}

// ---------------------------------------------------------------------------
#pragma mark - Miscellaneous actions

- (IBAction)revealAuditLogs:(id)sender {
    (void)sender;
    NSString *directory = VCEngine.sharedEngine.auditLogDirectory;
    NSURL *url = [NSURL fileURLWithPath:directory isDirectory:YES];
    // Showing a folder in Finder; when no report exists yet, show where it will appear.
    if (![[NSWorkspace sharedWorkspace] openURL:url]) {
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[url.URLByDeletingLastPathComponent]];
    }
}

- (void)chooseApplication:(id)sender {
    (void)sender;
    [self.delegate detailViewControllerDidRequestOpenPanel:self];
}

- (void)setDropHighlighted:(BOOL)highlighted {
    (void)self.view;
    _emptyView.highlighted = highlighted;
    // Over the empty state the drop zone itself lights up; over content an overlay appears.
    _dropOverlay.hidden = !(highlighted && _application != nil);
}

@end
