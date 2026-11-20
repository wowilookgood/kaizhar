// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

/// The public room list: the rooms a lobby server announces, for a menu to pick one from.
///
/// A lobby is the web service that desktop Azahar's "Browse Public Game Lobby" asks
/// (GET <url>/lobby, no account needed). Azahar ships without an address for one, so there is
/// no list until the user names a lobby:
///
///   /switch/Kaizhar/multiplayer.txt:
///       lobby=https://lobby.example.org      (http:// works too)
///
/// (room_link.h reads that file: RoomLink::LobbyUrl.)
///
/// Joining a listed room is the same as joining one by address (room_link.h): the list only
/// supplies the address, the port and whether a password is asked.
namespace RoomList {

struct Entry {
    std::string name;
    std::string description;
    std::string owner;          ///< the host's account name on the lobby; may be empty
    std::string address;        ///< a name or an IPv4 address
    u16 port = 0;
    u32 players = 0;
    u32 max_players = 0;
    bool has_password = false;
    std::string preferred_game; ///< empty: none named
    u64 preferred_game_id = 0;
    std::vector<std::string> nicknames; ///< who is in the room
};

struct Result {
    bool ok = false;
    std::string error;        ///< what a person would call the failure; empty when ok
    std::vector<Entry> rooms; ///< rooms this build can join, fullest first
    u32 other_version = 0;    ///< rooms left out: another room protocol version
};

/// Asks the lobby. Blocks until it has answered or about ten seconds have passed without one.
/// Sockets must be up (the frontend initializes them at start).
Result Fetch(const std::string& lobby_url);

/// The answer's text to rooms, as Fetch does it (apart, so it can be tried without a network).
Result Parse(const std::string& json_text);

} // namespace RoomList
