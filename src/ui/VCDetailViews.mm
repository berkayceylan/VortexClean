#import "VCDetailViews.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "VCFormat.h"

// ---------------------------------------------------------------------------
#pragma mark - Tree model

@implementation VCRemnantNode

- (BOOL)selectable {
    return !self.remnant.sipProtected;
}

@end

@implementation VCRemnantGroup

- (instancetype)init {
    if ((self = [super init])) {
        _nodes = [NSMutableArray array];
        _title = @"";
    }
    return self;
}

- (NSControlStateValue)checkState {
    NSUInteger selectable = 0;
    NSUInteger checked = 0;
    for (VCRemnantNode *node in _nodes) {
        if (!node.selectable) continue;
        ++selectable;
        if (node.checked) ++checked;
    }
    if (checked == 0) return NSControlStateValueOff;
    return checked == selectable ? NSControlStateValueOn : NSControlStateValueMixed;
}

- (NSUInteger)selectableCount {
    NSUInteger count = 0;
    for (VCRemnantNode *node in _nodes) {
        if (node.selectable) ++count;
    }
    return count;
}

- (uint64_t)measuredBytes {
    // A group can list a folder together with something inside it (a shared
    // vendor folder and the app's own sub-folder): count those bytes once.
    NSMutableSet<NSString *> *directories = [NSMutableSet set];
    for (VCRemnantNode *node in _nodes) {
        if (node.remnant.isDirectory) [directories addObject:node.remnant.path];
    }
    uint64_t total = 0;
    for (VCRemnantNode *node in _nodes) {
        if (!node.remnant.usageKnown) continue;
        BOOL covered = NO;
        if (directories.count > 1 || (directories.count == 1 && !node.remnant.isDirectory)) {
            NSString *ancestor = node.remnant.path.stringByDeletingLastPathComponent;
            while (ancestor.length > 1) {
                if ([directories containsObject:ancestor]) {
                    covered = YES;
                    break;
                }
                ancestor = ancestor.stringByDeletingLastPathComponent;
            }
        }
        if (!covered) total += node.remnant.allocatedBytes;
    }
    return total;
}

- (BOOL)fullyMeasured {
    for (VCRemnantNode *node in _nodes) {
        if (!node.remnant.usageKnown) return NO;
    }
    return YES;
}

@end

// ---------------------------------------------------------------------------
#pragma mark - Outline view

@implementation VCOutlineView

- (void)keyDown:(NSEvent *)event {
    NSString *characters = event.charactersIgnoringModifiers;
    const NSEventModifierFlags modifiers = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if (characters.length == 1 && (modifiers & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) == 0) {
        const unichar key = [characters characterAtIndex:0];
        if (key == ' ') {
            // Routed through the responder chain to the detail view controller.
            if ([NSApp sendAction:NSSelectorFromString(@"quickLookSelection:") to:nil from:self]) return;
        } else if (key == NSCarriageReturnCharacter || key == NSEnterCharacter) {
            if ([NSApp sendAction:NSSelectorFromString(@"toggleSelectedRows:") to:nil from:self]) return;
        }
    }
    [super keyDown:event];
}

/// Right-clicking a row that is not part of the selection acts on that row alone,
/// which is how Finder behaves; AppKit draws the contextual ring for it.
- (NSMenu *)menuForEvent:(NSEvent *)event {
    return [super menuForEvent:event];
}

@end

// ---------------------------------------------------------------------------
#pragma mark - Icons for rows (no disk access)

/// A type-based icon. Unlike -iconForFile: this never reads the item itself, so
/// it is safe on the main thread even for thousands of rows.
static NSImage *VCIconForRemnant(VCRemnant *remnant) {
    static NSCache<NSString *, NSImage *> *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        cache = [[NSCache alloc] init];
    });
    NSString *extension = remnant.path.pathExtension.lowercaseString;
    NSString *key = remnant.isDirectory ? [@"dir." stringByAppendingString:extension] : extension;
    NSImage *icon = [cache objectForKey:key];
    if (icon != nil) return icon;

    UTType *type = nil;
    if (extension.length > 0) {
        type = [UTType typeWithFilenameExtension:extension
                                conformingToType:remnant.isDirectory ? UTTypeDirectory : UTTypeData];
        // A folder whose extension is not a registered package type is still just a folder.
        if (type != nil && remnant.isDirectory && type.isDynamic) type = nil;
    }
    if (type == nil) type = remnant.isDirectory ? UTTypeFolder : UTTypeData;
    icon = [[NSWorkspace sharedWorkspace] iconForContentType:type];
    [cache setObject:icon forKey:key];
    return icon;
}

// ---------------------------------------------------------------------------
#pragma mark - Item cell

@implementation VCItemCellView {
    NSImageView *_iconView;
    NSTextField *_titleLabel;
    NSImageView *_badgeView;
    NSLayoutConstraint *_titleLeadingToIcon;
    NSLayoutConstraint *_titleLeadingToCheckbox;
    NSString *_representedPath;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _checkbox = [NSButton checkboxWithTitle:@"" target:nil action:nil];
        _checkbox.translatesAutoresizingMaskIntoConstraints = NO;
        _checkbox.controlSize = NSControlSizeSmall;
        [self addSubview:_checkbox];

        _iconView = [[NSImageView alloc] initWithFrame:NSZeroRect];
        _iconView.translatesAutoresizingMaskIntoConstraints = NO;
        _iconView.imageScaling = NSImageScaleProportionallyUpOrDown;
        [self addSubview:_iconView];

        _titleLabel = VCMakeLabel(@"", [NSFont systemFontOfSize:12.0], NSColor.labelColor);
        _titleLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
        [self addSubview:_titleLabel];

        _badgeView = [[NSImageView alloc] initWithFrame:NSZeroRect];
        _badgeView.translatesAutoresizingMaskIntoConstraints = NO;
        _badgeView.imageScaling = NSImageScaleProportionallyDown;
        _badgeView.hidden = YES;
        [self addSubview:_badgeView];

        self.textField = _titleLabel;
        self.imageView = _iconView;

        _titleLeadingToIcon = [_titleLabel.leadingAnchor constraintEqualToAnchor:_iconView.trailingAnchor constant:6.0];
        _titleLeadingToCheckbox =
            [_titleLabel.leadingAnchor constraintEqualToAnchor:_checkbox.trailingAnchor constant:4.0];
        [NSLayoutConstraint activateConstraints:@[
            [_checkbox.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2.0],
            [_checkbox.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],

            [_iconView.leadingAnchor constraintEqualToAnchor:_checkbox.trailingAnchor constant:4.0],
            [_iconView.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [_iconView.widthAnchor constraintEqualToConstant:16.0],
            [_iconView.heightAnchor constraintEqualToConstant:16.0],

            _titleLeadingToIcon,
            [_titleLabel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [_titleLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_badgeView.leadingAnchor constant:-6.0],

            [_badgeView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-4.0],
            [_badgeView.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [_badgeView.widthAnchor constraintEqualToConstant:14.0],
            [_badgeView.heightAnchor constraintEqualToConstant:14.0],
        ]];
    }
    return self;
}

- (void)configureWithGroup:(VCRemnantGroup *)group {
    _representedPath = nil;
    _checkbox.allowsMixedState = YES;
    _checkbox.state = group.checkState;
    _checkbox.enabled = group.selectableCount > 0;
    [_checkbox setAccessibilityLabel:[NSString stringWithFormat:@"Select all in %@", group.title]];

    _iconView.hidden = YES;
    _titleLeadingToIcon.active = NO;
    _titleLeadingToCheckbox.active = YES;
    _titleLabel.stringValue = group.title;
    _titleLabel.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightSemibold];
    _titleLabel.textColor = NSColor.labelColor;
    _badgeView.hidden = YES;
    self.toolTip = nil;
}

- (void)configureWithNode:(VCRemnantNode *)node {
    VCRemnant *remnant = node.remnant;
    _representedPath = remnant.path;
    _checkbox.allowsMixedState = NO;
    _checkbox.state = node.checked ? NSControlStateValueOn : NSControlStateValueOff;
    _checkbox.enabled = node.selectable;
    [_checkbox setAccessibilityLabel:[NSString stringWithFormat:@"Select %@", remnant.displayPath]];

    _iconView.hidden = NO;
    _titleLeadingToCheckbox.active = NO;
    _titleLeadingToIcon.active = YES;
    _titleLabel.stringValue = remnant.displayPath;
    _titleLabel.font = [NSFont systemFontOfSize:12.0];
    _titleLabel.textColor = node.selectable ? NSColor.labelColor : NSColor.secondaryLabelColor;

    // Application bundles get their real icon (loaded off the main thread);
    // everything else gets an icon derived from its type alone.
    if (remnant.isApplicationBundle) {
        VCIconCache *cache = VCIconCache.sharedCache;
        NSImage *cached = [cache cachedIconForPath:remnant.path];
        _iconView.image = cached != nil ? cached : VCIconForRemnant(remnant);
        if (cached == nil) {
            NSString *path = remnant.path;
            __weak VCItemCellView *weakSelf = self;
            [cache loadIconForPath:path
                        completion:^(NSImage *icon) {
                            VCItemCellView *cell = weakSelf;
                            if (cell != nil && [cell->_representedPath isEqualToString:path]) cell->_iconView.image = icon;
                        }];
        }
    } else {
        _iconView.image = VCIconForRemnant(remnant);
    }

    // Trailing badge: locked by SIP, needs an administrator, or a caution.
    NSString *tooltip = remnant.path;
    if (remnant.sipProtected) {
        _badgeView.image = VCSymbol(@"lock.fill", 11.0, NSFontWeightMedium);
        _badgeView.contentTintColor = NSColor.secondaryLabelColor;
        _badgeView.toolTip = @"Protected by System Integrity Protection — cannot be removed";
        _badgeView.hidden = NO;
    } else if (remnant.requiresRoot) {
        _badgeView.image = VCSymbol(@"lock.shield", 11.0, NSFontWeightMedium);
        _badgeView.contentTintColor = NSColor.systemOrangeColor;
        _badgeView.toolTip = @"Requires administrator privileges to remove";
        _badgeView.hidden = NO;
    } else if (remnant.permissionDenied) {
        _badgeView.image = VCSymbol(@"eye.slash", 11.0, NSFontWeightMedium);
        _badgeView.contentTintColor = NSColor.systemYellowColor;
        _badgeView.toolTip = @"Could not be read completely — Full Disk Access may be required";
        _badgeView.hidden = NO;
    } else {
        _badgeView.hidden = YES;
    }
    if (remnant.warning.length > 0) tooltip = [NSString stringWithFormat:@"%@\n%@", remnant.path, remnant.warning];
    self.toolTip = tooltip;
}

@end

// ---------------------------------------------------------------------------
#pragma mark - Match cell

@implementation VCMatchCellView {
    VCPillView *_pill;
    VCPillView *_sharedPill;
    NSTextField *_countLabel;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _pill = [[VCPillView alloc] initWithFrame:NSZeroRect];
        [self addSubview:_pill];
        _sharedPill = [[VCPillView alloc] initWithFrame:NSZeroRect];
        _sharedPill.text = @"Shared";
        _sharedPill.tintColor = NSColor.systemYellowColor;
        _sharedPill.image = VCSymbol(@"exclamationmark.triangle.fill", 9.0, NSFontWeightSemibold);
        [self addSubview:_sharedPill];
        _countLabel = VCMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:11.0 weight:NSFontWeightRegular],
                                  NSColor.secondaryLabelColor);
        [self addSubview:_countLabel];

        [NSLayoutConstraint activateConstraints:@[
            [_sharedPill.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2.0],
            [_sharedPill.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [_pill.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [_pill.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-2.0],
            [_countLabel.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2.0],
            [_countLabel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [_countLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-2.0],
        ]];
        // The confidence pill sits after the "Shared" badge when there is one.
        NSLayoutConstraint *afterShared = [_pill.leadingAnchor constraintEqualToAnchor:_sharedPill.trailingAnchor
                                                                               constant:4.0];
        afterShared.priority = NSLayoutPriorityDefaultHigh;
        afterShared.identifier = @"afterShared";
        NSLayoutConstraint *atEdge = [_pill.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2.0];
        atEdge.priority = NSLayoutPriorityDefaultLow;
        atEdge.identifier = @"atEdge";
        [NSLayoutConstraint activateConstraints:@[afterShared, atEdge]];
    }
    return self;
}

- (void)setSharedVisible:(BOOL)visible {
    _sharedPill.hidden = !visible;
    for (NSLayoutConstraint *constraint in self.constraints) {
        if ([constraint.identifier isEqualToString:@"afterShared"]) {
            constraint.priority = visible ? NSLayoutPriorityDefaultHigh : NSLayoutPriorityDefaultLow - 1;
        } else if ([constraint.identifier isEqualToString:@"atEdge"]) {
            constraint.priority = visible ? NSLayoutPriorityDefaultLow : NSLayoutPriorityDefaultHigh;
        }
    }
}

- (void)configureWithGroup:(VCRemnantGroup *)group {
    _pill.hidden = YES;
    [self setSharedVisible:NO];
    _countLabel.hidden = NO;
    _countLabel.stringValue = VCFormatCount(group.nodes.count, @"item", @"items");
    self.toolTip = nil;
}

- (void)configureWithNode:(VCRemnantNode *)node {
    VCRemnant *remnant = node.remnant;
    _countLabel.hidden = YES;
    _pill.hidden = NO;
    _pill.text = [NSString stringWithFormat:@"%@ · %@", remnant.confidenceName, remnant.reason];
    _pill.tintColor = VCColorForConfidence(remnant.confidence);
    [self setSharedVisible:remnant.isShared];

    NSMutableString *tooltip = [NSMutableString
        stringWithFormat:@"%@ confidence — matched by: %@", remnant.confidenceName, remnant.reason];
    if (remnant.isShared && remnant.sharedWith.count > 0) {
        [tooltip appendFormat:@"\nShared with: %@", [remnant.sharedWith componentsJoinedByString:@", "]];
    }
    if (remnant.warning.length > 0) [tooltip appendFormat:@"\n%@", remnant.warning];
    self.toolTip = tooltip;
    [self setAccessibilityLabel:tooltip];
}

@end

// ---------------------------------------------------------------------------
#pragma mark - Size cell

@implementation VCSizeCellView {
    NSTextField *_label;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _label = VCMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:12.0 weight:NSFontWeightRegular],
                             NSColor.labelColor);
        _label.alignment = NSTextAlignmentRight;
        [self addSubview:_label];
        self.textField = _label;
        [NSLayoutConstraint activateConstraints:@[
            [_label.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2.0],
            [_label.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-6.0],
            [_label.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        ]];
    }
    return self;
}

- (void)configureWithGroup:(VCRemnantGroup *)group {
    _label.font = [NSFont monospacedDigitSystemFontOfSize:12.0 weight:NSFontWeightSemibold];
    _label.textColor = NSColor.labelColor;
    NSString *size = VCFormatBytes(group.measuredBytes);
    _label.stringValue = group.fullyMeasured ? size : [size stringByAppendingString:@"+"];
}

- (void)configureWithNode:(VCRemnantNode *)node {
    _label.font = [NSFont monospacedDigitSystemFontOfSize:12.0 weight:NSFontWeightRegular];
    if (node.remnant.usageKnown) {
        _label.stringValue = VCFormatBytes(node.remnant.allocatedBytes);
        _label.textColor = NSColor.labelColor;
        self.toolTip = [NSString stringWithFormat:@"%@ on disk · %@ of data", VCFormatBytes(node.remnant.allocatedBytes),
                                                  VCFormatBytes(node.remnant.logicalBytes)];
    } else {
        _label.stringValue = @"—";
        _label.textColor = NSColor.tertiaryLabelColor;
        self.toolTip = @"Measuring…";
    }
}

@end

// ---------------------------------------------------------------------------
#pragma mark - Drop zone (empty state)

@implementation VCDropZoneView {
    NSImageView *_imageView;
    NSTextField *_titleLabel;
    NSTextField *_subtitleLabel;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.translatesAutoresizingMaskIntoConstraints = NO;

        _imageView = [[NSImageView alloc] initWithFrame:NSZeroRect];
        _imageView.translatesAutoresizingMaskIntoConstraints = NO;
        NSImage *glyph = VCSymbol(@"arrow.down.app", 52.0, NSFontWeightLight);
        if (glyph == nil) glyph = VCSymbol(@"square.and.arrow.down", 52.0, NSFontWeightLight);
        _imageView.image = glyph;
        _imageView.contentTintColor = NSColor.tertiaryLabelColor;
        [self addSubview:_imageView];

        _titleLabel = VCMakeLabel(@"Drop an Application Here", [NSFont systemFontOfSize:20.0 weight:NSFontWeightSemibold],
                                  NSColor.labelColor);
        _titleLabel.alignment = NSTextAlignmentCenter;
        [self addSubview:_titleLabel];

        _subtitleLabel = VCMakeLabel(
            @"Drag any app from Finder to analyze it, or choose one from the sidebar.\n"
            @"VortexClean finds its caches, preferences, containers, launch agents and package files.",
            [NSFont systemFontOfSize:13.0], NSColor.secondaryLabelColor);
        _subtitleLabel.alignment = NSTextAlignmentCenter;
        _subtitleLabel.lineBreakMode = NSLineBreakByWordWrapping;
        _subtitleLabel.maximumNumberOfLines = 3;
        [self addSubview:_subtitleLabel];

        _chooseButton = [NSButton buttonWithTitle:@"Choose Application…" target:nil action:nil];
        _chooseButton.translatesAutoresizingMaskIntoConstraints = NO;
        _chooseButton.bezelStyle = NSBezelStyleRounded;
        _chooseButton.controlSize = NSControlSizeLarge;
        [self addSubview:_chooseButton];

        [NSLayoutConstraint activateConstraints:@[
            [_imageView.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
            [_imageView.bottomAnchor constraintEqualToAnchor:_titleLabel.topAnchor constant:-16.0],
            [_titleLabel.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
            [_titleLabel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor constant:-6.0],
            [_subtitleLabel.topAnchor constraintEqualToAnchor:_titleLabel.bottomAnchor constant:8.0],
            [_subtitleLabel.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
            [_subtitleLabel.widthAnchor constraintLessThanOrEqualToConstant:460.0],
            [_subtitleLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.leadingAnchor constant:48.0],
            [_chooseButton.topAnchor constraintEqualToAnchor:_subtitleLabel.bottomAnchor constant:18.0],
            [_chooseButton.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        ]];
    }
    return self;
}

- (void)setHighlighted:(BOOL)highlighted {
    if (_highlighted == highlighted) return;
    _highlighted = highlighted;
    _imageView.contentTintColor = highlighted ? NSColor.controlAccentColor : NSColor.tertiaryLabelColor;
    self.needsDisplay = YES;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    // The dashed outline of the drop target, drawn with Core Graphics.
    const CGRect outline = CGRectInset(self.bounds, 28.0, 28.0);
    if (outline.size.width < 80.0 || outline.size.height < 80.0) return;
    CGContextRef context = NSGraphicsContext.currentContext.CGContext;
    CGPathRef path = CGPathCreateWithRoundedRect(outline, 18.0, 18.0, nullptr);
    NSColor *stroke = _highlighted ? NSColor.controlAccentColor : NSColor.separatorColor;
    if (_highlighted) {
        CGContextSetFillColorWithColor(context, [NSColor.controlAccentColor colorWithAlphaComponent:0.08].CGColor);
        CGContextAddPath(context, path);
        CGContextFillPath(context);
    }
    const CGFloat dashes[] = {8.0, 6.0};
    CGContextSetLineDash(context, 0.0, dashes, 2);
    CGContextSetLineWidth(context, _highlighted ? 2.5 : 1.5);
    CGContextSetStrokeColorWithColor(context, stroke.CGColor);
    CGContextAddPath(context, path);
    CGContextStrokePath(context);
    CGPathRelease(path);
}

@end

// ---------------------------------------------------------------------------
#pragma mark - Drop overlay

@implementation VCDropOverlayView {
    NSTextField *_label;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.translatesAutoresizingMaskIntoConstraints = NO;
        _label = VCMakeLabel(@"Drop to Analyze", [NSFont systemFontOfSize:22.0 weight:NSFontWeightSemibold],
                             NSColor.controlAccentColor);
        [self addSubview:_label];
        [NSLayoutConstraint activateConstraints:@[
            [_label.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
            [_label.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        ]];
    }
    return self;
}

/// The overlay is purely visual: clicks and drags fall through to what is underneath.
- (NSView *)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    CGContextRef context = NSGraphicsContext.currentContext.CGContext;
    CGContextSetFillColorWithColor(context, [NSColor.windowBackgroundColor colorWithAlphaComponent:0.86].CGColor);
    CGContextFillRect(context, self.bounds);

    const CGRect outline = CGRectInset(self.bounds, 14.0, 14.0);
    CGPathRef path = CGPathCreateWithRoundedRect(outline, 14.0, 14.0, nullptr);
    CGContextSetFillColorWithColor(context, [NSColor.controlAccentColor colorWithAlphaComponent:0.10].CGColor);
    CGContextAddPath(context, path);
    CGContextFillPath(context);
    const CGFloat dashes[] = {9.0, 6.0};
    CGContextSetLineDash(context, 0.0, dashes, 2);
    CGContextSetLineWidth(context, 2.5);
    CGContextSetStrokeColorWithColor(context, NSColor.controlAccentColor.CGColor);
    CGContextAddPath(context, path);
    CGContextStrokePath(context);
    CGPathRelease(path);
}

@end

// ---------------------------------------------------------------------------
#pragma mark - Banner

@implementation VCBannerView {
    NSImageView *_iconView;
    NSTextField *_label;
    NSColor *_tint;
}

- (instancetype)initWithSymbol:(NSString *)symbolName tint:(NSColor *)tint actionTitle:(NSString *)actionTitle {
    if ((self = [super initWithFrame:NSZeroRect])) {
        _tint = tint;
        _message = @"";
        self.translatesAutoresizingMaskIntoConstraints = NO;
        self.wantsLayer = YES;
        self.layer.cornerRadius = 9.0;
        self.layer.cornerCurve = kCACornerCurveContinuous;
        self.layer.borderWidth = 1.0;

        _iconView = [[NSImageView alloc] initWithFrame:NSZeroRect];
        _iconView.translatesAutoresizingMaskIntoConstraints = NO;
        _iconView.image = VCSymbol(symbolName, 15.0, NSFontWeightMedium);
        _iconView.contentTintColor = tint;
        [self addSubview:_iconView];

        _label = VCMakeLabel(@"", [NSFont systemFontOfSize:12.0], NSColor.labelColor);
        _label.lineBreakMode = NSLineBreakByWordWrapping;
        _label.maximumNumberOfLines = 3;
        [self addSubview:_label];

        _actionButton = [NSButton buttonWithTitle:actionTitle target:nil action:nil];
        _actionButton.translatesAutoresizingMaskIntoConstraints = NO;
        _actionButton.bezelStyle = NSBezelStyleRounded;
        _actionButton.controlSize = NSControlSizeSmall;
        [_actionButton setContentHuggingPriority:NSLayoutPriorityRequired
                                  forOrientation:NSLayoutConstraintOrientationHorizontal];
        [self addSubview:_actionButton];

        NSImage *closeGlyph = VCSymbol(@"xmark", 10.0, NSFontWeightSemibold);
        if (closeGlyph == nil) closeGlyph = [NSImage imageNamed:NSImageNameStopProgressTemplate];
        _closeButton = [NSButton buttonWithImage:closeGlyph target:nil action:nil];
        _closeButton.translatesAutoresizingMaskIntoConstraints = NO;
        _closeButton.bordered = NO;
        _closeButton.contentTintColor = NSColor.secondaryLabelColor;
        [_closeButton setAccessibilityLabel:@"Dismiss"];
        [self addSubview:_closeButton];

        [NSLayoutConstraint activateConstraints:@[
            [_iconView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:12.0],
            [_iconView.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [_iconView.widthAnchor constraintEqualToConstant:18.0],
            [_label.leadingAnchor constraintEqualToAnchor:_iconView.trailingAnchor constant:8.0],
            [_label.topAnchor constraintEqualToAnchor:self.topAnchor constant:8.0],
            [_label.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-8.0],
            [_label.trailingAnchor constraintLessThanOrEqualToAnchor:_actionButton.leadingAnchor constant:-10.0],
            [_actionButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [_actionButton.trailingAnchor constraintEqualToAnchor:_closeButton.leadingAnchor constant:-8.0],
            [_closeButton.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-10.0],
            [_closeButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [_closeButton.widthAnchor constraintEqualToConstant:16.0],
            [self.heightAnchor constraintGreaterThanOrEqualToConstant:36.0],
        ]];
        [self applyColors];
    }
    return self;
}

- (void)setMessage:(NSString *)message {
    _message = [message copy];
    _label.stringValue = _message;
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    [self applyColors];
}

- (void)applyColors {
    [self.effectiveAppearance performAsCurrentDrawingAppearance:^{
        self.layer.backgroundColor = [self->_tint colorWithAlphaComponent:0.14].CGColor;
        self.layer.borderColor = [self->_tint colorWithAlphaComponent:0.35].CGColor;
    }];
}

@end
