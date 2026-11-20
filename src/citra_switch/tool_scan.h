// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>

/// Looks for a 3DS running the Artic Setup Tool on the network this console is in, for "Link a
/// 3DS" (link_3ds.h): which addresses accept a TCP connection on the tool's port, 5543.
///
/// The tool serves one client, and by its source a client that just goes away leaves it
/// reading from the closed connection for ever: it never listens again until its server is
/// restarted on the 3DS (Y). So a connection the search makes is not closed: it is held, the
/// one to the address chosen becomes the setup's own connection (Keep, then the emulator's
/// client asks Take), and any other is ended as the setup's client ends one, with the
/// protocol's "$STOP" request, after which the tool listens again. Whatever else listens on
/// that port is listed too (the setup's connection then says what it is). remembered,
/// if not empty, is tried first by itself (a third of a second); only if it doesn't answer is
/// every address of this console's network (x.y.z.1 to .254) tried, in groups, about 1.5 s in
/// all. Connections are started without blocking and each socket is asked directly whether it
/// has a peer; groups get smaller when the system has no room for a whole one. Where sockets
/// can't be made non-blocking, a few threads make blocking connections with the socket's own
/// time limit, and all are joined. Every socket but those of the addresses that answered is
/// closed before this returns. A second log line says what each socket call did.
namespace ToolScan {

constexpr unsigned short Port = 5543;

/// The addresses that answered, remembered first. note: why nothing could be looked for (no
/// network address), when that is so.
std::vector<std::string> Find(const std::string& remembered, std::string& note);

/// The address chosen: its connection stays for the setup, the others are ended.
void Keep(const std::string& address);
/// Whether a connection to that address is held.
bool Holds(const std::string& address);
/// Connects to that address (an IPv4 address) and holds the connection: tried every half
/// second for total_ms, since a tool that has just ended a session listens again only after a
/// moment. False: no answer in that time, or not an address.
bool Hold(const std::string& address, int total_ms);
/// Nothing chosen, or the setup is over: every connection still held is ended.
void Drop();
/// For the emulator's setup client (network/artic_base, hen_artic_socket): the held connection
/// to that address, which is the caller's from then on; -1: none, connect as usual.
int Take(const char* address, unsigned port);

} // namespace ToolScan
