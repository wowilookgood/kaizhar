// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>

namespace Core {
class System;
}

namespace Service::IR {

/// Kaizhar: whether the running title reads a second stick, for the frontend's
/// choice of what the right stick is (citra_switch/main.cpp, RightStickRole). States, not
/// counters: written on the emulation thread where the title asks, read anywhere.
/// hen_rst_in_use: ir:rst (the New 3DS's C-Stick, ZL, ZR) between its Initialize and Shutdown.
/// hen_cpp_in_use: the Circle Pad Pro over ir:USER, from the title's request to send its state
/// (ConfigureHIDPolling) until the connection ends.
inline std::atomic<bool> hen_rst_in_use{false};
inline std::atomic<bool> hen_cpp_in_use{false};

void InstallInterfaces(Core::System& system);

} // namespace Service::IR
