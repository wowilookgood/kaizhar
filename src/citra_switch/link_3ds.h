// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <string>

#include "common/common_types.h"
#include "core/core.h"

/// "Link a 3DS": what desktop Azahar calls File > Set Up System Files. The user's own console
/// runs the Artic Setup Tool (a homebrew program that shows an address); the emulator connects
/// to it and
///   1. copies the console's identity: SecureInfo_A, LocalFriendCodeSeed_B, movable.sed and
///      otp.bin (nand/rw/sys, nand/private, sysdata), the console ID and the MAC address;
///   2. starts the console's own System Settings and network installer inside the emulator,
///      on the "system update" screen. The user confirms the update there, and the emulated
///      console downloads its system software from Nintendo's update servers into nand/.
/// Nothing but step 2 puts the system software there, and online play (Nintendo Network's
/// replacement, Pretendo) needs both. It is done once per set: the Old 3DS set first, then the
/// New 3DS set (either works with either kind of console).
///
/// Step 2 is an emulated title like any other: it is started by loading the path Begin gives,
/// through the frontend's usual load and run loop, and it ends when the emulated console asks
/// to restart. Run does all of it given a function that runs one title.
///
/// It needs HTTPS (the update servers), so a build without it refuses in Begin. A console
/// whose DNS keeps Nintendo's servers away can still do it: while a setup runs, the emulated
/// console's name lookups that the console's DNS doesn't answer are asked of a public name
/// server (guest_dns.h, switched on by the frontend around the run). Begin checks that one of
/// the two ways finds the update server and refuses if neither does.
namespace Link3ds {

enum class Set { Old3ds, New3ds };

struct State {
    bool console_linked = false; ///< the console's identity files are here and valid
    bool old3ds_installed = false;
    bool new3ds_installed = false;
};

/// What is on the card now. Reads a few hundred small files: not for every frame.
State Read();

/// How far the download is: system titles of the set that are installed, of how many. The
/// region is the linked console's; before step 1 has run, total is 0.
struct Count {
    u32 installed = 0;
    u32 total = 0;
};
Count Installed(Set set);

/// Read and both sets' Installed in one go, for the online setup screen: the same
/// answers, from the folder names of the title folder and the saved list of installed titles
/// (Service::AM::HenReadNandTitles) instead of a look at every system title of every region on
/// the card, which was seconds on a full card. The counts stay 0 when the console is not linked.
State ReadWithCounts(Count& old_set, Count& new_set);

/// One line for the screen or the log whenever something worth showing happens.
using Progress = std::function<void(const std::string& line)>;

/// Checks that the setup can run and prepares it: the address ("192.168.1.30", optionally with
/// ":port"; the tool's default is 5543), HTTPS, the order of the sets, the name lookup of the
/// update servers. A set that is already installed is refused unless reinstall is given (ask
/// the user first: Read tells); then its titles are removed and installed afresh, as on the
/// desktop. On success, program is the path to load. On failure, error says why in words for
/// the user, and nothing on the card has changed.
bool Begin(const std::string& address, Set set, bool reinstall, std::string& program,
           std::string& error, const Progress& progress = {});

/// Without an address ("") Begin does all but the address: program is then the path's start,
/// to which the address is added once it is known (the frontend looks for the 3DS last, so
/// that the connection its search makes is still fresh when the setup takes it over).
/// AddressOk: whether a typed address is one Begin would take.
bool AddressOk(const std::string& address);

/// Whether a path is one that Begin gave (the frontend skips what only applies to games:
/// the room, movies, the cartridge file).
bool IsSetupProgram(const std::string& program);
/// After a Begin that succeeded: a sentence for the person if the console's DNS didn't find
/// the update server and guest_dns.h's lookups will be used; empty otherwise.
std::string DnsNote();
/// Which set such a path is for.
Set SetOf(const std::string& program);

/// After the title has ended: what the card holds now, and in error, if the set is not
/// complete, the likely reason given how the run ended. True when the set is complete.
bool End(Set set, Core::System::ResultStatus last_status, const std::string& status_details,
         std::string& error, const Progress& progress = {});

/// The emulated console's country has to be one of the linked console's region, or the setup
/// stops after step 1 (and online services refuse later). If it isn't, sets the region's usual
/// country (United States for an American console, and so on) and returns true. Only between
/// titles; does nothing before a console is linked. The user's own choice, once there is a
/// settings screen for it, is kept as long as it fits the region.
bool FixCountry(Core::System& system, const Progress& progress = {});

/// The whole setup. run_title loads the given path, runs it until the emulated system stops
/// and returns the last status with its details (the frontend's title loop). A first setup
/// whose step 2 was refused because of the country is run a second time with the country
/// fixed: the user sees the connection made twice.
struct RunResult {
    Core::System::ResultStatus status = Core::System::ResultStatus::Success;
    std::string details;
};
using RunTitle = std::function<RunResult(const std::string& program)>;
bool Run(Core::System& system, const std::string& address, Set set, bool reinstall,
         const RunTitle& run_title, std::string& error, const Progress& progress = {});

/// The card as before the first link, for testing a first link again: both sets' system titles
/// removed, the console's identity files (movable.sed too) and the savegames made with them
/// removed, the emulated console's country back to the shipped one. Each step is logged.
void Fresh(Core::System& system);

/// Forgets the console: its identity files and the account, friends and installer saves made
/// with them. The system titles stay. (Desktop: the System tab's "Unlink".)
void Unlink();

/// Whether the console's DNS keeps Nintendo's update servers away (a blocking DNS
/// answers with 127.0.0.1 or not at all): a setup then uses guest_dns.h's lookups. Not a
/// refusal by itself. Makes one name lookup; may take seconds.
bool UpdateServersBlocked();

} // namespace Link3ds
