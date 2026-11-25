#import "VCFormat.h"

NSString *VCFormatBytes(uint64_t bytes) {
    static NSByteCountFormatter *formatter;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        formatter = [[NSByteCountFormatter alloc] init];
        formatter.countStyle = NSByteCountFormatterCountStyleFile;
        formatter.zeroPadsFractionDigits = NO;
        formatter.adaptive = YES;
    });
    const long long clamped = bytes > static_cast<uint64_t>(LLONG_MAX) ? LLONG_MAX : static_cast<long long>(bytes);
    if (clamped == 0) return @"Zero KB";
    return [formatter stringFromByteCount:clamped];
}

NSString *VCFormatCount(NSUInteger count, NSString *singular, NSString *plural) {
    return [NSString stringWithFormat:@"%lu %@", static_cast<unsigned long>(count), count == 1 ? singular : plural];
}

NSColor *VCColorForBucket(VCStorageBucket bucket) {
    switch (bucket) {
        case VCStorageBucketAppBundle: return NSColor.systemBlueColor;
        case VCStorageBucketApplicationSupport: return NSColor.systemPurpleColor;
        case VCStorageBucketCaches: return NSColor.systemOrangeColor;
        case VCStorageBucketContainers: return NSColor.systemTealColor;
        case VCStorageBucketPreferences: return NSColor.systemGreenColor;
        case VCStorageBucketLogs: return NSColor.systemYellowColor;
        case VCStorageBucketSystem: return NSColor.systemIndigoColor;
        case VCStorageBucketCount: break;
    }
    return NSColor.systemGrayColor;
}

NSColor *VCColorForConfidence(VCConfidence confidence) {
    switch (confidence) {
        case VCConfidenceExact: return NSColor.systemGreenColor;
        case VCConfidenceHigh: return NSColor.systemBlueColor;
        case VCConfidenceMedium: return NSColor.systemOrangeColor;
    }
    return NSColor.systemGrayColor;
}

NSImage *VCSymbol(NSString *name, CGFloat pointSize, NSFontWeight weight) {
    NSImage *image = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    if (image == nil) return nil;
    NSImageSymbolConfiguration *configuration =
        [NSImageSymbolConfiguration configurationWithPointSize:pointSize weight:weight];
    NSImage *configured = [image imageWithSymbolConfiguration:configuration];
    return configured != nil ? configured : image;
}

NSTextField *VCMakeLabel(NSString *text, NSFont *font, NSColor *color) {
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = font;
    label.textColor = color;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

// ---------------------------------------------------------------------------
#pragma mark - VCIconCache

@implementation VCIconCache {
    NSCache<NSString *, NSImage *> *_cache;
    dispatch_queue_t _queue;
}

+ (VCIconCache *)sharedCache {
    static VCIconCache *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        cache = [[VCIconCache alloc] init];
    });
    return cache;
}

- (instancetype)init {
    if ((self = [super init])) {
        _cache = [[NSCache alloc] init];
        _cache.countLimit = 2000;
        // Serial: NSWorkspace icon lookups are issued one at a time, off the main thread.
        _queue = dispatch_queue_create(
            "com.berkayceylan.VortexClean.icons",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    }
    return self;
}

- (NSImage *)cachedIconForPath:(NSString *)path {
    return [_cache objectForKey:path];
}

- (void)loadIconForPath:(NSString *)path completion:(void (^)(NSImage *))completion {
    NSImage *cached = [_cache objectForKey:path];
    if (cached != nil) {
        completion(cached);
        return;
    }
    dispatch_async(_queue, ^{
        NSImage *icon = [self->_cache objectForKey:path];
        if (icon == nil) {
            icon = [[NSWorkspace sharedWorkspace] iconForFile:path];
            [self->_cache setObject:icon forKey:path];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(icon);
        });
    });
}

- (void)removeIconForPath:(NSString *)path {
    [_cache removeObjectForKey:path];
}

@end

// ---------------------------------------------------------------------------
#pragma mark - VCPillView

@implementation VCPillView {
    NSTextField *_label;
    NSImageView *_imageView;
    NSLayoutConstraint *_labelLeadingToEdge;
    NSLayoutConstraint *_labelLeadingToImage;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.translatesAutoresizingMaskIntoConstraints = NO;
        self.wantsLayer = YES;
        self.layer.cornerRadius = 8.0;
        self.layer.cornerCurve = kCACornerCurveContinuous;
        _tintColor = NSColor.systemGrayColor;
        _text = @"";

        _imageView = [[NSImageView alloc] initWithFrame:NSZeroRect];
        _imageView.translatesAutoresizingMaskIntoConstraints = NO;
        _imageView.imageScaling = NSImageScaleProportionallyDown;
        _imageView.hidden = YES;
        [self addSubview:_imageView];

        _label = VCMakeLabel(@"", [NSFont systemFontOfSize:10.5 weight:NSFontWeightMedium], NSColor.labelColor);
        [_label setContentCompressionResistancePriority:NSLayoutPriorityDefaultHigh
                                         forOrientation:NSLayoutConstraintOrientationHorizontal];
        [_label setContentHuggingPriority:NSLayoutPriorityRequired
                           forOrientation:NSLayoutConstraintOrientationHorizontal];
        [self addSubview:_label];

        _labelLeadingToEdge = [_label.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:7.0];
        _labelLeadingToImage = [_label.leadingAnchor constraintEqualToAnchor:_imageView.trailingAnchor constant:3.0];
        [NSLayoutConstraint activateConstraints:@[
            [self.heightAnchor constraintEqualToConstant:16.0],
            [_imageView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:5.0],
            [_imageView.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [_imageView.widthAnchor constraintEqualToConstant:11.0],
            [_imageView.heightAnchor constraintEqualToConstant:11.0],
            _labelLeadingToEdge,
            [_label.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-7.0],
            [_label.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        ]];
        [self applyColors];
    }
    return self;
}

- (void)setText:(NSString *)text {
    _text = [text copy];
    _label.stringValue = _text;
}

- (void)setTintColor:(NSColor *)tintColor {
    _tintColor = tintColor;
    [self applyColors];
}

- (void)setImage:(NSImage *)image {
    _image = image;
    _imageView.image = image;
    _imageView.hidden = image == nil;
    _labelLeadingToEdge.active = image == nil;
    _labelLeadingToImage.active = image != nil;
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    [self applyColors];
}

/// CGColors do not follow the appearance by themselves: resolve them again on every change.
- (void)applyColors {
    [self.effectiveAppearance performAsCurrentDrawingAppearance:^{
        self.layer.backgroundColor = [self->_tintColor colorWithAlphaComponent:0.18].CGColor;
    }];
    _label.textColor = [NSColor.labelColor colorWithAlphaComponent:0.85];
    _imageView.contentTintColor = _tintColor;
}

@end
