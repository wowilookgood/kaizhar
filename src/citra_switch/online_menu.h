// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

/// The "Online play (Pretendo)" screen, reached from the Multiplayer screen: linking a 3DS
/// (link_3ds.h), the switch for the online system modules, the emulated console's own System
/// Settings and HOME Menu (where the Pretendo ID is made and the friend list is), unlinking.
///
/// The screen itself knows nothing of the emulator: what it shows and does comes through
/// Backend, which title_menu.cpp fills in. (A test program on a computer fills it with made-up
/// states and draws every screen.)
namespace OnlineMenu {

/// What is on the card (Link3ds::Read and Installed).
struct LinkState {
    bool linked = false;
    bool old_complete = false, new_complete = false;
    unsigned old_installed = 0, old_total = 0;
    unsigned new_installed = 0, new_total = 0;
};

/// What a setup is ready to start with.
struct Prepared {
    std::string program; ///< the path to load, up to the address
    std::string note;    ///< something the person should know first (the DNS in use); may be empty
};

struct Backend {
    /// Slow (hundreds of small files): asked when the screen opens and after a change.
    std::function<LinkState()> read;
    /// The 3DS's address as last typed; empty: none yet.
    std::function<std::string()> address;
    /// Looks for 3DS consoles running the setup tool on this network (about 1.5 s): their
    /// addresses. The connections made to them are held (tool_scan.h) until choose or drop.
    /// note: why nothing could be looked for, when that is so.
    std::function<std::vector<std::string>(const std::string& remembered, std::string& note)> find;
    /// The address to link from, found or typed: remembered, and (found) its connection kept
    /// for the setup, the others ended. False with error: not an address.
    std::function<bool(const std::string& address, bool typed, std::string& error)> choose;
    /// Nothing chosen: what the search holds is ended.
    std::function<void()> drop;
    /// Asks for the address. Nothing: cancelled.
    std::function<std::optional<std::string>(const std::string& initial)> ask_address;
    /// Checks and prepares a setup (Link3ds::Begin without an address). False with error in
    /// words for the person.
    std::function<bool(bool new_set, bool reinstall, Prepared& prepared, std::string& error)> prepare;
    std::function<void()> unlink;
    /// The online system modules' switch. forced: a launch option holds it on.
    std::function<bool()> modules_on;
    std::function<void(bool on)> set_modules;
    bool modules_forced = false;
    /// The installed System Settings and HOME Menu of the linked console's region; empty: not
    /// there.
    std::function<std::string()> settings_path, home_path;
    /// A number that changes when titles are installed or removed: what read and the two paths
    /// gave is kept for the program's run while it stands (they look at about 400 files).
    std::function<unsigned()> title_changes;
};

struct Outcome {
    bool exit = false;      ///< leave the app
    std::string start_name; ///< with start_path: start this (a setup's program, or a system title)
    std::string start_path;
};

/// The screen, until the person goes back, leaves the app or starts something.
Outcome Run(const Backend& backend);

} // namespace OnlineMenu
