// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>

#include "common/common_types.h"

/// (Switch 2 port) The caches that make a second start smooth, and removing them so that a
/// first start can be judged again: the title list's "Clear caches". Three kinds: the shader and
/// pipeline cache files (the user folder's shader directory), the lists of compiled blocks
/// (jit, jit-fresh), and the Vulkan driver's own cache.
///
/// The driver keeps its cache in one file per driver build under /config/ouncevk, shared by
/// every program built with the same devkit. Kaizhar gives it a folder of its own
/// (MESA_SHADER_CACHE_DIR; the driver adds "shader-cache"), so that clearing touches nobody
/// else's: /switch/Kaizhar/ouncevk/shader-cache/. Nothing here ever writes to or removes
/// anything under /config/ouncevk unless --shared-driver-cache says to use that folder.
namespace Caches {

struct Tally {
    unsigned files = 0;       ///< removed
    u64 bytes = 0;            ///< their sizes
    unsigned failed = 0;      ///< could not be removed
    std::string first_failed; ///< the first of those
};

/// Once, before anything uses the driver (the menu does). shared: --shared-driver-cache, the
/// driver's default folder. cold: --cold-driver, the files of the folder in use
/// are removed. Otherwise, the first time only: the shared folder's files are copied (never
/// moved) into the private one, so that an update does not start cold.
void SetupDriverCache(bool shared, bool cold);

/// The folder the driver's cache is in for this run (for the session report).
const char* DriverDir();

/// Removes the three kinds. Nothing of the driver's may be open: no menu, no title.
Tally ClearAll();

} // namespace Caches
