#import "StorageBreakdownView.h"

#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <vector>

#import "VCFormat.h"

namespace {

constexpr CGFloat kSummaryHeight = 18.0;
constexpr CGFloat kBarTopOffset = 24.0;
constexpr CGFloat kBarHeight = 14.0;
constexpr CGFloat kLegendTopOffset = kBarTopOffset + kBarHeight + 12.0;
constexpr CGFloat kLegendRowHeight = 18.0;
constexpr CGFloat kLegendSwatch = 8.0;
constexpr CGFloat kLegendItemSpacing = 18.0;
constexpr CGFloat kMinimumSegmentWidth = 3.0;
constexpr CFTimeInterval kAnimationDuration = 0.45;

}  // namespace

// ---------------------------------------------------------------------------
#pragma mark - Model

@implementation VCStorageSegment

+ (instancetype)segmentWithTitle:(NSString *)title
                           bytes:(uint64_t)bytes
                           color:(NSColor *)color
                       itemCount:(NSUInteger)itemCount {
    VCStorageSegment *segment = [[VCStorageSegment alloc] init];
    segment->_title = [title copy];
    segment->_bytes = bytes;
    segment->_color = color;
    segment->_itemCount = itemCount;
    return segment;
}

@end

// ---------------------------------------------------------------------------
#pragma mark - Segment layer (Core Graphics content)

/// One slice of the bar. Its content is rendered with Core Graphics into a
/// backing store sized for the display's scale factor.
@interface VCSegmentLayer : CALayer
@property (nonatomic) CGColorRef fillColor;
@property (nonatomic) CGColorRef separatorColor;
@end

@implementation VCSegmentLayer {
    CGColorRef _fillColor;
    CGColorRef _separatorColor;
}

- (instancetype)init {
    if ((self = [super init])) {
        self.needsDisplayOnBoundsChange = YES;
        self.contentsGravity = kCAGravityResize;
        // Geometry changes cross-fade nothing: only position and bounds animate.
        self.actions = @{@"contents": [NSNull null]};
    }
    return self;
}

- (void)dealloc {
    CGColorRelease(_fillColor);
    CGColorRelease(_separatorColor);
}

- (CGColorRef)fillColor {
    return _fillColor;
}

- (void)setFillColor:(CGColorRef)fillColor {
    if (fillColor == _fillColor) return;
    CGColorRetain(fillColor);
    CGColorRelease(_fillColor);
    _fillColor = fillColor;
    [self setNeedsDisplay];
}

- (CGColorRef)separatorColor {
    return _separatorColor;
}

- (void)setSeparatorColor:(CGColorRef)separatorColor {
    if (separatorColor == _separatorColor) return;
    CGColorRetain(separatorColor);
    CGColorRelease(_separatorColor);
    _separatorColor = separatorColor;
    [self setNeedsDisplay];
}

- (void)drawInContext:(CGContextRef)context {
    const CGRect bounds = self.bounds;
    if (CGRectIsEmpty(bounds) || _fillColor == nullptr) return;

    CGContextSetFillColorWithColor(context, _fillColor);
    CGContextFillRect(context, bounds);

    // A soft highlight along the upper half gives the bar a little depth.
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGFloat components[] = {1.0, 1.0, 1.0, 0.24, 1.0, 1.0, 1.0, 0.0};
    const CGFloat locations[] = {0.0, 1.0};
    CGGradientRef gradient = CGGradientCreateWithColorComponents(space, components, locations, 2);
    CGContextSaveGState(context);
    CGContextClipToRect(context, bounds);
    CGContextDrawLinearGradient(context, gradient, CGPointMake(0.0, CGRectGetMaxY(bounds)),
                                CGPointMake(0.0, CGRectGetMidY(bounds)), 0);
    CGContextRestoreGState(context);
    CGGradientRelease(gradient);
    CGColorSpaceRelease(space);

    // A one-device-pixel divider on the trailing edge separates neighbours.
    if (_separatorColor != nullptr) {
        const CGFloat pixel = 1.0 / MAX(self.contentsScale, 1.0);
        CGContextSetFillColorWithColor(context, _separatorColor);
        CGContextFillRect(context, CGRectMake(CGRectGetMaxX(bounds) - pixel, 0.0, pixel, bounds.size.height));
    }
}

@end

// ---------------------------------------------------------------------------
#pragma mark - View

@implementation StorageBreakdownView {
    CALayer *_trackLayer;
    CALayer *_barLayer;
    NSMutableArray<VCSegmentLayer *> *_segmentLayers;
    CALayer *_bubbleLayer;
    CATextLayer *_bubbleText;
    NSTrackingArea *_trackingArea;
    std::vector<CGRect> _segmentFrames;  // in view coordinates
    std::vector<CGRect> _legendFrames;   // in view coordinates
    CGFloat _legendHeight;
    NSSize _laidOutSize;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _segments = @[];
        _segmentLayers = [NSMutableArray array];
        _highlightedIndex = -1;
        _legendHeight = kLegendRowHeight;
        self.translatesAutoresizingMaskIntoConstraints = NO;
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;

        _trackLayer = [CALayer layer];
        _trackLayer.cornerRadius = kBarHeight / 2.0;
        _trackLayer.cornerCurve = kCACornerCurveContinuous;
        [self.layer addSublayer:_trackLayer];

        _barLayer = [CALayer layer];
        _barLayer.cornerRadius = kBarHeight / 2.0;
        _barLayer.cornerCurve = kCACornerCurveContinuous;
        _barLayer.masksToBounds = YES;
        [self.layer addSublayer:_barLayer];

        _bubbleLayer = [CALayer layer];
        _bubbleLayer.cornerRadius = 6.0;
        _bubbleLayer.opacity = 0.0;
        _bubbleLayer.shadowOpacity = 0.25;
        _bubbleLayer.shadowRadius = 4.0;
        _bubbleLayer.shadowOffset = CGSizeMake(0.0, -1.0);
        _bubbleLayer.zPosition = 10.0;
        _bubbleText = [CATextLayer layer];
        _bubbleText.alignmentMode = kCAAlignmentCenter;
        _bubbleText.truncationMode = kCATruncationEnd;
        _bubbleText.fontSize = 11.0;
        _bubbleText.font = (__bridge CFTypeRef)[NSFont systemFontOfSize:11.0 weight:NSFontWeightMedium];
        [_bubbleLayer addSublayer:_bubbleText];
        [self.layer addSublayer:_bubbleLayer];

        _trackingArea = [[NSTrackingArea alloc]
            initWithRect:NSZeroRect
                 options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInActiveApp |
                         NSTrackingInVisibleRect
                   owner:self
                userInfo:nil];
        [self addTrackingArea:_trackingArea];

        [self setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationVertical];
        [self setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                       forOrientation:NSLayoutConstraintOrientationVertical];
        [self setAccessibilityElement:YES];
        [self setAccessibilityRole:NSAccessibilityImageRole];
        [self setAccessibilityLabel:@"Storage breakdown"];
        [self applyAppearance];
    }
    return self;
}

// ---------------------------------------------------------------------------
#pragma mark Data

- (void)setSegments:(NSArray<VCStorageSegment *> *)segments {
    [self setSegments:segments animated:self.window != nil];
}

- (void)setSegments:(NSArray<VCStorageSegment *> *)segments animated:(BOOL)animated {
    _segments = segments != nil ? [segments copy] : @[];
    if (_highlightedIndex >= static_cast<NSInteger>(_segments.count)) [self setHighlightedIndex:-1 at:NSZeroPoint];
    [self layoutLegend];
    [self layoutSegmentsAnimated:animated];
    [self setAccessibilityValue:[self summaryText]];
    self.needsDisplay = YES;
}

- (void)setMeasuring:(BOOL)measuring {
    if (_measuring == measuring) return;
    _measuring = measuring;
    self.needsDisplay = YES;
}

- (uint64_t)totalBytes {
    uint64_t total = 0;
    for (VCStorageSegment *segment in _segments) total += segment.bytes;
    return total;
}

- (NSString *)summaryText {
    NSUInteger items = 0;
    for (VCStorageSegment *segment in _segments) items += segment.itemCount;
    const uint64_t total = [self totalBytes];
    if (_measuring) {
        return total > 0 ? [NSString stringWithFormat:@"Measuring… %@ so far", VCFormatBytes(total)] : @"Measuring…";
    }
    if (items == 0) return @"Nothing selected";
    return [NSString stringWithFormat:@"%@ in %@", VCFormatBytes(total), VCFormatCount(items, @"item", @"items")];
}

- (NSString *)detailTextForIndex:(NSInteger)index {
    VCStorageSegment *segment = _segments[static_cast<NSUInteger>(index)];
    const uint64_t total = [self totalBytes];
    const double share = total > 0 ? 100.0 * static_cast<double>(segment.bytes) / static_cast<double>(total) : 0.0;
    // Localised like the sizes next to it (decimal separator).
    const BOOL tiny = share > 0.0 && share < 0.1;
    NSString *percent = [NSString localizedStringWithFormat:@"%@%.1f%%", tiny ? @"<" : @"", tiny ? 0.1 : share];
    return [NSString stringWithFormat:@"%@ — %@ · %@ · %@", segment.title, VCFormatBytes(segment.bytes), percent,
                                      VCFormatCount(segment.itemCount, @"item", @"items")];
}

// ---------------------------------------------------------------------------
#pragma mark Layout

- (BOOL)isFlipped {
    return NO;
}

- (NSSize)intrinsicContentSize {
    return NSMakeSize(NSViewNoIntrinsicMetric, kLegendTopOffset + _legendHeight + 2.0);
}

/// The bar's rectangle in view coordinates (origin bottom-left).
- (CGRect)barRect {
    const CGRect bounds = self.bounds;
    return CGRectMake(0.0, bounds.size.height - kBarTopOffset - kBarHeight, MAX(bounds.size.width, 0.0), kBarHeight);
}

- (NSDictionary<NSAttributedStringKey, id> *)legendTitleAttributes:(BOOL)emphasised {
    return @{
        NSFontAttributeName: [NSFont systemFontOfSize:11.0 weight:emphasised ? NSFontWeightSemibold : NSFontWeightRegular],
        NSForegroundColorAttributeName: NSColor.labelColor,
    };
}

- (NSDictionary<NSAttributedStringKey, id> *)legendValueAttributes {
    return @{
        NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:11.0 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: NSColor.secondaryLabelColor,
    };
}

/// Flows the legend entries into as many rows as the current width needs.
- (void)layoutLegend {
    _legendFrames.clear();
    const CGFloat width = MAX(self.bounds.size.width, 1.0);
    CGFloat x = 0.0;
    CGFloat row = 0.0;
    for (VCStorageSegment *segment in _segments) {
        const CGFloat titleWidth = ceil([segment.title sizeWithAttributes:[self legendTitleAttributes:YES]].width);
        const CGFloat valueWidth =
            ceil([VCFormatBytes(segment.bytes) sizeWithAttributes:[self legendValueAttributes]].width);
        const CGFloat itemWidth = kLegendSwatch + 6.0 + titleWidth + 5.0 + valueWidth;
        if (x > 0.0 && x + itemWidth > width) {
            x = 0.0;
            row += 1.0;
        }
        const CGFloat top = kLegendTopOffset + row * kLegendRowHeight;
        _legendFrames.push_back(
            CGRectMake(x, self.bounds.size.height - top - kLegendRowHeight, itemWidth, kLegendRowHeight));
        x += itemWidth + kLegendItemSpacing;
    }
    const CGFloat newHeight = (row + 1.0) * kLegendRowHeight;
    if (newHeight != _legendHeight) {
        _legendHeight = newHeight;
        [self invalidateIntrinsicContentSize];
    }
}

- (void)layoutSegmentsAnimated:(BOOL)animated {
    const CGRect bar = [self barRect];
    const NSUInteger count = _segments.count;
    const CGFloat scale = self.window != nil ? self.window.backingScaleFactor : 2.0;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _trackLayer.frame = bar;
    _barLayer.frame = bar;
    // New slices start with zero width at the trailing edge and grow into place.
    while (_segmentLayers.count < count) {
        VCSegmentLayer *layer = [VCSegmentLayer layer];
        layer.anchorPoint = CGPointZero;
        layer.frame = CGRectMake(bar.size.width, 0.0, 0.0, kBarHeight);
        layer.contentsScale = scale;
        [_barLayer addSublayer:layer];
        [_segmentLayers addObject:layer];
    }
    while (_segmentLayers.count > count) {
        [_segmentLayers.lastObject removeFromSuperlayer];
        [_segmentLayers removeLastObject];
    }
    [CATransaction commit];

    // Proportional widths, with a floor so that tiny categories remain visible and hoverable.
    std::vector<CGFloat> widths(count, 0.0);
    const uint64_t total = [self totalBytes];
    if (total > 0 && bar.size.width > 0.0) {
        CGFloat used = 0.0;
        CGFloat flexible = 0.0;
        for (NSUInteger i = 0; i < count; ++i) {
            const uint64_t bytes = _segments[i].bytes;
            if (bytes == 0) continue;
            widths[i] = MAX(kMinimumSegmentWidth,
                            bar.size.width * static_cast<CGFloat>(bytes) / static_cast<CGFloat>(total));
            used += widths[i];
            if (widths[i] > kMinimumSegmentWidth * 2.0) flexible += widths[i];
        }
        const CGFloat overflow = used - bar.size.width;
        if (overflow > 0.0 && flexible > 0.0) {
            for (CGFloat &width : widths) {
                if (width > kMinimumSegmentWidth * 2.0) width -= overflow * (width / flexible);
            }
        }
    }

    [CATransaction begin];
    [CATransaction setDisableActions:!animated];
    [CATransaction setAnimationDuration:kAnimationDuration];
    [CATransaction setAnimationTimingFunction:[CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut]];
    _segmentFrames.assign(count, CGRectZero);
    CGFloat x = 0.0;
    for (NSUInteger i = 0; i < count; ++i) {
        VCSegmentLayer *layer = _segmentLayers[i];
        layer.frame = CGRectMake(x, 0.0, widths[i], kBarHeight);
        _segmentFrames[i] = CGRectMake(bar.origin.x + x, bar.origin.y, widths[i], kBarHeight);
        x += widths[i];
    }
    [CATransaction commit];
    [self applySegmentColors];
    [self applyHighlight];
}

- (void)layout {
    [super layout];
    // Height matters too: legend positions are measured from the top edge, and a
    // legend that needs another row makes the view taller after it was laid out.
    if (!NSEqualSizes(self.bounds.size, _laidOutSize)) {
        _laidOutSize = self.bounds.size;
        [self layoutLegend];
        self.needsDisplay = YES;
    }
    // Resizing must track the window exactly: no animation here.
    [self layoutSegmentsAnimated:NO];
}

// ---------------------------------------------------------------------------
#pragma mark Appearance

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    [self applyAppearance];
    self.needsDisplay = YES;
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    const CGFloat scale = self.window != nil ? self.window.backingScaleFactor : 2.0;
    for (VCSegmentLayer *layer in _segmentLayers) {
        layer.contentsScale = scale;
        [layer setNeedsDisplay];
    }
    _bubbleText.contentsScale = scale;
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self viewDidChangeBackingProperties];
}

/// Layers hold CGColors, which are static: resolve the semantic colours again
/// whenever the appearance (Light/Dark, contrast) changes.
- (void)applyAppearance {
    [self.effectiveAppearance performAsCurrentDrawingAppearance:^{
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        self->_trackLayer.backgroundColor = NSColor.quaternaryLabelColor.CGColor;
        self->_bubbleLayer.backgroundColor = [NSColor colorWithWhite:0.12 alpha:0.94].CGColor;
        self->_bubbleText.foregroundColor = NSColor.whiteColor.CGColor;
        [CATransaction commit];
    }];
    [self applySegmentColors];
}

- (void)applySegmentColors {
    [self.effectiveAppearance performAsCurrentDrawingAppearance:^{
        const CGColorRef separator = NSColor.windowBackgroundColor.CGColor;
        const NSUInteger count = MIN(self->_segmentLayers.count, self->_segments.count);
        for (NSUInteger i = 0; i < count; ++i) {
            self->_segmentLayers[i].fillColor = self->_segments[i].color.CGColor;
            self->_segmentLayers[i].separatorColor = (i + 1 < count) ? separator : nullptr;
        }
    }];
}

// ---------------------------------------------------------------------------
#pragma mark Drawing (summary line and legend)

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const CGRect bounds = self.bounds;
    CGContextRef context = NSGraphicsContext.currentContext.CGContext;

    // Summary line above the bar; replaced by the hovered segment's details.
    NSString *summary = _highlightedIndex >= 0 ? [self detailTextForIndex:_highlightedIndex] : [self summaryText];
    NSDictionary<NSAttributedStringKey, id> *summaryAttributes = @{
        NSFontAttributeName: [NSFont systemFontOfSize:11.0 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: NSColor.secondaryLabelColor,
    };
    [summary drawInRect:NSMakeRect(0.0, bounds.size.height - kSummaryHeight, bounds.size.width, kSummaryHeight)
         withAttributes:summaryAttributes];

    // Legend: a colour swatch, the category name and its size.
    for (NSUInteger i = 0; i < _segments.count && i < _legendFrames.size(); ++i) {
        VCStorageSegment *segment = _segments[i];
        const CGRect frame = _legendFrames[i];
        const BOOL emphasised = static_cast<NSInteger>(i) == _highlightedIndex;
        const BOOL dimmed = _highlightedIndex >= 0 && !emphasised;

        CGContextSaveGState(context);
        CGContextSetAlpha(context, dimmed ? 0.45 : 1.0);
        const CGRect swatch = CGRectMake(frame.origin.x, CGRectGetMidY(frame) - kLegendSwatch / 2.0, kLegendSwatch,
                                         kLegendSwatch);
        CGContextSetFillColorWithColor(context, segment.color.CGColor);
        CGContextFillEllipseInRect(context, swatch);

        NSDictionary<NSAttributedStringKey, id> *titleAttributes = [self legendTitleAttributes:emphasised];
        const NSSize titleSize = [segment.title sizeWithAttributes:titleAttributes];
        const CGFloat textY = CGRectGetMidY(frame) - titleSize.height / 2.0;
        const CGFloat titleX = CGRectGetMaxX(swatch) + 6.0;
        [segment.title drawAtPoint:NSMakePoint(titleX, textY) withAttributes:titleAttributes];
        [VCFormatBytes(segment.bytes) drawAtPoint:NSMakePoint(titleX + ceil(titleSize.width) + 5.0, textY)
                                   withAttributes:[self legendValueAttributes]];
        CGContextRestoreGState(context);
    }
}

// ---------------------------------------------------------------------------
#pragma mark Hover

- (NSRect)frameOfSegmentAtIndex:(NSUInteger)index {
    if (index >= _segmentFrames.size()) return NSZeroRect;
    return NSRectFromCGRect(_segmentFrames[index]);
}

- (NSInteger)indexAtPoint:(NSPoint)point {
    // A slightly taller hit area makes the thin bar easy to hover.
    for (std::size_t i = 0; i < _segmentFrames.size(); ++i) {
        if (_segmentFrames[i].size.width > 0.0 && CGRectContainsPoint(CGRectInset(_segmentFrames[i], 0.0, -4.0), point)) {
            return static_cast<NSInteger>(i);
        }
    }
    for (std::size_t i = 0; i < _legendFrames.size(); ++i) {
        if (CGRectContainsPoint(_legendFrames[i], point)) return static_cast<NSInteger>(i);
    }
    return -1;
}

- (void)mouseMoved:(NSEvent *)event {
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    [self setHighlightedIndex:[self indexAtPoint:point] at:point];
}

- (void)mouseEntered:(NSEvent *)event {
    [self mouseMoved:event];
}

- (void)mouseExited:(NSEvent *)event {
    (void)event;
    [self setHighlightedIndex:-1 at:NSZeroPoint];
}

- (void)setHighlightedIndex:(NSInteger)index at:(NSPoint)point {
    (void)point;
    if (index == _highlightedIndex) return;
    _highlightedIndex = index;
    [self applyHighlight];
    self.needsDisplay = YES;
}

/// Dims every slice except the hovered one and positions the tooltip above it.
- (void)applyHighlight {
    [CATransaction begin];
    [CATransaction setAnimationDuration:0.15];
    for (NSUInteger i = 0; i < _segmentLayers.count; ++i) {
        const BOOL dimmed = _highlightedIndex >= 0 && static_cast<NSInteger>(i) != _highlightedIndex;
        _segmentLayers[i].opacity = dimmed ? 0.35f : 1.0f;
    }
    [CATransaction commit];

    const BOOL show = _highlightedIndex >= 0 && static_cast<std::size_t>(_highlightedIndex) < _segmentFrames.size() &&
                      static_cast<NSUInteger>(_highlightedIndex) < _segments.count;
    if (!show) {
        _bubbleLayer.opacity = 0.0;
        return;
    }
    NSString *text = [self detailTextForIndex:_highlightedIndex];
    NSDictionary<NSAttributedStringKey, id> *attributes =
        @{NSFontAttributeName: [NSFont systemFontOfSize:11.0 weight:NSFontWeightMedium]};
    const NSSize textSize = [text sizeWithAttributes:attributes];
    const CGFloat width = MIN(ceil(textSize.width) + 16.0, MAX(self.bounds.size.width, 40.0));
    const CGFloat height = ceil(textSize.height) + 8.0;
    const CGRect segment = _segmentFrames[static_cast<std::size_t>(_highlightedIndex)];
    CGFloat x = CGRectGetMidX(segment) - width / 2.0;
    x = MAX(0.0, MIN(x, self.bounds.size.width - width));
    const CGFloat y = CGRectGetMaxY([self barRect]) + 5.0;

    [CATransaction begin];
    [CATransaction setDisableActions:_bubbleLayer.opacity == 0.0f];  // jump into place when it first appears
    _bubbleLayer.frame = CGRectMake(round(x), round(y), width, height);
    _bubbleText.frame = CGRectMake(8.0, 3.0, width - 16.0, height - 7.0);
    _bubbleText.string = text;
    [CATransaction commit];
    _bubbleLayer.opacity = 1.0;
}

@end
