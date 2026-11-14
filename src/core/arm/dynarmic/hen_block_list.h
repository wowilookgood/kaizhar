// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The blocks of program code the CPU JIT compiled, kept in a file per title, so that a later
// session compiles them before the program gets to them: some when the title starts and the
// rest in the time the frame limiter would otherwise sleep. Compiling when first needed is what
// makes a session's first seconds and every new scene slow (on Switch 2 homebrew also because
// making new code executable costs a system call there: hen/jit/hen_jit.c).
//
// A listed block is only compiled if the program's code at its address still hashes to what
// was recorded, so code loaded later or changed since is left to the normal path.

#pragma once

#include <chrono>
#include "common/common_types.h"

namespace Dynarmic::A32 {
class Jit;
}

namespace Core::HenBlockList {

/// Loads the title's list and compiles from it for up to start_budget_ms.
void Open(u64 title_id, unsigned start_budget_ms, const char* folder = "jit/");
/// Whether idle time also goes to compiling along the branches of compiled blocks (on by
/// default).
void SetSpeculation(bool enabled);
/// Appends the blocks recorded since the last call to the title's file.
void Flush(bool wait = false);
void Attach(Dynarmic::A32::Jit* jit, u16 tag);
void Detach(Dynarmic::A32::Jit* jit);
/// Compiles listed blocks until shortly before the deadline.
void Idle(std::chrono::steady_clock::time_point deadline);
void UpgradeLinks();

struct Stats {
    std::size_t listed;   ///< blocks in the file when it was loaded
    std::size_t recorded; ///< new this session
    std::size_t compiled; ///< compiled ahead of use from the list
    std::size_t speculated; ///< compiled ahead of use along branches, in idle time
    std::size_t waiting;  ///< listed, not compiled (yet)
    double start_ms;      ///< spent compiling when the title started
    double idle_ms;       ///< spent compiling in idle time, in total
};
Stats GetStats();

} // namespace Core::HenBlockList
