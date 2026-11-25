// VortexClean UI — the small views the detail pane is assembled from.
#import <AppKit/AppKit.h>

#import "VCModels.h"

NS_ASSUME_NONNULL_BEGIN

// ---------------------------------------------------------------------------
// Tree model
// ---------------------------------------------------------------------------

@class VCRemnantGroup;

/// One detected item together with its check state.
@interface VCRemnantNode : NSObject
@property (nonatomic, strong) VCRemnant *remnant;
@property (nonatomic) BOOL checked;
@property (nonatomic, weak, nullable) VCRemnantGroup *group;
/// SIP-protected items are locked: they can never be checked.
@property (nonatomic, readonly) BOOL selectable;
@end

/// All items of one category.
@interface VCRemnantGroup : NSObject
@property (nonatomic) NSInteger category;
@property (nonatomic, copy) NSString *title;
@property (nonatomic, strong) NSMutableArray<VCRemnantNode *> *nodes;
/// On, off, or mixed — derived from the selectable items.
@property (nonatomic, readonly) NSControlStateValue checkState;
@property (nonatomic, readonly) uint64_t measuredBytes;
@property (nonatomic, readonly) BOOL fullyMeasured;
@property (nonatomic, readonly) NSUInteger selectableCount;
@end

// ---------------------------------------------------------------------------
// Outline view and cells
// ---------------------------------------------------------------------------

/// Adds keyboard conveniences: Space opens Quick Look, Return toggles the check boxes.
@interface VCOutlineView : NSOutlineView
@end

@interface VCItemCellView : NSTableCellView
@property (nonatomic, readonly, strong) NSButton *checkbox;
- (void)configureWithGroup:(VCRemnantGroup *)group;
- (void)configureWithNode:(VCRemnantNode *)node;
@end

@interface VCMatchCellView : NSTableCellView
- (void)configureWithGroup:(VCRemnantGroup *)group;
- (void)configureWithNode:(VCRemnantNode *)node;
@end

@interface VCSizeCellView : NSTableCellView
- (void)configureWithGroup:(VCRemnantGroup *)group;
- (void)configureWithNode:(VCRemnantNode *)node;
@end

// ---------------------------------------------------------------------------
// Empty state, banner
// ---------------------------------------------------------------------------

/// The empty state: a dashed drop target that invites dragging an application in.
@interface VCDropZoneView : NSView
@property (nonatomic, getter=isHighlighted) BOOL highlighted;
@property (nonatomic, strong, readonly) NSButton *chooseButton;
@end

/// A translucent overlay shown over the whole pane while an application is dragged above it.
@interface VCDropOverlayView : NSView
@end

/// A non-intrusive inline notice with one action and a close button.
@interface VCBannerView : NSView
@property (nonatomic, copy) NSString *message;
@property (nonatomic, strong, readonly) NSButton *actionButton;
@property (nonatomic, strong, readonly) NSButton *closeButton;
- (instancetype)initWithSymbol:(NSString *)symbolName tint:(NSColor *)tint actionTitle:(NSString *)actionTitle;
@end

NS_ASSUME_NONNULL_END
