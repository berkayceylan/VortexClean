#import "SidebarViewController.h"

#import "VCFormat.h"

namespace {

constexpr CGFloat kRowHeight = 46.0;
constexpr CGFloat kIconSize = 32.0;
NSString *const kCellIdentifier = @"VCAppCell";

}  // namespace

// ---------------------------------------------------------------------------
#pragma mark - Row view

/// One application row: icon, name, "version · size", and a dot while it is running.
@interface VCAppCellView : NSTableCellView
@property (nonatomic, copy, nullable) NSString *representedPath;
- (void)configureWithApplication:(VCApp *)application running:(BOOL)running;
@end

@implementation VCAppCellView {
    NSImageView *_iconView;
    NSTextField *_nameLabel;
    NSTextField *_detailLabel;
    NSView *_runningDot;
    NSImageView *_lockView;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.identifier = kCellIdentifier;

        _iconView = [[NSImageView alloc] initWithFrame:NSZeroRect];
        _iconView.translatesAutoresizingMaskIntoConstraints = NO;
        _iconView.imageScaling = NSImageScaleProportionallyUpOrDown;
        [self addSubview:_iconView];

        _nameLabel = VCMakeLabel(@"", [NSFont systemFontOfSize:13.0], NSColor.labelColor);
        [self addSubview:_nameLabel];
        _detailLabel = VCMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:11.0 weight:NSFontWeightRegular],
                                   NSColor.secondaryLabelColor);
        [self addSubview:_detailLabel];

        _runningDot = [[NSView alloc] initWithFrame:NSZeroRect];
        _runningDot.translatesAutoresizingMaskIntoConstraints = NO;
        _runningDot.wantsLayer = YES;
        _runningDot.layer.cornerRadius = 3.5;
        _runningDot.hidden = YES;
        _runningDot.toolTip = @"Running";
        [_runningDot setAccessibilityLabel:@"Running"];
        [self addSubview:_runningDot];

        _lockView = [[NSImageView alloc] initWithFrame:NSZeroRect];
        _lockView.translatesAutoresizingMaskIntoConstraints = NO;
        _lockView.image = VCSymbol(@"lock.fill", 10.0, NSFontWeightMedium);
        _lockView.contentTintColor = NSColor.tertiaryLabelColor;
        _lockView.toolTip = @"Protected by System Integrity Protection";
        _lockView.hidden = YES;
        [self addSubview:_lockView];

        self.imageView = _iconView;
        self.textField = _nameLabel;

        [NSLayoutConstraint activateConstraints:@[
            [_iconView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:4.0],
            [_iconView.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [_iconView.widthAnchor constraintEqualToConstant:kIconSize],
            [_iconView.heightAnchor constraintEqualToConstant:kIconSize],

            [_nameLabel.leadingAnchor constraintEqualToAnchor:_iconView.trailingAnchor constant:9.0],
            [_nameLabel.topAnchor constraintEqualToAnchor:self.topAnchor constant:6.0],
            [_nameLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_lockView.leadingAnchor constant:-4.0],

            [_detailLabel.leadingAnchor constraintEqualToAnchor:_nameLabel.leadingAnchor],
            [_detailLabel.topAnchor constraintEqualToAnchor:_nameLabel.bottomAnchor constant:1.0],
            [_detailLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_runningDot.leadingAnchor constant:-6.0],

            [_lockView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-6.0],
            [_lockView.centerYAnchor constraintEqualToAnchor:_nameLabel.centerYAnchor],
            [_lockView.widthAnchor constraintEqualToConstant:11.0],

            [_runningDot.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-8.0],
            [_runningDot.centerYAnchor constraintEqualToAnchor:_detailLabel.centerYAnchor],
            [_runningDot.widthAnchor constraintEqualToConstant:7.0],
            [_runningDot.heightAnchor constraintEqualToConstant:7.0],
        ]];
        [self applyColors];
    }
    return self;
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    [self applyColors];
}

- (void)applyColors {
    [self.effectiveAppearance performAsCurrentDrawingAppearance:^{
        self->_runningDot.layer.backgroundColor = NSColor.systemGreenColor.CGColor;
    }];
}

- (void)configureWithApplication:(VCApp *)application running:(BOOL)running {
    self.representedPath = application.bundlePath;
    _nameLabel.stringValue = application.displayName;

    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    if (application.version.length > 0) [parts addObject:application.version];
    [parts addObject:application.usageKnown ? VCFormatBytes(application.allocatedBytes) : @"Measuring…"];
    _detailLabel.stringValue = [parts componentsJoinedByString:@" · "];

    _runningDot.hidden = !running;
    _lockView.hidden = !application.isSipProtected;
    self.toolTip = application.bundlePath;

    // Icons are read from disk on a background queue; show a generic one until then.
    VCIconCache *cache = VCIconCache.sharedCache;
    NSImage *cached = [cache cachedIconForPath:application.bundlePath];
    if (cached != nil) {
        _iconView.image = cached;
        return;
    }
    static NSImage *placeholder;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        placeholder = [NSImage imageNamed:NSImageNameApplicationIcon];
    });
    _iconView.image = placeholder;
    NSString *path = application.bundlePath;
    __weak VCAppCellView *weakSelf = self;
    [cache loadIconForPath:path
                completion:^(NSImage *icon) {
                    VCAppCellView *cell = weakSelf;
                    // The cell may have been reused for another row in the meantime.
                    if (cell != nil && [cell.representedPath isEqualToString:path]) cell->_iconView.image = icon;
                }];
}

@end

// ---------------------------------------------------------------------------
#pragma mark - Sidebar

/// The selected row of a snapshot (see -[SidebarViewController rendersOffscreen]).
@interface VCSnapshotRowView : NSTableRowView
@end

@implementation VCSnapshotRowView
- (void)drawBackgroundInRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (!self.selected) return;
    [[NSColor.labelColor colorWithAlphaComponent:0.10] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 10.0, 0.0) xRadius:8.0 yRadius:8.0] fill];
}
@end

@interface SidebarViewController () <NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate>
@end

@implementation SidebarViewController {
    NSSearchField *_searchField;
    NSSegmentedControl *_sortControl;
    NSTableView *_tableView;
    NSScrollView *_scrollView;
    NSTextField *_footerLabel;
    NSProgressIndicator *_spinner;

    NSArray<VCApp *> *_allApplications;
    NSArray<VCApp *> *_visibleApplications;
    NSSet<NSString *> *_runningBundlePaths;
    NSString *_selectedBundlePath;
    BOOL _suppressSelectionCallback;
}

- (void)loadView {
    // The translucent, behind-window sidebar material.
    NSVisualEffectView *background = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, 280, 600)];
    background.material = NSVisualEffectMaterialSidebar;
    background.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    background.state = NSVisualEffectStateFollowsWindowActiveState;
    self.view = background;

    _allApplications = @[];
    _visibleApplications = @[];
    _runningBundlePaths = [NSSet set];

    _searchField = [[NSSearchField alloc] initWithFrame:NSZeroRect];
    _searchField.translatesAutoresizingMaskIntoConstraints = NO;
    _searchField.placeholderString = @"Search Applications";
    _searchField.delegate = self;
    _searchField.sendsSearchStringImmediately = YES;
    _searchField.sendsWholeSearchString = NO;
    _searchField.target = self;
    _searchField.action = @selector(searchChanged:);
    [_searchField setAccessibilityLabel:@"Search applications"];
    [background addSubview:_searchField];

    _sortControl = [NSSegmentedControl segmentedControlWithLabels:@[@"Size", @"Name", @"Last Used"]
                                                    trackingMode:NSSegmentSwitchTrackingSelectOne
                                                          target:self
                                                          action:@selector(sortChanged:)];
    _sortControl.translatesAutoresizingMaskIntoConstraints = NO;
    _sortControl.controlSize = NSControlSizeSmall;
    _sortControl.segmentDistribution = NSSegmentDistributionFillEqually;
    _sortControl.selectedSegment = VCSidebarSortSize;
    [_sortControl setAccessibilityLabel:@"Sort applications by"];
    [background addSubview:_sortControl];

    _tableView = [[NSTableView alloc] initWithFrame:NSZeroRect];
    _tableView.style = NSTableViewStyleSourceList;
    _tableView.headerView = nil;
    _tableView.rowHeight = kRowHeight;  // fixed height keeps the table fully virtualised
    _tableView.usesAutomaticRowHeights = NO;
    _tableView.intercellSpacing = NSMakeSize(0.0, 0.0);
    _tableView.allowsEmptySelection = YES;
    _tableView.allowsMultipleSelection = NO;
    _tableView.backgroundColor = NSColor.clearColor;
    _tableView.dataSource = self;
    _tableView.delegate = self;
    [_tableView setAccessibilityLabel:@"Installed applications"];
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"app"];
    column.resizingMask = NSTableColumnAutoresizingMask;
    [_tableView addTableColumn:column];
    _tableView.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;

    _scrollView = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.documentView = _tableView;
    _scrollView.hasVerticalScroller = YES;
    _scrollView.autohidesScrollers = YES;
    _scrollView.drawsBackground = NO;
    _scrollView.borderType = NSNoBorder;
    [background addSubview:_scrollView];

    _footerLabel = VCMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:11.0 weight:NSFontWeightRegular],
                               NSColor.secondaryLabelColor);
    _footerLabel.alignment = NSTextAlignmentCenter;
    [background addSubview:_footerLabel];

    _spinner = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    _spinner.translatesAutoresizingMaskIntoConstraints = NO;
    _spinner.style = NSProgressIndicatorStyleSpinning;
    _spinner.controlSize = NSControlSizeSmall;
    _spinner.displayedWhenStopped = NO;
    [background addSubview:_spinner];

    NSLayoutGuide *safeArea = background.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_searchField.topAnchor constraintEqualToAnchor:safeArea.topAnchor constant:6.0],
        [_searchField.leadingAnchor constraintEqualToAnchor:background.leadingAnchor constant:12.0],
        [_searchField.trailingAnchor constraintEqualToAnchor:background.trailingAnchor constant:-12.0],

        [_sortControl.topAnchor constraintEqualToAnchor:_searchField.bottomAnchor constant:8.0],
        [_sortControl.leadingAnchor constraintEqualToAnchor:_searchField.leadingAnchor],
        [_sortControl.trailingAnchor constraintEqualToAnchor:_searchField.trailingAnchor],

        [_scrollView.topAnchor constraintEqualToAnchor:_sortControl.bottomAnchor constant:8.0],
        [_scrollView.leadingAnchor constraintEqualToAnchor:background.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:background.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:_footerLabel.topAnchor constant:-6.0],

        [_footerLabel.leadingAnchor constraintEqualToAnchor:background.leadingAnchor constant:12.0],
        [_footerLabel.trailingAnchor constraintEqualToAnchor:background.trailingAnchor constant:-12.0],
        [_footerLabel.bottomAnchor constraintEqualToAnchor:background.bottomAnchor constant:-8.0],

        [_spinner.centerYAnchor constraintEqualToAnchor:_footerLabel.centerYAnchor],
        [_spinner.trailingAnchor constraintEqualToAnchor:background.trailingAnchor constant:-12.0],
    ]];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    // Running state is event driven: NSWorkspace tells us when apps launch and quit.
    NSNotificationCenter *center = NSWorkspace.sharedWorkspace.notificationCenter;
    [center addObserver:self
               selector:@selector(runningApplicationsChanged:)
                   name:NSWorkspaceDidLaunchApplicationNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(runningApplicationsChanged:)
                   name:NSWorkspaceDidTerminateApplicationNotification
                 object:nil];
    [self refreshRunningSet];
    [self updateFooter];
}

- (void)dealloc {
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
}

// ---------------------------------------------------------------------------
#pragma mark Public

- (VCApp *)selectedApplication {
    if (_selectedBundlePath == nil) return nil;
    for (VCApp *application in _allApplications) {
        if ([application.bundlePath isEqualToString:_selectedBundlePath]) return application;
    }
    return nil;
}

- (NSUInteger)applicationCount {
    return _allApplications.count;
}

- (void)setApplications:(NSArray<VCApp *> *)applications {
    _allApplications = [applications copy];
    if (_selectedBundlePath != nil && self.selectedApplication == nil) _selectedBundlePath = nil;
    [self rebuildVisibleList];
}

- (void)selectApplicationWithBundlePath:(NSString *)bundlePath {
    _selectedBundlePath = [bundlePath copy];
    // Make sure an active filter does not hide what was just opened.
    BOOL visible = NO;
    for (VCApp *application in _visibleApplications) {
        if ([application.bundlePath isEqualToString:bundlePath]) visible = YES;
    }
    if (!visible && _searchField.stringValue.length > 0) {
        _searchField.stringValue = @"";
        [self rebuildVisibleList];
    } else {
        [self restoreSelection];
    }
    const NSInteger row = _tableView.selectedRow;
    if (row >= 0) [_tableView scrollRowToVisible:row];
}

- (void)clearSelection {
    _selectedBundlePath = nil;
    _suppressSelectionCallback = YES;
    [_tableView deselectAll:nil];
    _suppressSelectionCallback = NO;
}

- (void)setLoading:(BOOL)loading {
    if (loading) {
        [_spinner startAnimation:nil];
    } else {
        [_spinner stopAnimation:nil];
    }
}

- (void)focusSearchField {
    [self.view.window makeFirstResponder:_searchField];
}

// ---------------------------------------------------------------------------
#pragma mark List maintenance

- (void)rebuildVisibleList {
    NSString *query = [_searchField.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSMutableArray<VCApp *> *filtered = [NSMutableArray arrayWithCapacity:_allApplications.count];
    for (VCApp *application in _allApplications) {
        if (query.length == 0 ||
            [application.displayName rangeOfString:query
                                           options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch]
                    .location != NSNotFound ||
            [application.bundleIdentifier rangeOfString:query options:NSCaseInsensitiveSearch].location != NSNotFound) {
            [filtered addObject:application];
        }
    }

    const VCSidebarSort sort = static_cast<VCSidebarSort>(_sortControl.selectedSegment);
    [filtered sortUsingComparator:^NSComparisonResult(VCApp *a, VCApp *b) {
        NSComparisonResult result = NSOrderedSame;
        if (sort == VCSidebarSortSize) {
            // Largest first; bundles that are still being measured go last.
            if (a.usageKnown != b.usageKnown) return a.usageKnown ? NSOrderedAscending : NSOrderedDescending;
            if (a.allocatedBytes != b.allocatedBytes) {
                result = a.allocatedBytes > b.allocatedBytes ? NSOrderedAscending : NSOrderedDescending;
            }
        } else if (sort == VCSidebarSortLastUsed) {
            // Most recently used first; never-used applications go last.
            NSDate *dateA = a.lastUsedDate;
            NSDate *dateB = b.lastUsedDate;
            if ((dateA == nil) != (dateB == nil)) return dateA != nil ? NSOrderedAscending : NSOrderedDescending;
            if (dateA != nil && dateB != nil) result = [dateB compare:dateA];
        }
        if (result != NSOrderedSame) return result;
        result = [a.displayName localizedStandardCompare:b.displayName];
        return result != NSOrderedSame ? result : [a.bundlePath compare:b.bundlePath];
    }];

    _visibleApplications = [filtered copy];
    _suppressSelectionCallback = YES;
    [_tableView reloadData];
    _suppressSelectionCallback = NO;
    [self restoreSelection];
    [self updateFooter];
}

- (void)restoreSelection {
    NSInteger row = -1;
    if (_selectedBundlePath != nil) {
        for (NSUInteger i = 0; i < _visibleApplications.count; ++i) {
            if ([_visibleApplications[i].bundlePath isEqualToString:_selectedBundlePath]) {
                row = static_cast<NSInteger>(i);
                break;
            }
        }
    }
    _suppressSelectionCallback = YES;
    if (row >= 0) {
        [_tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];
    } else {
        [_tableView deselectAll:nil];
    }
    _suppressSelectionCallback = NO;
}

- (void)updateFooter {
    uint64_t total = 0;
    BOOL complete = YES;
    for (VCApp *application in _allApplications) {
        if (application.usageKnown) {
            total += application.allocatedBytes;
        } else {
            complete = NO;
        }
    }
    NSString *count = VCFormatCount(_allApplications.count, @"application", @"applications");
    if (_visibleApplications.count != _allApplications.count) {
        count = [NSString stringWithFormat:@"%lu of %@", static_cast<unsigned long>(_visibleApplications.count), count];
    }
    _footerLabel.stringValue = _allApplications.count == 0
                                   ? @"Looking for applications…"
                                   : [NSString stringWithFormat:@"%@ · %@%@", count, VCFormatBytes(total),
                                                                complete ? @"" : @"+"];
}

// ---------------------------------------------------------------------------
#pragma mark Running state

- (void)refreshRunningSet {
    NSMutableSet<NSString *> *paths = [NSMutableSet set];
    for (NSRunningApplication *running in NSWorkspace.sharedWorkspace.runningApplications) {
        NSString *path = running.bundleURL.path;
        if (path.length > 0) [paths addObject:path];
    }
    _runningBundlePaths = [paths copy];
}

- (void)runningApplicationsChanged:(NSNotification *)notification {
    (void)notification;
    [self refreshRunningSet];
    // Only rows that are on screen need to be touched.
    const NSRange visible = [_tableView rowsInRect:_tableView.visibleRect];
    if (visible.length == 0) return;
    [_tableView reloadDataForRowIndexes:[NSIndexSet indexSetWithIndexesInRange:visible]
                          columnIndexes:[NSIndexSet indexSetWithIndex:0]];
}

// ---------------------------------------------------------------------------
#pragma mark Actions

- (void)searchChanged:(id)sender {
    (void)sender;
    [self rebuildVisibleList];
}

- (void)controlTextDidChange:(NSNotification *)notification {
    if (notification.object == _searchField) [self rebuildVisibleList];
}

- (void)sortChanged:(id)sender {
    (void)sender;
    [self rebuildVisibleList];
    const NSInteger row = _tableView.selectedRow;
    if (row >= 0) [_tableView scrollRowToVisible:row];
}

// ---------------------------------------------------------------------------
#pragma mark Table view

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
    (void)tableView;
    return static_cast<NSInteger>(_visibleApplications.count);
}

- (void)setRendersOffscreen:(BOOL)rendersOffscreen {
    _rendersOffscreen = rendersOffscreen;
    (void)self.view;  // the table exists once the view is loaded
    _tableView.selectionHighlightStyle =
        rendersOffscreen ? NSTableViewSelectionHighlightStyleNone : NSTableViewSelectionHighlightStyleRegular;
}

- (NSTableRowView *)tableView:(NSTableView *)tableView rowViewForRow:(NSInteger)row {
    (void)tableView;
    (void)row;
    return self.rendersOffscreen ? [[VCSnapshotRowView alloc] initWithFrame:NSZeroRect] : nil;
}

- (NSView *)tableView:(NSTableView *)tableView viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)column;
    VCAppCellView *cell = [tableView makeViewWithIdentifier:kCellIdentifier owner:self];
    if (cell == nil) cell = [[VCAppCellView alloc] initWithFrame:NSMakeRect(0, 0, 240, kRowHeight)];
    VCApp *application = _visibleApplications[static_cast<NSUInteger>(row)];
    [cell configureWithApplication:application running:[_runningBundlePaths containsObject:application.bundlePath]];
    return cell;
}

- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    (void)notification;
    if (_suppressSelectionCallback) return;
    const NSInteger row = _tableView.selectedRow;
    VCApp *application = row >= 0 && row < static_cast<NSInteger>(_visibleApplications.count)
                             ? _visibleApplications[static_cast<NSUInteger>(row)]
                             : nil;
    _selectedBundlePath = [application.bundlePath copy];
    [self.delegate sidebar:self didSelectApplication:application];
}

@end
