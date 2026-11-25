// VortexClean bridge — the two Cocoa-backed services the C++ deletion engine
// needs but cannot provide itself: the Trash and privilege elevation.
#pragma once

#include <string>
#include <vector>

#include "DeletionEngine.hpp"
#include "Result.hpp"

namespace vcbridge {

/// Moves an item to the Trash with -[NSFileManager trashItemAtURL:resultingItemURL:error:]
/// and returns where it ended up. Thread-safe.
vortex::Result<std::string> moveToTrash(const std::string& path);

/// Asks for administrator authorization (the standard macOS password dialog),
/// then runs this executable as root in `--privileged-helper` mode and hands it
/// the items. Works for locally built, ad-hoc signed copies of the app, which
/// cannot register a privileged launchd helper. Blocks until the helper is done;
/// must not be called on the main thread.
vortex::PrivilegedBatchResult runPrivilegedBatch(const std::vector<vortex::DeletionItem>& items,
                                                 vortex::DeleteMode mode);

/// Hooks for a synthetic disk (`--root`): the Trash is `<home>/.Trash` inside it,
/// launchd is never contacted, and administrator rights are never requested.
vortex::DeletionHooks sandboxHooks(const std::string& home);

/// True when this macOS still offers the mechanism runPrivilegedBatch relies on.
bool isElevationAvailable();

}  // namespace vcbridge
