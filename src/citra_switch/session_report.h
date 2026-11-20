// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>

#include "common/common_types.h"

/// A short plain-text account of the last run, for when someone reports a problem that can't
/// be reproduced: /switch/Kaizhar/log/last-session.txt (the one before it is kept as
/// last-session.old.txt). Written by every build. It holds the version, docked or handheld,
/// each title's ID and name, the resolution and screen arrangement, what the CPU boost's calls
/// returned, the state of the caches at the start, one line for every five seconds of play,
/// and totals. Nothing of the user's: no nickname, no addresses, no file names.
///
/// The numbers are the ones the development log is made of (main.cpp hands them over); nothing
/// is measured for this. Lines are kept in memory and reach the card from a thread of this
/// file's, every thirty seconds and when a title ends, so that a run closed from the HOME menu
/// leaves all but its last half minute. The emulation thread never writes the file.
namespace SessionReport {

struct Title {
    u64 id = 0;
    std::string name;        ///< as the title names itself; may be empty
    unsigned resolution = 0; ///< the internal resolution factor; 0: the size shown
    std::string layout;      ///< the screen arrangement, in words
    bool jit_list = false;   ///< the list of compiled blocks is in use
    std::size_t jit_listed = 0;   ///< ...blocks in it when the title started
    std::size_t jit_compiled = 0; ///< ...of them compiled before the title ran
    std::string shader_dir;  ///< where the title's shader cache files are (never written out)
    unsigned long long blocks_now = 0; ///< JIT blocks compiled so far
};

/// A title starts. The first call of a run starts the file.
void Begin(const Title& title);

struct Interval {
    double seconds = 5.0;
    double speed = 0;    ///< 1.0: full speed
    double game_fps = 0;
    unsigned long long shown = 0, gaps40 = 0, gaps70 = 0, gaps120 = 0; ///< frames shown, gaps between them
    double longest_gap_ms = 0;
    unsigned long long pipelines = 0;    ///< compiled by the driver in the interval
    unsigned long long blocks_total = 0; ///< JIT blocks compiled so far
    unsigned long long boost_on_ms = 0;
};

/// Every five seconds while a title runs.
void Add(const Interval& interval);

/// A line of its own before the title's totals (the online record's counts).
void Note(const std::string& line);

/// The title has ended: its totals, and everything to the card.
void End(const char* how);

/// At exit: ends the thread.
void Close();

} // namespace SessionReport
