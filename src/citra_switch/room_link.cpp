// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <enet/enet.h>
#include <fmt/format.h>

#include "citra_switch/room_link.h"
#include "citra_switch/switch_platform.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/hle/service/cfg/cfg.h"
#include "network/network.h"
#include "network/room.h"
#include "network/room_member.h"
#include "network/verify_user.h"

extern "C" int hen_online_log_full; // core/hle/service/soc/soc_u.cpp

namespace RoomLink {

// Wireless frames by type (Network::WifiPacket::PacketType: beacon, data, authentication,
// association response, deauthentication, node map) since the last line about them: received
// from the room, sent to it, and wanted sent while in no room.
constexpr int FrameTypes = 6;
std::atomic<unsigned> frames_in[FrameTypes], frames_out[FrameTypes], frames_unsent;
/// Times another console connected to this one's network as a spectator (type -1): how a
/// console that looks for games reads what this one hosts.
std::atomic<unsigned> spectators;

namespace {

// (a test program on a computer builds this file too: its files go to the folder it runs in)
#ifdef __SWITCH__
#define ROOM_FOLDER "/switch/Kaizhar/"
#else
#define ROOM_FOLDER "./"
#endif
constexpr const char* ConfigFile = ROOM_FOLDER "multiplayer.txt";
/// The Multiplayer screen's: nickname=..., and room=[password@]address:port, the last joined first.
constexpr const char* RecentsFile = ROOM_FOLDER "rooms.txt";
constexpr std::size_t RecentsKept = 5;
/// How long a join waits for the room's answer once the connection stands.
constexpr int AnswerWaitMs = 3000;
/// The room this console hosts.
constexpr const char* HostedName = "Kaizhar room";
constexpr u32 HostedSlots = 8;
constexpr const char* Loopback = "127.0.0.1";
/// Where a console that hosts answers FindRooms' question (UDP, beside the rooms' default
/// port), and the question and the answer: "KZR?" and "KZR!" with the room's port (two bytes,
/// high first), members, slots, and the room's name.
constexpr u16 AnswerPort = 24873;
constexpr char Question[4] = {'K', 'Z', 'R', '?'};
constexpr char Answer[4] = {'K', 'Z', 'R', '!'};
/// How long FindRooms listens.
constexpr int FindMs = 1000;

struct Config {
    bool enabled = false; ///< a room is to be joined
    bool own = false;     ///< ...the one this console hosts
    std::string address;
    u16 port = Network::DefaultRoomPort;
    std::string nickname = "Kaizhar";
    std::string password;
    bool by_menu = false; ///< chosen in the Multiplayer screen: given up once it doesn't answer
    std::string lobby;       ///< a lobby server's address; empty: none
    bool online_lle = false; ///< main.cpp's to act on
    std::string link_address; ///< the 3DS last linked from (the Online play screen)
    std::string dns;          ///< the name servers of guest_dns.h, if the user names their own
    bool host = false; ///< host a room at the start
    u16 host_port = Network::DefaultRoomPort;
};
Config config;
std::vector<Recent> recents;

bool started = false; ///< Network::Init succeeded
bool hosting = false; ///< the room of this console is open
std::thread answerer;             ///< answers FindRooms' question while this console hosts
std::atomic<bool> answering{false};
std::string last;     ///< how the last try or session went, for the menu

// Written by the room client's thread (the callbacks below), read by the emulator's.
constexpr int NoError = -1;
std::atomic<int> error{NoError}; ///< the last Network::RoomMember::Error
std::atomic<bool> joined{false}; ///< the room accepted us at some point of this session
std::atomic<bool> lost{false};   ///< ...and the connection ended before we left
std::atomic<unsigned> members{0};
std::atomic<unsigned long long> beacons{0}, data_frames{0}, other_frames{0};
std::mutex list_mutex;                 ///< for the three below
std::string member_lines;              ///< the member list as last printed
std::string room_name;                 ///< as the room gave it
std::vector<std::string> member_names; ///< "nickname (title)"

// The callbacks stay bound from Start to Stop.
Network::RoomMember::CallbackHandle<Network::RoomMember::State> on_state;
Network::RoomMember::CallbackHandle<Network::RoomMember::Error> on_error;
Network::RoomMember::CallbackHandle<Network::RoomInformation> on_information;
Network::RoomMember::CallbackHandle<Network::WifiPacket> on_packet;
Network::RoomMember::CallbackHandle<Network::ChatEntry> on_chat;
Network::RoomMember::CallbackHandle<Network::StatusMessageEntry> on_status;

/// Whether anything of this file is in use: the steps and the card's trace are for those runs.
bool InUse() {
    return started && (config.enabled || hosting);
}

#if HEN_DEV
constexpr const char* StepFile = ROOM_FOLDER "room-exit.log";
uint64_t step_start; ///< sw_time_ms at Start

/// One line of StepFile, on the card when this returns. (open and write themselves: the
/// program's fopen and fwrite put work off to a thread, switch_file_defer.c)
void StepLine(const char* text, bool fresh = false) {
    const int fd = open(StepFile, O_WRONLY | O_CREAT | (fresh ? O_TRUNC : O_APPEND), 0666);
    if (fd < 0) {
        return;
    }
    char line[256];
    const int length = std::snprintf(line, sizeof(line), "%8llu ms: %s\n",
                                     static_cast<unsigned long long>(sw_time_ms() - step_start), text);
    if (length > 0) {
        (void)!write(fd, line, std::min<std::size_t>(static_cast<std::size_t>(length), sizeof(line) - 1));
    }
    fsync(fd);
    close(fd);
}
#endif

/// A failure: in the log connection like everything else here, and in the emulator's log file
/// (/switch/Kaizhar/log), which is all a release build started from the homebrew menu leaves.
void Fail(const std::string& text) {
    std::printf("hen: room: %s\n", text.c_str());
    LOG_ERROR(Network, "room: {}", text);
}

std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/// What a person would call the error (the title list shows it).
const char* ErrorText(Network::RoomMember::Error e) {
    using Error = Network::RoomMember::Error;
    switch (e) {
    case Error::LostConnection:
        return "lost the connection";
    case Error::HostKicked:
        return "kicked by the host";
    case Error::UnknownError:
        return "could not start a connection";
    case Error::NameCollision:
        return "nickname taken or not allowed";
    case Error::MacCollision:
        return "MAC address already in the room";
    case Error::ConsoleIdCollision:
        return "console ID already in the room";
    case Error::WrongVersion:
        return "room version differs";
    case Error::WrongPassword:
        return "wrong password";
    case Error::CouldNotConnect:
        return "room did not answer";
    case Error::RoomIsFull:
        return "room is full";
    case Error::HostBanned:
        return "banned by the host";
    case Error::PermissionDenied:
        return "permission denied";
    case Error::NoSuchUser:
        return "no such user";
    }
    return "unknown error";
}

/// key=value lines of a file; lines that are empty, start with # or have no = are skipped.
template <typename F>
bool ReadKeys(const char* file, F&& each) {
    std::string text;
    if (!FileUtil::Exists(file) || FileUtil::ReadFileToString(true, file, text) == 0) {
        return false;
    }
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = Trim(text.substr(at, end - at));
        at = end + 1;
        const std::size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) {
            continue;
        }
        each(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));
    }
    return true;
}

bool ReadPort(const std::string& text, u16& port) {
    if (text.empty() || text.size() > 5 || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    const int value = std::atoi(text.c_str());
    if (value <= 0 || value >= 65536) {
        return false;
    }
    port = static_cast<u16>(value);
    return true;
}

void ReadRecents() {
    recents.clear();
    ReadKeys(RecentsFile, [](const std::string& key, const std::string& value) {
        if (key == "nickname") {
            if (NicknameFits(value)) {
                config.nickname = value;
            }
        } else if (key == "room" && recents.size() < RecentsKept) {
            Recent recent;
            const std::size_t at = value.rfind('@');
            if (at != std::string::npos) {
                recent.password = value.substr(0, at);
            }
            if (ParseAddress(value.substr(at == std::string::npos ? 0 : at + 1), recent.address, recent.port)) {
                recents.push_back(recent);
            }
        }
    });
}

void WriteRecents() {
    std::string text = "# Kaizhar's Multiplayer screen keeps this file.\n";
    text += "nickname=" + config.nickname + "\n";
    for (const Recent& recent : recents) {
        text += fmt::format("room={}{}{}:{}\n", recent.password, recent.password.empty() ? "" : "@",
                            recent.address, recent.port);
    }
    FileUtil::CreateFullPath(RecentsFile);
    if (FileUtil::WriteStringToFile(true, RecentsFile, text) == 0) {
        Fail(fmt::format("{} could not be written", RecentsFile));
    }
}

void ReadFile() {
    const bool found = ReadKeys(ConfigFile, [](const std::string& key, const std::string& value) {
        if (key == "enabled") {
            config.enabled = std::atoi(value.c_str()) != 0;
        } else if (key == "address") {
            config.address = value;
        } else if (key == "port") {
            if (ReadPort(value, config.port)) {
                config.host_port = config.port;
            }
        } else if (key == "nickname") {
            config.nickname = value;
        } else if (key == "password") {
            config.password = value;
        } else if (key == "host") {
            config.host = std::atoi(value.c_str()) != 0;
        } else if (key == "lobby") {
            config.lobby = value;
        } else if (key == "online_lle") {
            config.online_lle = std::atoi(value.c_str()) != 0;
        } else if (key == "online_log") {
            // full: the online record in the log file shows other networks' addresses whole
            hen_online_log_full = value == "full";
        } else if (key == "link_address") {
            config.link_address = value;
        } else if (key == "dns") {
            config.dns = value;
        } else {
            std::printf("hen: room: %s: unknown key \"%s\"\n", ConfigFile, key.c_str());
        }
    });
    if (found) {
        std::printf("hen: room: %s: enabled %d, host %d, address \"%s\", port %u, nickname \"%s\", "
                    "password %s\n",
                    ConfigFile, config.enabled ? 1 : 0, config.host ? 1 : 0, config.address.c_str(),
                    config.port, config.nickname.c_str(), config.password.empty() ? "none" : "set");
    }
}

/// nick[:password]@address[:port]
bool ReadArgument(const std::string& argument) {
    const std::size_t at = argument.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 >= argument.size()) {
        return false;
    }
    const std::string who = argument.substr(0, at);
    const std::size_t colon = who.find(':');
    config.nickname = who.substr(0, colon);
    config.password = colon == std::string::npos ? std::string{} : who.substr(colon + 1);
    if (!ParseAddress(argument.substr(at + 1), config.address, config.port)) {
        return false;
    }
    config.enabled = true;
    return true;
}

std::string MacText(const Network::MacAddress& mac) {
    return Service::CFG::MacToString(mac);
}

// The callbacks. They run on the room client's thread (or, for a connection that never came
// about, inside Join), and may not bind or unbind anything.

void OnState(const Network::RoomMember::State& state) {
    std::printf("hen: room: state: %s\n", Network::GetStateStr(state));
#if HEN_DEV
    if (InUse()) {
        StepLine(Network::GetStateStr(state));
    }
#endif
    if (state == Network::RoomMember::State::Joined || state == Network::RoomMember::State::Moderator) {
        joined = true;
        if (const auto member = Network::GetRoomMember().lock()) {
            std::printf("hen: room: joined as \"%s\", MAC %s\n", member->GetNickname().c_str(),
                        MacText(member->GetMacAddress()).c_str());
        }
    }
}

void OnError(const Network::RoomMember::Error& e) {
    error = static_cast<int>(e);
    if (e == Network::RoomMember::Error::LostConnection || e == Network::RoomMember::Error::HostKicked ||
        e == Network::RoomMember::Error::HostBanned) {
        lost = true;
    }
    Fail(fmt::format("error: {} ({})", Network::GetErrorStr(e), ErrorText(e)));
}

void OnInformation(const Network::RoomInformation& information) {
    const auto member = Network::GetRoomMember().lock();
    if (!member) {
        return;
    }
    const auto& list = member->GetMemberInformation();
    members = static_cast<unsigned>(list.size());
    std::string lines = fmt::format("room \"{}\", {} of {} members:", information.name, list.size(),
                                    information.member_slots);
    std::vector<std::string> names;
    for (const auto& entry : list) {
        lines += fmt::format(" [{} {} {}]", entry.nickname, MacText(entry.mac_address),
                             entry.game_info.name.empty() ? "no title" : entry.game_info.name);
        names.push_back(entry.game_info.name.empty()
                            ? entry.nickname
                            : fmt::format("{} ({})", entry.nickname, entry.game_info.name));
    }
    std::lock_guard lock(list_mutex);
    room_name = information.name;
    member_names = std::move(names);
    // (the room sends the list again for every change of anyone's title: only what differs)
    if (lines != member_lines) {
        member_lines = lines;
        std::printf("hen: room: %s\n", lines.c_str());
    }
}

void OnPacket(const Network::WifiPacket& packet) {
    const int type = static_cast<int>(packet.type);
    if (type >= 0 && type < FrameTypes) {
        frames_in[type]++;
    }
    switch (packet.type) {
    case Network::WifiPacket::PacketType::Beacon:
        beacons++;
        break;
    case Network::WifiPacket::PacketType::Data:
        data_frames++;
        break;
    default:
        other_frames++;
        break;
    }
}

void OnChat(const Network::ChatEntry& chat) {
    std::printf("hen: room: chat: %s: %s\n", chat.nickname.c_str(), chat.message.c_str());
}

void OnStatus(const Network::StatusMessageEntry& status) {
    const char* what = "?";
    switch (status.type) {
    case Network::IdMemberJoin:
        what = "joined";
        break;
    case Network::IdMemberLeave:
        what = "left";
        break;
    case Network::IdMemberKicked:
        what = "was kicked";
        break;
    case Network::IdMemberBanned:
        what = "was banned";
        break;
    case Network::IdAddressUnbanned:
        what = "was unbanned";
        break;
    }
    std::printf("hen: room: member %s %s\n", status.nickname.c_str(), what);
}


/// Ends the connection, if there is one, and says how the session went.
void Disconnect() {
    const auto member = Network::GetRoomMember().lock();
    const bool was_connected = member->IsConnected();
    const uint64_t leave_start = sw_time_ms();
    Step(was_connected ? "leaving the room (connected)" : "leaving the room (not connected)");
    member->Leave(); // (also ends a thread that has stopped by itself, and closes the socket)
    Step("left the room");
    const unsigned long long leave_ms = was_connected ? sw_time_ms() - leave_start : 0;
    unsigned long long io[6];
    sw_room_io_stats(io);
    const int e = error;
    std::printf("hen: room: summary: %s%s%s; datagrams sent %llu (%llu bytes), received %llu (%llu bytes), "
                "failed %llu/%llu; wireless frames received: %llu beacons, %llu data, %llu other; "
                "%u member(s) at the end; leaving took %llu ms\n",
                joined ? "joined" : "never joined", e != NoError ? ", last error " : "",
                e != NoError ? Network::GetErrorStr(static_cast<Network::RoomMember::Error>(e)) : "",
                io[0], io[1], io[2], io[3], io[4], io[5], beacons.load(), data_frames.load(),
                other_frames.load(), members.load(), leave_ms);
    if (joined) {
        last = fmt::format("was in the room, {} member(s), {} wireless frames received{}", members.load(),
                           beacons.load() + data_frames.load() + other_frames.load(),
                           lost ? fmt::format("; then {}", ErrorText(static_cast<Network::RoomMember::Error>(e)))
                                : std::string{});
    } else if (e != NoError) {
        last = ErrorText(static_cast<Network::RoomMember::Error>(e));
    } else if (was_connected) {
        last = "no answer from the room";
    }
    std::lock_guard lock(list_mutex);
    member_names.clear();
    room_name.clear();
}

/// One try: connects to ip and asks to join. True once the room has accepted.
bool TryJoin(const char* ip, const std::string& nickname, const Network::MacAddress& mac,
             const std::string& console_id_hash) {
    const auto member = Network::GetRoomMember().lock();
    error = NoError;
    joined = false;
    lost = false;
    std::printf("hen: room: connecting to %s port %u (UDP) as \"%s\", MAC %s, console ID hash %.8s...\n",
                ip, config.port, nickname.c_str(), MacText(mac).c_str(), console_id_hash.c_str());
    const uint64_t connect_start = sw_time_ms();
    member->Join(nickname, console_id_hash, ip, config.port, 0, mac, config.password);
    if (!member->IsConnected()) {
        // (the error callback has said why)
        std::printf("hen: room: no connection after %llu ms\n",
                    static_cast<unsigned long long>(sw_time_ms() - connect_start));
        return false;
    }
    std::printf("hen: room: connected in %llu ms; asked to join\n",
                static_cast<unsigned long long>(sw_time_ms() - connect_start));
    for (int waited = 0; waited < AnswerWaitMs && !joined && error == NoError; waited += 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!joined && error == NoError) {
        Fail(fmt::format("no answer to the join request in {} ms", AnswerWaitMs));
    }
    return joined;
}

/// Joins the wanted room. Tries again by itself where another try can help: a nickname or a MAC
/// address another member has (two consoles with the default nickname, or with copies of one
/// emulated console), and the hosted room by this console's own address where the loopback
/// address leads nowhere.
bool Connect(Core::System& system) {
    const auto member = Network::GetRoomMember().lock();
    if (member->IsConnected() || member->GetState() != Network::RoomMember::State::Idle) {
        member->Leave();
    }
    members = 0;
    beacons = data_frames = other_frames = 0;
    {
        std::lock_guard lock(list_mutex);
        member_lines.clear();
        member_names.clear();
        room_name.clear();
    }
    sw_room_io_reset();
    last.clear();

    // The address first, by itself: the client's Join would go on with 0.0.0.0 for a name that
    // doesn't resolve.
    std::string target = config.address;
    char own[16] = "";
    const bool have_own = config.own && sw_own_address(own, sizeof(own));
    char ip[48] = "";
    const auto resolve = [&]() {
        const uint64_t resolve_start = sw_time_ms();
        std::printf("hen: room: resolving %s\n", target.c_str());
        ENetAddress address{};
        if (enet_address_set_host(&address, target.c_str()) != 0 ||
            enet_address_get_host_ip(&address, ip, sizeof(ip)) != 0) {
            Fail(fmt::format("could not resolve \"{}\" ({} ms)", target, sw_time_ms() - resolve_start));
            last = "could not find that address";
            return false;
        }
        std::printf("hen: room: %s is %s (%llu ms)\n", target.c_str(), ip,
                    static_cast<unsigned long long>(sw_time_ms() - resolve_start));
        return true;
    };
    if (!resolve()) {
        return false;
    }

    // As the other frontends join: the emulated console's own MAC address as the one asked
    // for, and the hash of its ID, which the room compares between members.
    Network::MacAddress mac = Service::CFG::GetConsoleMacAddress(system);
    const std::string console_id_hash = Service::CFG::GetConsoleIdHash(system);
    std::string nickname = config.nickname;
    int name_tries = 1;
    for (int attempt = 0; attempt < 6; attempt++) {
        if (TryJoin(ip, nickname, mac, console_id_hash)) {
            return true;
        }
        const int e = error;
        member->Leave(); // (the thread of a try the room refused)
        if (e == static_cast<int>(Network::RoomMember::Error::NameCollision) && NicknameFits(config.nickname) &&
            name_tries < 4) {
            nickname = fmt::format("{}-{}", config.nickname.substr(0, 18), ++name_tries);
            std::printf("hen: room: that nickname is taken: trying \"%s\"\n", nickname.c_str());
        } else if (e == static_cast<int>(Network::RoomMember::Error::MacCollision) &&
                   mac != Network::NoPreferredMac) {
            mac = Network::NoPreferredMac; // (the room then gives one out)
            std::printf("hen: room: that MAC address is in the room already: asking the room for one\n");
        } else if (e == static_cast<int>(Network::RoomMember::Error::CouldNotConnect) && have_own &&
                   target != own) {
            target = own;
            std::printf("hen: room: no answer at the loopback address: trying this console's own\n");
            if (!resolve()) {
                break;
            }
        } else {
            break;
        }
    }
    const int e = error;
    last = e != NoError ? ErrorText(static_cast<Network::RoomMember::Error>(e)) : "no answer from the room";
    return false;
}

void Remember(const std::string& address, u16 port, const std::string& password) {
    recents.erase(std::remove_if(recents.begin(), recents.end(),
                                 [&](const Recent& r) { return r.address == address && r.port == port; }),
                  recents.end());
    recents.insert(recents.begin(), Recent{address, port, password});
    if (recents.size() > RecentsKept) {
        recents.resize(RecentsKept);
    }
    WriteRecents();
}

/// While this console hosts: answers the question other consoles' FindRooms sends. Its own
/// socket and port: nothing of the room's.
void AnswerLoop(u16 room_port) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in here{};
    here.sin_family = AF_INET;
    here.sin_addr.s_addr = htonl(INADDR_ANY);
    here.sin_port = htons(AnswerPort);
    if (fd < 0 || bind(fd, reinterpret_cast<const sockaddr*>(&here), sizeof(here)) != 0) {
        std::printf("hen: room: no answers to other consoles' search (UDP port %u: errno %d)\n", AnswerPort, errno);
        if (fd >= 0) {
            close(fd);
        }
        return;
    }
    while (answering) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        timeval timeout{0, 250000};
        if (select(fd + 1, &readable, nullptr, nullptr, &timeout) <= 0) {
            continue;
        }
        char question[16];
        sockaddr_in from{};
        socklen_t from_length = sizeof(from);
        const ssize_t got = recvfrom(fd, question, sizeof(question), 0, reinterpret_cast<sockaddr*>(&from), &from_length);
        if (got != sizeof(Question) || std::memcmp(question, Question, sizeof(Question)) != 0) {
            continue;
        }
        unsigned char answer[8 + 64];
        std::memcpy(answer, Answer, sizeof(Answer));
        answer[4] = static_cast<unsigned char>(room_port >> 8);
        answer[5] = static_cast<unsigned char>(room_port);
        const auto room = Network::GetRoom().lock();
        answer[6] = room ? static_cast<unsigned char>(room->GetRoomMemberList().size()) : 0;
        answer[7] = HostedSlots;
        const std::size_t name_length = std::min<std::size_t>(std::strlen(HostedName), 64);
        std::memcpy(answer + 8, HostedName, name_length);
        sendto(fd, answer, 8 + name_length, 0, reinterpret_cast<const sockaddr*>(&from), from_length);
    }
    close(fd);
}

void StopAnswering() {
    answering = false;
    if (answerer.joinable()) {
        answerer.join();
    }
}

/// Opens the room of this console. False: the port could not be had.
bool OpenRoom(u16 port) {
    const auto room = Network::GetRoom().lock();
    if (hosting) {
        return true;
    }
    if (port == 0) {
        port = Network::DefaultRoomPort;
    }
    // Not listed anywhere, no password, and nobody's name is checked (the backend that accepts
    // everyone: the room asks it about every member).
    if (!room->Create(HostedName, "", "", port, "", HostedSlots, "", "", 0,
                      std::make_unique<Network::VerifyUser::NullBackend>())) {
        Fail(fmt::format("could not open a room on port {} (UDP)", port));
        last = fmt::format("could not open a room on port {}", port);
        return false;
    }
    hosting = true;
    answering = true;
    answerer = std::thread(AnswerLoop, port);
    config.host_port = port;
    config.enabled = true;
    config.own = true;
    config.address = Loopback;
    config.port = port;
    config.password.clear();
    char own[16] = "";
    std::printf("hen: room: hosting \"%s\" on port %u (UDP), %u members at most; this console's address: %s\n",
                HostedName, port, HostedSlots, sw_own_address(own, sizeof(own)) ? own : "none");
#if HEN_DEV
    StepLine("hosting");
#endif
    return true;
}

} // namespace

bool NicknameFits(const std::string& nickname) {
    if (nickname.size() < 4 || nickname.size() > 20) {
        return false;
    }
    for (const char c : nickname) {
        const bool fits = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == ' ' || c == '.' || c == '_' || c == '-';
        if (!fits) {
            return false;
        }
    }
    return true;
}

bool ParseAddress(const std::string& text, std::string& address, u16& port) {
    std::string where = Trim(text);
    u16 found = Network::DefaultRoomPort;
    const std::size_t colon = where.rfind(':');
    if (colon != std::string::npos) {
        if (!ReadPort(where.substr(colon + 1), found)) {
            return false;
        }
        where.resize(colon);
    }
    if (where.empty() || where.find_first_of(" \t@:") != std::string::npos) {
        return false;
    }
    address = where;
    port = found;
    return true;
}

void Configure(const char* argument, const char* host_argument) {
    ReadRecents();
    ReadFile();
    if (argument != nullptr) {
        if (ReadArgument(argument)) {
            std::printf("hen: room: --room: address \"%s\", port %u, nickname \"%s\", password %s\n",
                        config.address.c_str(), config.port, config.nickname.c_str(),
                        config.password.empty() ? "none" : "set");
        } else {
            config.enabled = false;
            Fail(fmt::format("--room {}: expected nick[:password]@address[:port]", argument));
        }
    }
    if (host_argument != nullptr) {
        config.host = true;
        if (host_argument[0] != 0 && !ReadPort(host_argument, config.host_port)) {
            Fail(fmt::format("--host-room={}: not a port; using {}", host_argument, config.host_port));
        }
        std::printf("hen: room: --host-room: port %u\n", config.host_port);
    }
    if (config.host) {
        config.enabled = false; // (Start opens the room and joins that one)
    }
    if (config.enabled && config.address.empty()) {
        config.enabled = false;
        Fail(fmt::format("{}: enabled, but no address", ConfigFile));
    }
    if ((config.enabled || config.host) && !NicknameFits(config.nickname)) {
        // (still tried: the room is what decides, and its answer names the reason)
        Fail(fmt::format("nickname \"{}\": rooms accept 4 to 20 of letters, digits, space . _ -",
                         config.nickname));
    }
}

bool Enabled() {
    return config.enabled;
}

std::string LobbyUrl() {
    return config.lobby;
}

bool OnlineLle() {
    return config.online_lle;
}

std::string LinkAddress() {
    return config.link_address;
}

std::string DnsServers() {
    return config.dns;
}

bool SetSetting(const std::string& key, const std::string& value) {
    if (key == "online_lle") {
        config.online_lle = std::atoi(value.c_str()) != 0;
    } else if (key == "link_address") {
        config.link_address = value;
    } else {
        return false;
    }
    // The file is the user's: every other line stays as it is.
    std::string text, out;
    FileUtil::ReadFileToString(true, ConfigFile, text);
    bool written = false;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const std::size_t equals = line.find('=');
        if (equals != std::string::npos && Trim(line.substr(0, equals)) == key) {
            if (!written) {
                out += key + "=" + value + "\n";
                written = true;
            }
            continue;
        }
        out += line + "\n";
    }
    if (!written) {
        out += key + "=" + value + "\n";
    }
    FileUtil::CreateFullPath(ConfigFile);
    if (FileUtil::WriteStringToFile(true, ConfigFile, out) == 0) {
        Fail(fmt::format("{} could not be written", ConfigFile));
        return false;
    }
    return true;
}

void Start(Core::System& system) {
    if (started) {
        return;
    }
    if (!Network::Init()) {
        Fail("the network library could not start");
        last = "network library could not start";
        return;
    }
    const auto member = Network::GetRoomMember().lock();
    on_state = member->BindOnStateChanged(OnState);
    on_error = member->BindOnError(OnError);
    on_information = member->BindOnRoomInformationChanged(OnInformation);
    on_packet = member->BindOnWifiPacketReceived(OnPacket);
    on_chat = member->BindOnChatMessageRecieved(OnChat);
    on_status = member->BindOnStatusMessageReceived(OnStatus);
    started = true;
#if HEN_DEV
    step_start = sw_time_ms();
    if (config.enabled || config.host) {
        StepLine("client ready", true);
    }
#endif
    if (config.enabled || config.host) {
        std::printf("hen: room: client ready (room protocol version %u)\n", Network::network_version);
    }
    if (config.host) {
        Host(system, config.host_port);
    }
}

void Join(Core::System& system) {
    if (!started || !config.enabled) {
        return;
    }
    const auto member = Network::GetRoomMember().lock();
    if (member->IsConnected() && !lost) {
        std::printf("hen: room: still in the room\n");
        return;
    }
    if (!Connect(system)) {
        Fail("the title starts without the room");
        if (config.by_menu && !config.own) {
            // (a room set up by hand or on the command line is tried at every title's start)
            const std::string why = last;
            Disconnect();
            config.enabled = false;
            last = why;
        }
    }
}

void TitleEnded(bool staying) {
    if (!started) {
        return;
    }
    const auto member = Network::GetRoomMember().lock();
    if (staying && member->IsConnected() && !lost) {
        // (back to the title list: the emulated system has told the room that no title runs)
        Step("staying in the room");
        return;
    }
    if (config.enabled || member->IsConnected()) {
        Disconnect();
    }
}

bool Host(Core::System& system, u16 port) {
    if (!started) {
        return false;
    }
    if (!hosting) {
        const auto member = Network::GetRoomMember().lock();
        if (member->IsConnected()) {
            Disconnect(); // (in someone else's room)
        }
        if (!OpenRoom(port)) {
            return false;
        }
    }
    return Connect(system);
}

void StopHosting() {
    if (!started || !hosting) {
        return;
    }
    if (config.own) {
        Disconnect();
        config.enabled = false;
        config.own = false;
    }
    Step("closing the hosted room");
    Network::GetRoom().lock()->Destroy(); // (tells the members, ends the room's thread)
    StopAnswering();
    Step("hosted room closed");
    hosting = false;
    std::printf("hen: room: hosting ended\n");
    last.clear();
}

bool JoinRoom(Core::System& system, const std::string& address, u16 port, const std::string& password) {
    if (!started) {
        return false;
    }
    const Config before = config;
    config.enabled = true;
    config.own = false;
    config.by_menu = true;
    config.address = address;
    config.port = port;
    config.password = password;
    if (!Connect(system)) {
        // (this console stays in the room it hosts, if it does)
        const std::string why = last;
        config = before;
        if (hosting && config.own) {
            Connect(system);
        }
        last = why;
        return false;
    }
    Remember(address, port, password);
    return true;
}

void LeaveRoom() {
    if (!started) {
        return;
    }
    Disconnect();
    config.enabled = false;
    config.own = false;
    last.clear();
}

bool SetNickname(const std::string& nickname) {
    if (!NicknameFits(nickname)) {
        return false;
    }
    config.nickname = nickname;
    WriteRecents();
    return true;
}

std::vector<Recent> Recents() {
    return recents;
}

std::vector<Found> FindRooms(std::string& note, u16 port) {
    std::vector<Found> found;
    note.clear();
    if (!started) {
        note = "the network library could not start";
        return found;
    }
    if (port == 0) {
        port = Network::DefaultRoomPort;
    }
    char own_text[16] = "";
    in_addr own{};
    if (!sw_own_address(own_text, sizeof(own_text)) || inet_pton(AF_INET, own_text, &own) != 1) {
        note = "this console is in no network";
        return found;
    }
    // The subnet, 254 addresses at most: of a wider network, the /24 this console is in.
    unsigned char mask_bytes[4] = {255, 255, 255, 0};
    const bool mask_known = sw_own_netmask(mask_bytes);
    u32 mask = static_cast<u32>(mask_bytes[0]) << 24 | mask_bytes[1] << 16 | mask_bytes[2] << 8 | mask_bytes[3];
    mask |= 0xFFFFFF00u;
    const u32 self = ntohl(own.s_addr);
    const u32 first = (self & mask) + 1, last = (self | ~mask) - 1;
    const std::size_t count = last >= first ? last - first + 1 : 0;
    const uint64_t start = sw_time_ms();

    // One ENet host asks every address at once: a room completes the connection, and nothing
    // more is said (a connection without a join request shows nowhere in a room, and is closed
    // again below). The question for consoles goes out beside it.
    ENetHost* host = count != 0 ? enet_host_create(nullptr, count, Network::NumChannels, 0, 0) : nullptr;
    if (host == nullptr) {
        note = "the search could not start";
        return found;
    }
    const int question = socket(AF_INET, SOCK_DGRAM, 0);
    if (question >= 0) {
        fcntl(question, F_SETFL, fcntl(question, F_GETFL, 0) | O_NONBLOCK);
    }
    const auto ask = [&] {
        for (u32 address = first; question >= 0 && address <= last; address++) {
            if (address == self) {
                continue;
            }
            sockaddr_in where{};
            where.sin_family = AF_INET;
            where.sin_addr.s_addr = htonl(address);
            where.sin_port = htons(AnswerPort);
            sendto(question, Question, sizeof(Question), 0, reinterpret_cast<const sockaddr*>(&where), sizeof(where));
        }
    };
    for (u32 address = first; address <= last; address++) {
        if (address == self) {
            continue; // (this console's own room is not something to find)
        }
        ENetAddress to{};
        to.host = htonl(address);
        to.port = port;
        enet_host_connect(host, &to, Network::NumChannels, 0);
    }
    ask();
    // (Asked a second time a little later: the first datagram to an address the network has
    // not seen yet is often dropped while its hardware address is looked up. ENet repeats its
    // own by itself.)
    bool asked_twice = false;
    const auto text_of = [](u32 address) {
        in_addr a{};
        a.s_addr = htonl(address);
        char text[16] = "";
        inet_ntop(AF_INET, &a, text, sizeof(text));
        return std::string(text);
    };
    while (sw_time_ms() - start < FindMs) {
        if (!asked_twice && sw_time_ms() - start >= 300) {
            asked_twice = true;
            ask();
        }
        ENetEvent event{};
        if (enet_host_service(host, &event, 20) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                Found room;
                room.address = text_of(ntohl(event.peer->address.host));
                room.port = port;
                found.push_back(room);
                enet_peer_disconnect_now(event.peer, 0);
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
        }
        // (the consoles' answers)
        for (;;) {
            unsigned char answer[8 + 64];
            sockaddr_in from{};
            socklen_t from_length = sizeof(from);
            const ssize_t got = question >= 0 ? recvfrom(question, answer, sizeof(answer), 0,
                                                         reinterpret_cast<sockaddr*>(&from), &from_length)
                                              : -1;
            if (got < 8) {
                break;
            }
            if (std::memcmp(answer, Answer, sizeof(Answer)) != 0) {
                continue;
            }
            Found room;
            room.address = text_of(ntohl(from.sin_addr.s_addr));
            room.port = static_cast<u16>(answer[4] << 8 | answer[5]);
            room.described = true;
            room.members = answer[6];
            room.slots = answer[7];
            room.name.assign(reinterpret_cast<const char*>(answer) + 8, static_cast<std::size_t>(got) - 8);
            for (char& c : room.name) {
                if (static_cast<unsigned char>(c) < 0x20) {
                    c = ' ';
                }
            }
            // (in place of the bare address the connection gave, if it came first)
            found.erase(std::remove_if(found.begin(), found.end(),
                                       [&](const Found& f) { return f.address == room.address && f.port == room.port; }),
                        found.end());
            found.push_back(room);
        }
    }
    // An address a console has described is listed once.
    for (auto it = found.begin(); it != found.end();) {
        const bool again = !it->described &&
                           std::any_of(found.begin(), found.end(), [&](const Found& f) {
                               return f.described && f.address == it->address && f.port == it->port;
                           });
        it = again ? found.erase(it) : it + 1;
    }
    enet_host_flush(host);
    enet_host_destroy(host);
    if (question >= 0) {
        close(question);
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        return a.described != b.described ? a.described : a.address < b.address;
    });
    std::printf("hen: room: search: %zu addresses from %s (%s mask), port %u: %zu room(s) in %llu ms\n", count,
                text_of(first).c_str(), mask_known ? "the network's" : "an assumed 255.255.255.0", port, found.size(),
                static_cast<unsigned long long>(sw_time_ms() - start));
    for (const Found& room : found) {
        std::printf("hen: room: search: %s port %u%s%s\n", room.address.c_str(), room.port,
                    room.described ? ": " : "", room.described ? room.name.c_str() : "");
    }
    return found;
}

Status GetStatus() {
    Status status;
    status.hosting = hosting;
    status.host_port = config.host_port;
    char own[16] = "";
    if (sw_own_address(own, sizeof(own))) {
        status.own_address = own;
    }
    status.wanted = config.enabled;
    status.own_room = config.enabled && config.own;
    status.address = config.address;
    status.port = config.port;
    status.nickname = config.nickname;
    status.last = last;
    if (!started) {
        return status;
    }
    if (hosting) {
        status.host_members = static_cast<unsigned>(Network::GetRoom().lock()->GetRoomMemberList().size());
    }
    const auto member = Network::GetRoomMember().lock();
    status.connected = member->IsConnected() && joined && !lost;
    if (status.connected) {
        std::lock_guard lock(list_mutex);
        status.room_name = room_name;
        status.members = member_names;
    } else if (lost) {
        status.last = ErrorText(static_cast<Network::RoomMember::Error>(error.load()));
    }
    return status;
}

void Stop() {
    if (!started) {
        return;
    }
    const auto member = Network::GetRoomMember().lock();
    if (member->IsConnected() || member->GetState() != Network::RoomMember::State::Idle) {
        Disconnect();
    }
    if (hosting) {
        Step("closing the hosted room");
        Network::GetRoom().lock()->Destroy();
        StopAnswering();
        Step("hosted room closed");
    }
    Step("closing the client");
    const bool in_use = InUse();
    Network::Shutdown();
    Step("client closed");
    hosting = false;
    on_state.reset();
    on_error.reset();
    on_information.reset();
    on_packet.reset();
    on_chat.reset();
    on_status.reset();
    if (in_use) {
        std::printf("hen: room: client closed\n");
        std::fflush(stdout);
    }
    started = false;
}

void Step(const char* text) {
    if (!InUse()) {
        return;
    }
    std::printf("hen: room: step: %s\n", text);
    std::fflush(stdout);
#if HEN_DEV
    StepLine(text);
#endif
}

void Alive() {
#if HEN_DEV
    if (!InUse()) {
        return;
    }
    {
        // What the emulated console's wireless sent and received since the last of these lines:
        // whether a hosted game is announced (beacons out), whether anyone asks to join
        // (authentication in), whether data flows.
        unsigned in[FrameTypes], out[FrameTypes];
        for (int i = 0; i < FrameTypes; i++) {
            in[i] = frames_in[i].exchange(0);
            out[i] = frames_out[i].exchange(0);
        }
        const unsigned unsent = frames_unsent.exchange(0);
        const unsigned looked = spectators.exchange(0);
        std::printf("hen: room: frames: sent %u beacons, %u data, %u authentication, %u association, "
                    "%u deauthentication, %u node maps; received %u beacons, %u data, %u authentication, "
                    "%u association, %u deauthentication, %u node maps; %u not sent (in no room); other "
                    "consoles read this one's hosted game %u times\n",
                    out[0], out[1], out[2], out[3], out[4], out[5], in[0], in[1], in[2], in[3], in[4], in[5],
                    unsent, looked);
    }
    char text[96];
    std::snprintf(text, sizeof(text), "running: %s, focus %d",
                  joined ? (lost ? "lost the room" : "in the room") : "not in the room", sw_focus_state());
    StepLine(text);
#endif
}

std::string StatusLine() {
    if (!InUse()) {
        return {};
    }
    const Status status = GetStatus();
    std::string line;
    if (status.hosting) {
        line = fmt::format("Multiplayer: hosting, {} in the room; others join {} port {}", status.host_members,
                           status.own_address.empty() ? "(no network)" : status.own_address, status.host_port);
    } else if (status.connected) {
        line = fmt::format("Multiplayer: in \"{}\" at {}, {} in the room", status.room_name, status.address,
                           status.members.size());
    } else {
        line = fmt::format("Multiplayer: {} is joined when a title starts", status.address);
    }
    if (!status.connected && !status.last.empty()) {
        line += "   (" + status.last + ")";
    }
    return line;
}

} // namespace RoomLink

extern "C" void hen_room_step(const char* text) {
    RoomLink::Step(text);
}

extern "C" void hen_room_frame_sent(int type, int sent) {
    if (type == -1) {
        RoomLink::spectators++;
    } else if (!sent) {
        RoomLink::frames_unsent++;
    } else if (type >= 0 && type < RoomLink::FrameTypes) {
        RoomLink::frames_out[type]++;
    }
}

extern "C" void hen_room_note(const char* text) {
#if HEN_DEV
    std::printf("hen: room: wireless: %s\n", text);
#else
    (void)text;
#endif
}
