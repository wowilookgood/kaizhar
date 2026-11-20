// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "citra_switch/guest_dns.h"

/// The core's hook (core/hle/service/soc/soc_u.cpp): asked before the emulated console's own
/// lookup of a name. 1 with an IPv4 address in network byte order: that is the answer.
extern "C" int (*hen_guest_lookup)(const char* name, unsigned* ipv4);

namespace GuestDns {

namespace {

constexpr const char* DefaultServers[] = {"1.1.1.1", "8.8.8.8"};
/// How long a name that nobody could resolve is not asked again: the emulated console repeats
/// its requests, and its socket service's lookups hold the emulation thread.
constexpr auto FailureKept = std::chrono::seconds(30);

std::mutex mutex; ///< for all of the below: the HTTP service asks from its request threads
std::vector<std::string> servers;
struct Kept {
    Answer answer;
    std::chrono::steady_clock::time_point at;
};
std::map<std::string, Kept> kept;
std::vector<std::string> resolved;

std::string Dotted(u32 address) {
    char text[INET_ADDRSTRLEN] = {};
    in_addr in{};
    in.s_addr = address;
    inet_ntop(AF_INET, &in, text, sizeof(text));
    return text;
}

/// An address of another machine: not 0.0.0.0 and not 127.x.x.x, which is what a blocking DNS
/// answers with.
bool Usable(u32 address) {
    const u32 host_order = ntohl(address);
    return host_order != 0 && (host_order >> 24) != 127;
}

bool IsAddress(const std::string& name) {
    in_addr in{};
    return inet_pton(AF_INET, name.c_str(), &in) == 1 || name.find(':') != std::string::npos;
}

/// The console's own DNS. text: what it said, for the log.
bool AskSystem(const std::string& name, u32& address, std::string& text) {
    text = "nothing";
#ifndef __SWITCH__
    // Tests on a computer: KAIZHAR_TEST_BLOCKED_DNS=suffix makes names ending in it look as a
    // blocking DNS answers them.
    if (const char* suffix = std::getenv("KAIZHAR_TEST_BLOCKED_DNS")) {
        const std::size_t length = std::strlen(suffix);
        if (length != 0 && name.size() >= length &&
            name.compare(name.size() - length, length, suffix) == 0) {
            text = "127.0.0.1";
            return false;
        }
    }
#endif
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
        return false;
    }
    bool usable = false;
    for (const addrinfo* entry = found; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET || entry->ai_addr == nullptr) {
            continue;
        }
        const u32 one = reinterpret_cast<const sockaddr_in*>(entry->ai_addr)->sin_addr.s_addr;
        text = Dotted(one);
        if (Usable(one)) {
            address = one;
            usable = true;
            break;
        }
    }
    freeaddrinfo(found);
    return usable;
}

/// Steps over a name in a DNS message (labels, or a pointer to earlier ones). False: the
/// message ends first.
bool SkipName(const u8* message, std::size_t size, std::size_t& at) {
    while (at < size) {
        const u8 length = message[at];
        if (length == 0) {
            at += 1;
            return true;
        }
        if ((length & 0xC0) == 0xC0) {
            at += 2;
            return at <= size;
        }
        at += 1 + length;
    }
    return false;
}

} // namespace

void SetServers(const std::string& list) {
    std::vector<std::string> given;
    std::size_t at = 0;
    while (at <= list.size()) {
        const std::size_t end = std::min(list.find_first_of(", ", at), list.size());
        const std::string one = list.substr(at, end - at);
        at = end + 1;
        in_addr in{};
        if (!one.empty() && inet_pton(AF_INET, one.c_str(), &in) == 1 && Usable(in.s_addr)) {
            given.push_back(one);
        } else if (!one.empty()) {
            std::printf("hen: dns: \"%s\" is not a name server's IPv4 address: left out\n", one.c_str());
        }
    }
    std::scoped_lock lock{mutex};
    servers = std::move(given);
}

std::vector<std::string> Servers() {
    std::scoped_lock lock{mutex};
    if (!servers.empty()) {
        return servers;
    }
    return {std::begin(DefaultServers), std::end(DefaultServers)};
}

bool Ask(const std::string& server, const std::string& name, u32& address, int timeout_ms) {
    sockaddr_in where{};
    where.sin_family = AF_INET;
    where.sin_port = htons(53);
    if (inet_pton(AF_INET, server.c_str(), &where.sin_addr) != 1) {
        return false;
    }
    // The question: a header, the name as labels, type A, class IN.
    u8 question[300];
    std::size_t length = 0;
    const u16 id = static_cast<u16>(arc4random());
    const u8 header[12] = {static_cast<u8>(id >> 8), static_cast<u8>(id), 0x01, 0x00, // (recursion wanted)
                           0, 1, 0, 0, 0, 0, 0, 0};
    std::memcpy(question, header, sizeof(header));
    length = sizeof(header);
    std::size_t at = 0;
    while (at < name.size()) {
        const std::size_t end = std::min(name.find('.', at), name.size());
        const std::size_t label = end - at;
        if (label == 0 || label > 63 || length + label + 6 > 270) {
            return false;
        }
        question[length++] = static_cast<u8>(label);
        std::memcpy(question + length, name.data() + at, label);
        length += label;
        at = end + 1;
    }
    if (length == sizeof(header)) {
        return false;
    }
    const u8 tail[5] = {0, 0, 1, 0, 1};
    std::memcpy(question + length, tail, sizeof(tail));
    length += sizeof(tail);

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    bool found = false;
    if (sendto(fd, question, length, 0, reinterpret_cast<const sockaddr*>(&where), sizeof(where)) ==
        static_cast<ssize_t>(length)) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
                until - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                break;
            }
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(fd, &readable);
            timeval timeout{};
            timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(left.count() / 1000000);
            timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(left.count() % 1000000);
            if (select(fd + 1, &readable, nullptr, nullptr, &timeout) <= 0) {
                break;
            }
            u8 answer[1500];
            sockaddr_in from{};
            socklen_t from_length = sizeof(from);
            const ssize_t got = recvfrom(fd, answer, sizeof(answer), 0, reinterpret_cast<sockaddr*>(&from),
                                         &from_length);
            // Only the server asked, answering this question.
            if (got < 12 || from.sin_addr.s_addr != where.sin_addr.s_addr || from.sin_port != where.sin_port ||
                answer[0] != header[0] || answer[1] != header[1] || (answer[2] & 0x80) == 0) {
                continue;
            }
            const std::size_t size = static_cast<std::size_t>(got);
            if ((answer[3] & 0x0F) != 0) {
                break; // (no such name, or the server refuses)
            }
            const unsigned questions = answer[4] << 8 | answer[5];
            const unsigned answers = answer[6] << 8 | answer[7];
            std::size_t pos = 12;
            bool whole = true;
            for (unsigned i = 0; i < questions && whole; i++) {
                whole = SkipName(answer, size, pos) && pos + 4 <= size;
                pos += 4;
            }
            for (unsigned i = 0; i < answers && whole && !found; i++) {
                if (!SkipName(answer, size, pos) || pos + 10 > size) {
                    break;
                }
                const unsigned type = answer[pos] << 8 | answer[pos + 1];
                const unsigned klass = answer[pos + 2] << 8 | answer[pos + 3];
                const unsigned data = answer[pos + 8] << 8 | answer[pos + 9];
                pos += 10;
                if (pos + data > size) {
                    break;
                }
                // (aliases come before the address they lead to: stepped over)
                if (type == 1 && klass == 1 && data == 4) {
                    u32 one;
                    std::memcpy(&one, answer + pos, 4);
                    if (Usable(one)) {
                        address = one;
                        found = true;
                    }
                }
                pos += data;
            }
            break;
        }
    }
    close(fd);
    return found;
}

Answer Resolve(const std::string& name) {
    Answer answer;
    if (name.empty() || IsAddress(name)) {
        return answer;
    }
    if (AskSystem(name, answer.address, answer.system)) {
        answer.how = How::System;
        answer.text = Dotted(answer.address);
        return answer;
    }
    for (const std::string& server : Servers()) {
        if (Ask(server, name, answer.address)) {
            answer.how = How::Own;
            answer.text = Dotted(answer.address);
            answer.server = server;
            return answer;
        }
    }
    return answer;
}

namespace {

int GuestLookup(const char* name_given, unsigned* ipv4) {
    if (name_given == nullptr || ipv4 == nullptr) {
        return 0;
    }
    const std::string name = name_given;
    if (name.empty() || IsAddress(name) || name == "localhost") {
        return 0;
    }
    {
        std::scoped_lock lock{mutex};
        const auto it = kept.find(name);
        if (it != kept.end()) {
            if (it->second.answer.how != How::None) {
                *ipv4 = it->second.answer.address;
                return 1;
            }
            if (std::chrono::steady_clock::now() - it->second.at < FailureKept) {
                return 0;
            }
        }
    }
    const Answer answer = Resolve(name);
    std::scoped_lock lock{mutex};
    const bool first = kept.find(name) == kept.end();
    kept[name] = {answer, std::chrono::steady_clock::now()};
    if (answer.how == How::Own) {
        resolved.push_back(name + " -> " + answer.text + " (" + answer.server + ")");
        std::printf("hen: dns: %s: the console's DNS says %s; %s says %s\n", name.c_str(),
                    answer.system.c_str(), answer.server.c_str(), answer.text.c_str());
    } else if (answer.how == How::None && first) {
        std::printf("hen: dns: %s: the console's DNS says %s, and no name server asked directly "
                    "gave an address\n",
                    name.c_str(), answer.system.c_str());
    }
    if (answer.how == How::None) {
        return 0;
    }
    *ipv4 = answer.address;
    return 1;
}

} // namespace

void Enable(bool on) {
    if (on) {
        {
            std::scoped_lock lock{mutex};
            kept.clear();
            resolved.clear();
        }
        hen_guest_lookup = &GuestLookup;
        std::string list;
        for (const std::string& server : Servers()) {
            list += (list.empty() ? "" : ", ") + server;
        }
        std::printf("hen: dns: setup: names the console's DNS doesn't resolve for the emulated console "
                    "are asked of %s\n",
                    list.c_str());
        return;
    }
    if (hen_guest_lookup == nullptr) {
        return;
    }
    hen_guest_lookup = nullptr;
    std::scoped_lock lock{mutex};
    std::printf("hen: dns: setup over: %u name(s) were resolved by a name server asked directly\n",
                static_cast<unsigned>(resolved.size()));
    kept.clear();
}

std::vector<std::string> Resolved() {
    std::scoped_lock lock{mutex};
    return resolved;
}

} // namespace GuestDns
