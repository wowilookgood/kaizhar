// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

/// Name lookups for the emulated console while "Link a 3DS" runs (link_3ds.h).
///
/// The setup makes the emulated console download its system software from Nintendo's update
/// servers. Many consoles this program runs on use a DNS that keeps Nintendo's names away (it
/// answers 127.0.0.1, or nothing), and that setting is the owner's to keep. So for the time of
/// a setup, and only for names the emulated console asks for (its HTTP service and its socket
/// service's lookups: core/hle/service/http, soc), a name the console's DNS gives no usable
/// address for is asked of a public name server directly: one plain DNS question over UDP.
/// Nothing of the console's settings changes, and nothing else in this program resolves this
/// way: rooms, the lobby and the connection to the 3DS use the console's DNS as always, and so
/// does the emulated console in normal play.
///
/// The name servers: 1.1.1.1, then 8.8.8.8; a line dns=a.b.c.d[,a.b.c.d] in
/// /switch/Kaizhar/multiplayer.txt names others.
namespace GuestDns {

enum class How {
    None,   ///< no usable address from anywhere
    System, ///< the console's DNS answered
    Own,    ///< the console's DNS didn't (blocked or failed); a name server asked directly did
};

struct Answer {
    How how = How::None;
    u32 address = 0;    ///< IPv4, in network byte order
    std::string text;   ///< the same, dotted
    std::string server; ///< Own: the name server that answered
    std::string system; ///< what the console's DNS said: an address, or "nothing"
};

/// The name servers to ask ("a.b.c.d,a.b.c.d"); empty or nothing usable in it: the defaults.
void SetServers(const std::string& list);
std::vector<std::string> Servers();

/// The console's DNS first, then the name servers. Blocks: up to about 2 s per name server.
/// Not cached.
Answer Resolve(const std::string& name);

/// One question to one name server (IPv4 address of the name, record type A). False: no
/// answer in time, no such name, or an answer that is no address of another machine.
bool Ask(const std::string& server, const std::string& name, u32& address, int timeout_ms = 2000);

/// On: the emulated console's lookups go through Resolve (answers are kept until Off).
/// Off: they are the core's own again, and the names resolved by a name server are listed in
/// the log. Only around a setup's run, from the thread that loads and shuts down the title.
void Enable(bool on);

/// The names a name server was asked for since the last Enable(true), as "name -> address
/// (server)".
std::vector<std::string> Resolved();

} // namespace GuestDns
