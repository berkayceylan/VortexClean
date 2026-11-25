// VortexClean bridge — construction of the Objective-C models from engine types.
// Private to the bridge: this is the only header that mixes the two worlds.
#import "VCModels.h"

#include <string>
#include <string_view>
#include <vector>

#include "DeletionEngine.hpp"
#include "ProcessInspector.hpp"
#include "Remnant.hpp"
#include "RemnantScanner.hpp"

NS_ASSUME_NONNULL_BEGIN

/// UTF-8 std::string -> NSString. Never nil: invalid sequences are repaired first.
NSString *VCStringFromStd(std::string_view text);
/// NSString -> file-system representation as std::string.
std::string VCStdFromString(NSString *_Nullable string);

@interface VCApp ()
- (instancetype)initWithAppInfo:(const vortex::AppInfo &)info external:(BOOL)external;
/// The engine's view of this application. Safe to read from any thread.
- (const vortex::AppInfo &)appInfo;
@end

@interface VCRemnant ()
- (instancetype)initWithRemnant:(const vortex::Remnant &)remnant
                          index:(NSUInteger)index
                          roots:(const vortex::ScanRoots &)roots;
- (const vortex::Remnant &)remnant;
@end

@interface VCScanResult ()
- (instancetype)initWithResult:(const vortex::ScanResult &)result app:(VCApp *)app roots:(const vortex::ScanRoots &)roots;
@end

@interface VCProcess ()
- (instancetype)initWithProcess:(const vortex::RunningProcess &)process;
- (const vortex::RunningProcess &)process;
@end

@interface VCCleanReport ()
- (instancetype)initWithReport:(const vortex::DeletionReport &)report;
@end

NS_ASSUME_NONNULL_END
