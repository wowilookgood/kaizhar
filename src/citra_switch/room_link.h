// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

/// Local wireless play through an Azahar multiplayer room: the emulated console is a member of
/// a room, and the titles' local wireless traffic (NWM_UDS) goes through it. The room is one
/// that desktop Azahar, its dedicated server or another Kaizhar hosts, or one this console
/// hosts itself. The title list's Multiplayer screen sets it up (title_menu.cpp); for a start
/// without the menu:
///
///   /switch/Kaizhar/multiplayer.txt, lines of key=value:
///       enabled=1
///       address=192.168.1.20      (a name or an IPv4 address)
///       port=24872                (optional; 24872 is the rooms' default)
///       nickname=Player           (4 to 20 of: letters, digits, space . _ -)
///       password=...              (optional)
///       host=1                    (host a room on this console, on port, and join it)
///       lobby=https://...         (a lobby server: the Multiplayer screen lists its rooms)
///       online_lle=1              (the system modules for online play as the console's own code)
///       online_log=full           (the log file's online record shows addresses whole)
///       link_address=192.168.1.30 (the 3DS last linked from; the Online play screen keeps it)
///       dns=1.1.1.1,8.8.8.8       (name servers for the emulated console during "Link a 3DS",
///                                  guest_dns.h; these two are the default)
///
///   or, for one start, on the command line (it wins over the file):
///       --room nick[:password]@address[:port]
///       --host-room[=port]
///
/// The Multiplayer screen keeps the nickname and the rooms joined last in
/// /switch/Kaizhar/rooms.txt.
namespace RoomLink {

/// A room joined before, for joining again.
struct Recent {
    std::string address;
    u16 port = 0;
    std::string password;
};

/// What the Multiplayer screen shows.
struct Status {
    bool hosting = false;      ///< a room runs on this console
    u16 host_port = 0;
    unsigned host_members = 0; ///< members of that room, this console among them
    std::string own_address;   ///< this console's address in the network; empty without one
    bool wanted = false;       ///< a room is to be joined (when the next title starts, if not yet)
    bool connected = false;    ///< in a room now
    bool own_room = false;     ///< ...the one this console hosts
    std::string address;
    u16 port = 0;
    std::string nickname;             ///< as set; the room may know this console by another
    std::string room_name;            ///< as the room gave it
    std::vector<std::string> members; ///< "nickname (title)", as the room last listed them
    std::string last;                 ///< how the last try or session went; empty: nothing to say
};

/// Reads the files, then the command-line forms (each may be null; host: "" for the default
/// port).
void Configure(const char* room_argument, const char* host_argument = nullptr);

/// Whether a room is to be joined.
bool Enabled();

/// The lobby server named in the file ("lobby=", http:// or https://); empty: none.
std::string LobbyUrl();
/// Whether the file asks for the online system modules as the console's own code.
bool OnlineLle();
/// The 3DS last linked from, and the name servers the file names for guest_dns.h; empty: none.
std::string LinkAddress();
std::string DnsServers();
/// Keeps a setting of the Online play screen ("online_lle", "link_address") in the file; its
/// other lines stay as they are. False: not such a key, or the file couldn't be written.
bool SetSetting(const std::string& key, const std::string& value);

/// Once, before the first title loads: the emulator's wireless service looks for the room
/// client when a title starts and never again. Starts the room this console is to host.
void Start(Core::System& system);

/// Before a title loads: joins the wanted room unless this console is in it already. Waits for
/// the room's answer, a few seconds at most; a room that can't be reached doesn't keep the title
/// from starting.
void Join(Core::System& system);

/// When a title has ended. staying (back to the title list): this console stays in its room.
/// Otherwise it leaves: ends the client's thread and closes its socket.
void TitleEnded(bool staying);

/// At exit: leaves, ends a hosted room and its thread.
void Stop();

// The Multiplayer screen. Joining and hosting return once the room has answered or the try has
// failed (up to about 8 s for a room that doesn't answer); Status::last then says how it went.

/// Hosts a room on this console (the default port for 0) and joins it.
bool Host(Core::System& system, u16 port = 0);
void StopHosting();
/// Joins that room now, and from then on when a title starts; a room joined is kept in the list
/// of recent ones.
bool JoinRoom(Core::System& system, const std::string& address, u16 port, const std::string& password);
/// Leaves, and no room is wanted any more.
void LeaveRoom();

/// "address[:port]" as a person types it. False: no address, or not a port.
bool ParseAddress(const std::string& text, std::string& address, u16& port);
/// The rooms' rule for nicknames: 4 to 20 of letters, digits, space . _ -
bool NicknameFits(const std::string& nickname);
/// False (and nothing changes) for a nickname the rooms wouldn't take.
bool SetNickname(const std::string& nickname);

std::vector<Recent> Recents();
Status GetStatus();

/// A room found on the local network.
struct Found {
    std::string address;
    u16 port = 0;
    bool described = false; ///< a console running Kaizhar hosts it and has said what follows
    std::string name;
    unsigned members = 0, slots = 0;
};

/// Looks for rooms on the network this console is in: every address of its subnet (254 at
/// most) is asked on the rooms' default port, and whatever answers as a room does is listed.
/// Rooms say nothing about themselves before a member has joined, so one hosted by desktop
/// Azahar is an address and no more; a console running Kaizhar that hosts also answers a small
/// question of its own (name, members, port), whatever port its room is on. Takes about a
/// second. note: why nothing could be looked for, when that is so. port: 0 for the default.
std::vector<Found> FindRooms(std::string& note, u16 port = 0);

/// One line for the title list: the room and how things stand. Empty when no room is in use.
std::string StatusLine();

/// A step of a title's end or of the exit, for a run that stops without a word: a line
/// "hen: room: step: ..." in the log, flushed, and (development builds) the same line in
/// /switch/Kaizhar/room-exit.log, on the card before this returns: the log connection can't
/// tell a program that was ended from outside from one that died. Nothing without a room.
void Step(const char* text);

/// Every few seconds while a title runs: a line in room-exit.log (development builds), so that
/// the file's last line says how long the run loop lived.
void Alive();

} // namespace RoomLink

/// Step, for the emulator core (the wireless and download play services' ends).
extern "C" void hen_room_step(const char* text);
/// A wireless frame the emulated console sent to the room (type: Network::WifiPacket's; sent 0:
/// it had nowhere to go, the console is in no room). Counted for the log.
extern "C" void hen_room_frame_sent(int type, int sent);
/// What happened to a wireless network of the emulated console: a "hen: room: wireless:" line.
extern "C" void hen_room_note(const char* text);
