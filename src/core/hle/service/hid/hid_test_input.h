// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}

/// Scripted input for tests run on a computer (hen/tests/multiplayer): with the environment
/// variable KAIZHAR_TEST_DIR set to a folder, the emulated console takes button presses and
/// other commands from the file "cmd" in it, and says where it is in "status". Nothing without
/// the variable, and nothing at all in the console's build. Commands, one a line:
///   press BUTTONS MS     hold the buttons (A B X Y L R START SELECT UP DOWN LEFT RIGHT, joined
///                        by +) for MS milliseconds of emulated time
///   shot NAME            both screens into NAME.raw ("W H\n", then RGBA)
///   hostroom PORT        open a multiplayer room in this instance
///   room NICK ADDRESS PORT   join a multiplayer room
///   leave                leave it
namespace Service::HID::TestInput {

/// Called for every sample of the buttons: adds the scripted ones to buttons (the HID pad bits).
void Apply(Core::System& system, u32& buttons);

/// Called when the program discards a download play host (dlp_clt_base.cpp): with the
/// environment variable KAIZHAR_TEST_TRACE_CODE set to a folder, the registers and the stack's
/// top go to the log, and once, the program's code to "code.bin" in that folder.
void TraceDiscard(Core::System& system);

} // namespace Service::HID::TestInput
