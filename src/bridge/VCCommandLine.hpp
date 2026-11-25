// VortexClean bridge — a small read-only command-line surface over the engine.
//
// It exists so the bundle can be verified on machines without a window server
// (CI, ssh): `VortexClean --version`, `--list`, `--scan <app>`, `--dry-run <app>`.
// Nothing here can delete anything: the only pipeline it runs is the dry run.
#pragma once

#include "RemnantScanner.hpp"

namespace vcbridge {

/// Handles the command line when it asks for one of the headless commands.
/// Returns true and sets `exitStatus` when it did; returns false when the
/// arguments are not ours and the graphical application should start.
/// `roots` is the disk to work on: the real one, or a sandbox given with `--root`.
bool runCommandLine(int argc, const char* const* argv, const vortex::ScanRoots& roots, int& exitStatus);

}  // namespace vcbridge
