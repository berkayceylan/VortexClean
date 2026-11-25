// VortexClean UI — the storage visualiser.
//
// A segmented horizontal bar with a legend that shows how the reclaimable space
// is distributed across categories. Segments are Core Animation layers whose
// content is drawn with Core Graphics at the display's backing scale, so they
// stay sharp on Retina screens and animate on the render server (no timers).
// Hovering a segment or a legend entry highlights it and shows a tooltip.
#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface VCStorageSegment : NSObject
@property (nonatomic, readonly, copy) NSString *title;
@property (nonatomic, readonly) uint64_t bytes;
@property (nonatomic, readonly, strong) NSColor *color;
@property (nonatomic, readonly) NSUInteger itemCount;
+ (instancetype)segmentWithTitle:(NSString *)title
                           bytes:(uint64_t)bytes
                           color:(NSColor *)color
                       itemCount:(NSUInteger)itemCount;
@end

@interface StorageBreakdownView : NSView

/// The segments currently shown. Assigning animates the bar to the new distribution.
@property (nonatomic, copy) NSArray<VCStorageSegment *> *segments;
- (void)setSegments:(NSArray<VCStorageSegment *> *)segments animated:(BOOL)animated;

/// While YES the summary line says that sizes are still being measured.
@property (nonatomic, getter=isMeasuring) BOOL measuring;

/// Index of the segment under the pointer, or -1.
@property (nonatomic, readonly) NSInteger highlightedIndex;

/// The slice of the bar that belongs to a segment, in this view's coordinates
/// (NSZeroRect for a segment that is empty or does not exist).
- (NSRect)frameOfSegmentAtIndex:(NSUInteger)index;

@end

NS_ASSUME_NONNULL_END
