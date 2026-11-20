// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#include "common/archives.h"
#include "common/assert.h"
#include "common/bit_field.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/result.h"
#include "core/hle/service/soc/soc_u.h"
#include "network/socket_manager.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

// MinGW does not define several errno constants
#ifndef _MSC_VER
#define EBADMSG 104
#define ENODATA 120
#define ENOMSG 122
#define ENOSR 124
#define ENOSTR 125
#define ETIME 137
#endif // _MSC_VER
#else
#include <cerrno>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define WSAEAGAIN WSAEWOULDBLOCK
#define WSAEMULTIHOP -1 // Invalid dummy value
#define ERRNO(x) WSA##x
#define GET_ERRNO WSAGetLastError()
#define poll(x, y, z) WSAPoll(x, y, z);
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#else
#define ERRNO(x) x
#define GET_ERRNO errno
#define closesocket(x) close(x)
#endif
#define MSGCUSTOM_HANDLE_DONTWAIT 0x80000000

SERIALIZE_EXPORT_IMPL(Service::SOC::SOC_U)

// Change according the debugging needs
#define LOG_SEND_RECV LOG_TRACE
#define LOG_POLL LOG_TRACE

namespace Service::SOC {

const s32 SOCKET_ERROR_VALUE = -1;

static u32 SocketProtocolToPlatform(u32 protocol) {
    switch (protocol) {
    case 0:
        return 0;
    case 6:
        return IPPROTO_TCP;
    case 17:
        return IPPROTO_UDP;
    default:
        break;
    }
    return -1;
}

static u32 SocketProtocolFromPlatform(u32 protocol) {
    switch (protocol) {
    case 0:
        return 0;
    case IPPROTO_TCP:
        return 6;
    case IPPROTO_UDP:
        return 17;
    default:
        break;
    }
    return -1;
}

static u32 SocketDomainToPlatform(u32 domain) {
    switch (domain) {
    case 0:
        return AF_UNSPEC;
    case 2:
        return AF_INET;
    case 23:
        return AF_INET6;
    default:
        break;
    }
    return -1;
}

static u32 SocketDomainFromPlatform(u32 domain) {
    switch (domain) {
    case AF_UNSPEC:
        return 0;
    case AF_INET:
        return 2;
    case AF_INET6:
        return 23;
    default:
        break;
    }
    return -1;
}

static u32 SocketTypeToPlatform(u32 type) {
    switch (type) {
    case 0:
        return 0;
    case 1:
        return SOCK_STREAM;
    case 2:
        return SOCK_DGRAM;
    default:
        break;
    }
    return -1;
}

static u32 SocketTypeFromPlatform(u32 type) {
    switch (type) {
    case 0:
        return 0;
    case SOCK_STREAM:
        return 1;
    case SOCK_DGRAM:
        return 2;
    default:
        break;
    }
    return -1;
}

/// Holds the translation from system network errors to 3DS network errors
static const std::unordered_map<int, int> error_map = {{
    {E2BIG, 1},
    {ERRNO(EACCES), 2},
    {ERRNO(EADDRINUSE), 3},
    {ERRNO(EADDRNOTAVAIL), 4},
    {ERRNO(EAFNOSUPPORT), 5},
    {EAGAIN, 6},
#ifdef _WIN32
    {WSAEWOULDBLOCK, 6},
#else
#if EAGAIN != EWOULDBLOCK
    {EWOULDBLOCK, 6},
#endif
#endif // _WIN32
    {ERRNO(EALREADY), 7},
    {ERRNO(EBADF), 8},
    {EBADMSG, 9},
    {EBUSY, 10},
    {ECANCELED, 11},
    {ECHILD, 12},
    {ERRNO(ECONNABORTED), 13},
    {ERRNO(ECONNREFUSED), 14},
    {ERRNO(ECONNRESET), 15},
    {EDEADLK, 16},
    {ERRNO(EDESTADDRREQ), 17},
    {EDOM, 18},
    {ERRNO(EDQUOT), 19},
    {EEXIST, 20},
    {ERRNO(EFAULT), 21},
    {EFBIG, 22},
    {ERRNO(EHOSTUNREACH), 23},
    {EIDRM, 24},
    {EILSEQ, 25},
    {ERRNO(EINPROGRESS), 26},
    {ERRNO(EINTR), 27},
    {ERRNO(EINVAL), 28},
    {EIO, 29},
    {ERRNO(EISCONN), 30},
    {EISDIR, 31},
    {ERRNO(ELOOP), 32},
    {ERRNO(EMFILE), 33},
    {EMLINK, 34},
    {ERRNO(EMSGSIZE), 35},
#ifdef EMULTIHOP
    {ERRNO(EMULTIHOP), 36},
#endif
    {ERRNO(ENAMETOOLONG), 37},
    {ERRNO(ENETDOWN), 38},
    {ERRNO(ENETRESET), 39},
    {ERRNO(ENETUNREACH), 40},
    {ENFILE, 41},
    {ERRNO(ENOBUFS), 42},
#ifdef ENODATA
    {ENODATA, 43},
#endif
    {ENODEV, 44},
    {ENOENT, 45},
    {ENOEXEC, 46},
    {ENOLCK, 47},
    {ENOLINK, 48},
    {ENOMEM, 49},
    {ENOMSG, 50},
    {ERRNO(ENOPROTOOPT), 51},
    {ENOSPC, 52},
#ifdef ENOSR
    {ENOSR, 53},
#endif
#ifdef ENOSTR
    {ENOSTR, 54},
#endif
    {ENOSYS, 55},
    {ERRNO(ENOTCONN), 56},
#ifdef _WIN32
    {WSAESHUTDOWN, 56},
#endif
    {ENOTDIR, 57},
    {ERRNO(ENOTEMPTY), 58},
#ifdef _WIN32
    {ERRNO(ENOTSOCK), 8},
#else
    {ERRNO(ENOTSOCK), 59},
#endif
    {ENOTSUP, 60},
    {ENOTTY, 61},
    {ENXIO, 62},
    {ERRNO(EOPNOTSUPP), 63},
    {EOVERFLOW, 64},
    {EPERM, 65},
    {EPIPE, 66},
    {EPROTO, 67},
    {ERRNO(EPROTONOSUPPORT), 68},
    {ERRNO(EPROTOTYPE), 69},
    {ERANGE, 70},
    {EROFS, 71},
    {ESPIPE, 72},
    {ESRCH, 73},
    {ERRNO(ESTALE), 74},
#ifdef ETIME
    {ETIME, 75},
#endif
    {ERRNO(ETIMEDOUT), 76},
}};

static const std::unordered_map<int, int> gai_error_map = {{
    {EAI_AGAIN, 302},
    {EAI_FAMILY, 303},
    {EAI_MEMORY, 304},
    {EAI_NONAME, 305},
    {EAI_SOCKTYPE, 307},
}};

/// Converts a network error from platform-specific to 3ds-specific
#if defined(__SWITCH__) && HEN_DEV
} // namespace Service::SOC
// (Kaizhar, --trace-sockets, development builds) One line per call of the emulated
// console's socket service that matters: "hen: soc: <ms> ..." with the service's socket number,
// addresses as a.b.c.d:port, byte counts, the host's errno and the answer the program gets.
// A receive with nothing there (EAGAIN) is said the first time for a socket and then every
// 200th, with the count; a poll that finds nothing likewise.
extern "C" {
int hen_trace_sockets = 0;
}
namespace Service::SOC {
namespace {
void HenSoc(const char* format, ...) __attribute__((format(printf, 1, 2)));
void HenSoc(const char* format, ...) {
    static const auto start = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("hen: soc: %lld ms %s\n", static_cast<long long>(ms), text);
}
std::string HenAddr(const sockaddr* address) {
    if (address == nullptr) {
        return "(none)";
    }
    if (address->sa_family != AF_INET) {
        return fmt::format("(family {})", static_cast<int>(address->sa_family));
    }
    const auto* in = reinterpret_cast<const sockaddr_in*>(address);
    const u8* b = reinterpret_cast<const u8*>(&in->sin_addr);
    return fmt::format("{}.{}.{}.{}:{}", b[0], b[1], b[2], b[3], ntohs(in->sin_port));
}
/// An address as the emulated console has it: length, family, port (big endian), four bytes.
std::string HenCtrAddr(const std::vector<u8>& raw) {
    if (raw.size() < 8) {
        return "(none)";
    }
    return fmt::format("{}.{}.{}.{}:{}", raw[4], raw[5], raw[6], raw[7], (raw[2] << 8) | raw[3]);
}
std::unordered_map<u32, u32> hen_soc_quiet; ///< socket -> receives with nothing there in a row
u32 hen_poll_quiet = 0;
} // namespace
#define HEN_SOC(...)                                                                               \
    do {                                                                                           \
        if (hen_trace_sockets) {                                                                   \
            HenSoc(__VA_ARGS__);                                                                   \
        }                                                                                          \
    } while (0)
#else
#define HEN_SOC(...)                                                                               \
    do {                                                                                           \
    } while (0)
#endif

} // namespace Service::SOC

// (Kaizhar) The online connection record, in every build: what the emulated console's
// sockets did, written to the emulator's log file (kaizhar_log.txt) as "online:" lines and
// counted for a line in last-session.txt. Without it a release build's log holds nothing after
// the name lookups of Mario Kart 7's NAT check. Per socket: the port asked
// for and the port the host gave, the first packet to and from every address, counts and byte
// totals when the socket closes, errors with the host's code, a line for an address that was
// sent to ten times and never answered; for the two NAT-check servers (names beginning nncs)
// the replies' words, and whether both servers saw the same address and port. Never: packet
// contents other than those sixteen bytes' fields, names of accounts, tokens. Addresses outside
// the private ranges have their last two numbers hidden unless multiplayer.txt has
// online_log=full.
extern "C" {
int hen_online_log_full = 0;
}
namespace Service::SOC::HenOnline {
namespace {
struct Peer {
    u32 ip = 0; ///< as its four bytes in network order
    u16 port = 0;
    u32 sent = 0, received = 0;
    u64 sent_bytes = 0, received_bytes = 0;
    bool said_silent = false;
};
struct Sock {
    bool udp = false, bound = false;
    u32 pid = 0;
    u16 asked_port = 0, host_port = 0;
    std::vector<Peer> peers;
    u32 more_peers = 0, send_errors = 0, receive_errors = 0;
};
std::mutex mutex;
std::unordered_map<long long, Sock> socks;
bool said_start = false;
struct Nat {
    u32 ip = 0;
    u32 probes = 0, replies = 0;
    u32 words[4] = {};
    bool have = false;
} nat[2];
unsigned nat_known = 0;
struct {
    u32 udp_sockets = 0, tcp_connects = 0, tcp_failed = 0, peers_tried = 0, peers_answered = 0, send_errors = 0,
        receive_errors = 0, port_differs = 0, lookups = 0;
} total;
const auto origin = std::chrono::steady_clock::now();

double Seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}
bool Private(const u8* b) {
    return b[0] == 10 || b[0] == 127 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168) ||
           (b[0] == 169 && b[1] == 254) || b[0] == 0;
}
std::string Address(u32 ip, u16 port) {
    const u8* b = reinterpret_cast<const u8*>(&ip);
    if (hen_online_log_full || Private(b)) {
        return fmt::format("{}.{}.{}.{}:{}", b[0], b[1], b[2], b[3], port);
    }
    return fmt::format("{}.{}.x.x:{}", b[0], b[1], port);
}
void Start() {
    if (!said_start) {
        said_start = true;
        LOG_INFO(Service_SOC,
                 "online: record of the emulated console's sockets; addresses outside private networks are shown "
                 "as a.b.x.x:port (online_log=full in /switch/Kaizhar/multiplayer.txt shows them whole); times are "
                 "seconds since the first socket");
    }
}
Nat* NatOf(u32 ip) {
    for (unsigned i = 0; i < nat_known; i++) {
        if (nat[i].ip == ip) {
            return &nat[i];
        }
    }
    return nullptr;
}
Peer* PeerOf(Sock& sock, u32 ip, u16 port) {
    for (Peer& peer : sock.peers) {
        if (peer.ip == ip && peer.port == port) {
            return &peer;
        }
    }
    if (sock.peers.size() >= 48) {
        sock.more_peers++;
        return nullptr;
    }
    sock.peers.push_back({ip, port});
    return &sock.peers.back();
}
} // namespace

void Socket(long long fd, u32 pid, bool udp) {
    std::scoped_lock lock{mutex};
    Start();
    Sock& sock = socks[fd];
    sock = {};
    sock.udp = udp;
    sock.pid = pid;
    total.udp_sockets += udp;
}

void Bound(long long fd, const sockaddr_in& asked, int host_error) {
    std::scoped_lock lock{mutex};
    Sock& sock = socks[fd];
    sock.asked_port = ntohs(asked.sin_port);
    if (host_error != 0) {
        LOG_INFO(Service_SOC, "online: {:.3f} s: {} socket of process {}: bind to port {} failed, host error {} ({})",
                 Seconds(), sock.udp ? "UDP" : "TCP", sock.pid, sock.asked_port, host_error, std::strerror(host_error));
        return;
    }
    sockaddr_in got{};
    socklen_t got_len = sizeof(got);
    if (::getsockname(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&got), &got_len) == 0) {
        sock.host_port = ntohs(got.sin_port);
    }
    sock.bound = true;
    const bool differs = sock.asked_port != 0 && sock.host_port != sock.asked_port;
    total.port_differs += differs;
    LOG_INFO(Service_SOC, "online: {:.3f} s: {} socket of process {}: asked for local port {}, the host gave port {}{}",
             Seconds(), sock.udp ? "UDP" : "TCP", sock.pid, sock.asked_port, sock.host_port,
             differs ? " (NOT the port asked for)" : sock.asked_port == 0 ? " (any port was asked for)" : "");
}

/// ret: the host's result (bytes, or negative with host_error).
void Sent(long long fd, bool has_address, u32 ip, u16 port, int ret, int host_error) {
    std::scoped_lock lock{mutex};
    const auto found = socks.find(fd);
    if (found == socks.end()) {
        return;
    }
    Sock& sock = found->second;
    if (ret < 0) {
        if (host_error != EAGAIN && host_error != EWOULDBLOCK) {
            total.send_errors++;
            if (sock.send_errors++ < 8) {
                LOG_INFO(Service_SOC, "online: {:.3f} s: {} socket (local port {}): send to {} failed, host error {} ({})",
                         Seconds(), sock.udp ? "UDP" : "TCP", sock.host_port,
                         has_address ? Address(ip, port) : std::string("its peer"), host_error, std::strerror(host_error));
            }
        }
        return;
    }
    if (!sock.udp || !has_address) {
        return;
    }
    if (Nat* server = NatOf(ip)) {
        server->probes++;
    }
    Peer* peer = PeerOf(sock, ip, port);
    if (peer == nullptr) {
        return;
    }
    peer->sent++;
    peer->sent_bytes += static_cast<u64>(ret);
    if (peer->sent == 1) {
        total.peers_tried++;
        LOG_INFO(Service_SOC, "online: {:.3f} s: UDP socket (local port {}): first packet to {}, {} bytes{}", Seconds(),
                 sock.host_port, Address(ip, port), ret, NatOf(ip) != nullptr ? " (a NAT-check server)" : "");
    } else if (peer->sent == 10 && peer->received == 0 && !peer->said_silent) {
        peer->said_silent = true;
        LOG_INFO(Service_SOC, "online: {:.3f} s: UDP socket (local port {}): {} has been sent 10 packets and has not answered",
                 Seconds(), sock.host_port, Address(ip, port));
    }
}

void Received(long long fd, u32 ip, u16 port, int ret, int host_error, const u8* data) {
    std::scoped_lock lock{mutex};
    const auto found = socks.find(fd);
    if (found == socks.end()) {
        return;
    }
    Sock& sock = found->second;
    if (ret < 0) {
        if (host_error != 0 && host_error != EAGAIN && host_error != EWOULDBLOCK) {
            total.receive_errors++;
            if (sock.receive_errors++ < 8) {
                LOG_INFO(Service_SOC, "online: {:.3f} s: {} socket (local port {}): receive failed, host error {} ({})",
                         Seconds(), sock.udp ? "UDP" : "TCP", sock.host_port, host_error, std::strerror(host_error));
            }
        }
        return;
    }
    if (!sock.udp || ret == 0) {
        return;
    }
    if (Nat* server = NatOf(ip); server != nullptr && ret >= 16 && data != nullptr) {
        server->replies++;
        u32 words[4];
        for (int i = 0; i < 4; i++) {
            words[i] = (u32(data[4 * i]) << 24) | (u32(data[4 * i + 1]) << 16) | (u32(data[4 * i + 2]) << 8) | data[4 * i + 3];
        }
        if (!server->have || std::memcmp(server->words + 1, words + 1, 12) != 0) {
            // (what the fields are is from memory of the protocol, not from its source: a type,
            // then the port and the address the server saw the packet come from, then the
            // console's own port. Printed as numbers so that a reader can judge.)
            const u32 seen_ip = htonl(words[2]);
            LOG_INFO(Service_SOC,
                     "online: {:.3f} s: NAT check: reply from server {} ({}): words {:08x} {:08x} [{}] {:08x}; read as: "
                     "the server saw port {} at {}; our local port is {}{}",
                     Seconds(), static_cast<int>(server - nat) + 1, Address(ip, port), words[0], words[1],
                     Address(seen_ip, static_cast<u16>(words[1])), words[3], words[1],
                     Address(seen_ip, static_cast<u16>(words[1])), sock.host_port,
                     words[1] == sock.host_port ? " (the same port: the router kept it)" : " (another port: the router changed it)");
        }
        std::memcpy(server->words, words, sizeof(words));
        server->have = true;
        if (nat[0].have && nat[1].have && nat[0].replies + nat[1].replies <= 4) {
            const bool same = std::memcmp(nat[0].words + 1, nat[1].words + 1, 8) == 0;
            LOG_INFO(Service_SOC, "online: {:.3f} s: NAT check: the two servers saw {}", Seconds(),
                     same ? "the same address and port (a router that keeps one mapping: other consoles can reach it)"
                          : "DIFFERENT addresses or ports (a mapping per destination, \"symmetric\": other consoles "
                            "usually cannot reach this one)");
        }
    }
    Peer* peer = PeerOf(sock, ip, port);
    if (peer == nullptr) {
        return;
    }
    peer->received++;
    peer->received_bytes += static_cast<u64>(ret);
    if (peer->received == 1) {
        total.peers_answered++;
        LOG_INFO(Service_SOC, "online: {:.3f} s: UDP socket (local port {}): first packet from {}, {} bytes (after {} sent to it)",
                 Seconds(), sock.host_port, Address(ip, port), ret, peer->sent);
    }
}

void Connected(long long fd, const sockaddr_in& to, int host_error) {
    std::scoped_lock lock{mutex};
    const auto found = socks.find(fd);
    const bool udp = found != socks.end() && found->second.udp;
    if (!udp) {
        total.tcp_connects++;
        total.tcp_failed += host_error != 0 && host_error != EINPROGRESS;
    }
    LOG_INFO(Service_SOC, "online: {:.3f} s: {} connect to {}: {}", Seconds(), udp ? "UDP" : "TCP",
             Address(to.sin_addr.s_addr, ntohs(to.sin_port)),
             host_error == 0 ? std::string("ok")
                             : host_error == EINPROGRESS ? std::string("in progress")
                                                         : fmt::format("host error {} ({})", host_error, std::strerror(host_error)));
}

void Closed(long long fd) {
    std::scoped_lock lock{mutex};
    const auto found = socks.find(fd);
    if (found == socks.end()) {
        return;
    }
    const Sock& sock = found->second;
    if (sock.udp && !sock.peers.empty()) {
        u32 answered = 0;
        u64 sent = 0, received = 0;
        for (const Peer& peer : sock.peers) {
            answered += peer.received != 0;
            sent += peer.sent_bytes;
            received += peer.received_bytes;
        }
        LOG_INFO(Service_SOC,
                 "online: {:.3f} s: UDP socket (local port {}) closed: {} addresses sent to, {} answered; {} bytes sent, {} "
                 "received; {} send and {} receive errors{}",
                 Seconds(), sock.host_port, sock.peers.size(), answered, sent, received, sock.send_errors, sock.receive_errors,
                 sock.more_peers != 0 ? "; more addresses than the record keeps" : "");
        for (const Peer& peer : sock.peers) {
            LOG_INFO(Service_SOC, "online:   {}: {} packets, {} bytes sent; {} packets, {} bytes received{}", Address(peer.ip, peer.port),
                     peer.sent, peer.sent_bytes, peer.received, peer.received_bytes, peer.received == 0 ? "  NEVER ANSWERED" : "");
        }
    }
    socks.erase(found);
}

void LookedUp(const char* asked, const char* as, const hostent* result) {
    std::scoped_lock lock{mutex};
    Start();
    total.lookups++;
    u32 ip = 0;
    if (result != nullptr && result->h_addr_list != nullptr && result->h_addr_list[0] != nullptr) {
        std::memcpy(&ip, result->h_addr_list[0], 4);
    }
    LOG_INFO(Service_SOC, "online: {:.3f} s: name {}{} -> {}", Seconds(), asked,
             std::strcmp(asked, as) != 0 ? fmt::format(" (looked up as {})", as) : std::string(),
             ip != 0 ? Address(ip, 0) : std::string("NO ADDRESS"));
    if (std::strncmp(asked, "nncs", 4) == 0 && ip != 0) {
        const unsigned index = asked[4] == '2' ? 1 : 0;
        nat[index] = {};
        nat[index].ip = ip;
        nat_known = std::max(nat_known, index + 1);
    }
}
} // namespace Service::SOC::HenOnline

// For last-session.txt: counts and outcomes only, nothing that names a network. 0: nothing to say.
extern "C" int hen_online_summary(char* out, unsigned long size) {
    using namespace Service::SOC::HenOnline;
    std::scoped_lock lock{mutex};
    if (!said_start || size == 0) {
        return 0;
    }
    std::string nat_text = "not seen";
    if (nat_known != 0) {
        const u32 probes = nat[0].probes + nat[1].probes, replies = nat[0].replies + nat[1].replies;
        nat_text = fmt::format("{} of {} probes answered", replies, probes);
        if (nat[0].have && nat[1].have) {
            nat_text += std::memcmp(nat[0].words + 1, nat[1].words + 1, 8) == 0 ? ", both servers saw the same mapping"
                                                                               : ", the servers saw different mappings (symmetric)";
        } else if (replies == 0 && probes != 0) {
            nat_text += " (no reply at all)";
        }
    }
    const std::string text = fmt::format(
        "online: NAT check: {}; UDP sockets {}; local ports the host changed {}; addresses sent to {}, answered {}; send "
        "errors {}, receive errors {}; TCP connections {} ({} failed); name lookups {}",
        nat_text, total.udp_sockets, total.port_differs, total.peers_tried, total.peers_answered, total.send_errors,
        total.receive_errors, total.tcp_connects, total.tcp_failed, total.lookups);
    std::snprintf(out, size, "%s", text.c_str());
    return 1;
}

namespace Service::SOC {

static int TranslateError(int error) {
    const auto& found = error_map.find(error);
#if defined(__SWITCH__) && HEN_DEV
    if (hen_trace_sockets && error != EAGAIN && error != EWOULDBLOCK) {
        HenSoc("  host errno %d (%s) -> %s", error, std::strerror(error),
               found != error_map.end() ? fmt::format("3DS -{}", found->second).c_str()
                                        : "NOT IN THE TABLE: handed on as it is");
    }
#endif
    if (found != error_map.end()) {
        return -found->second;
    }
    return error;
}

/// Converts a getaddrinfo/getnameinfo error from platform-specific to 3ds-specific
static int TranslateGaiError(int gai_error) {
    if (const auto& known_soc_errno = gai_error_map.find(gai_error);
        known_soc_errno != gai_error_map.end()) {
        return -known_soc_errno->second;
    }
    return gai_error;
}

struct CTRLinger {
    u32_le l_onoff;
    u32_le l_linger;
};

/// Holds the translation from system network socket options to 3DS network socket options
/// Note: -1 = No effect/unavailable
static constexpr u64 sockopt_map_key(int i, int j) {
    return (u64)i << 32 | (unsigned int)j;
}
const std::unordered_map<u64, std::pair<int, int>> SOC_U::sockopt_map = {{
    {sockopt_map_key(SOC_SOL_IP, 0x0007), {IPPROTO_IP, IP_TOS}},
    {sockopt_map_key(SOC_SOL_IP, 0x0008), {IPPROTO_IP, IP_TTL}},
    {sockopt_map_key(SOC_SOL_IP, 0x0009), {IPPROTO_IP, IP_MULTICAST_LOOP}},  // Never used?
    {sockopt_map_key(SOC_SOL_IP, 0x000A), {IPPROTO_IP, IP_MULTICAST_TTL}},   // Never used?
    {sockopt_map_key(SOC_SOL_IP, 0x000B), {IPPROTO_IP, IP_ADD_MEMBERSHIP}},  // Never used?
    {sockopt_map_key(SOC_SOL_IP, 0x000C), {IPPROTO_IP, IP_DROP_MEMBERSHIP}}, // Never used?
    {sockopt_map_key(SOC_SOL_SOCKET, 0x0004), {SOL_SOCKET, SO_REUSEADDR}},
    {sockopt_map_key(SOC_SOL_SOCKET, 0x0080), {SOL_SOCKET, SO_LINGER}},
    {sockopt_map_key(SOC_SOL_SOCKET, 0x0100), {SOL_SOCKET, SO_OOBINLINE}},
    {sockopt_map_key(SOC_SOL_SOCKET, 0x1001), {SOL_SOCKET, SO_SNDBUF}},
    {sockopt_map_key(SOC_SOL_SOCKET, 0x1002), {SOL_SOCKET, SO_RCVBUF}},
    {sockopt_map_key(SOC_SOL_SOCKET, 0x1003),
     {SOL_SOCKET, SO_SNDLOWAT}}, // Not supported in winsock2
    {sockopt_map_key(SOC_SOL_SOCKET, 0x1004),
     {SOL_SOCKET, SO_RCVLOWAT}}, // Not supported in winsock2
    {sockopt_map_key(SOC_SOL_SOCKET, 0x1008), {SOL_SOCKET, SO_TYPE}},
    {sockopt_map_key(SOC_SOL_SOCKET, 0x1009), {SOL_SOCKET, SO_ERROR}},
    {sockopt_map_key(SOC_SOL_TCP, 0x2001), {IPPROTO_TCP, TCP_NODELAY}},
    {sockopt_map_key(SOC_SOL_TCP, 0x2002), {IPPROTO_TCP, -1}}, // TCP_MAXSEG, never used?
    {sockopt_map_key(SOC_SOL_TCP, 0x2003), {IPPROTO_TCP, -1}}, // TCP_STDURG, never used?
}};

/// Converts a socket option from platform-specific to 3ds-specific
std::pair<int, int> SOC_U::TranslateSockOpt(int level, int opt) {
    const auto& found = sockopt_map.find(sockopt_map_key(level, opt));
    if (found != sockopt_map.end()) {
        return found->second;
    }
    return std::make_pair(SOL_SOCKET, opt);
}

void SOC_U::TranslateSockOptDataToPlatform(std::vector<u8>& out, const std::vector<u8>& in,
                                           int platform_level, int platform_opt) {
    // linger structure may be different between 3DS and platform
    if (platform_level == SOL_SOCKET && platform_opt == SO_LINGER &&
        in.size() == sizeof(CTRLinger)) {
        linger linger_out;
        linger_out.l_onoff = static_cast<decltype(linger_out.l_onoff)>(
            reinterpret_cast<const CTRLinger*>(in.data())->l_onoff);
        linger_out.l_linger = static_cast<decltype(linger_out.l_linger)>(
            reinterpret_cast<const CTRLinger*>(in.data())->l_linger);
        out.resize(sizeof(linger));
        std::memcpy(out.data(), &linger_out, sizeof(linger));
        return;
    }
    // Other options should have the size of an int, even for booleans
    int value;
    if (in.size() == sizeof(u8)) {
        value = static_cast<int>(*reinterpret_cast<const u8*>(in.data()));
    } else if (in.size() == sizeof(u16)) {
        value = static_cast<int>(*reinterpret_cast<const u16_le*>(in.data()));
    } else if (in.size() == sizeof(u32)) {
        value = static_cast<int>(*reinterpret_cast<const u32_le*>(in.data()));
    } else {
        LOG_ERROR(
            Service_SOC,
            "Unknown sockopt data combination: in_size={}, platform_level={}, platform_opt={}",
            in.size(), platform_level, platform_opt);
        out = in;
        return;
    }
    // Setting TTL to 0 means resetting it to the default value.
    if (platform_level == IPPROTO_IP && platform_opt == IP_TTL && value == 0) {
        value = SOC_TTL_DEFAULT;
    }
    out.resize(sizeof(int));
    std::memcpy(out.data(), &value, sizeof(int));
}

static u32 TranslateSockOptSizeToPlatform(int platform_level, int platform_opt) {
    if (platform_level == SOL_SOCKET && platform_opt == SO_LINGER)
        return sizeof(linger);
    return sizeof(int);
}

static void TranslateSockOptDataFromPlatform(std::vector<u8>& out, const std::vector<u8>& in,
                                             int platform_level, int platform_opt) {
    if (platform_level == SOL_SOCKET && platform_opt == SO_LINGER && in.size() == sizeof(linger)) {
        CTRLinger linger_out;
        linger_out.l_onoff = static_cast<decltype(linger_out.l_onoff)>(
            reinterpret_cast<const linger*>(in.data())->l_onoff);
        linger_out.l_linger = static_cast<decltype(linger_out.l_linger)>(
            reinterpret_cast<const linger*>(in.data())->l_linger);
        std::memcpy(out.data(), &linger_out, std::min(out.size(), sizeof(CTRLinger)));
        return;
    }
    if (out.size() == sizeof(u8) && in.size() == sizeof(int)) {
        *reinterpret_cast<u8*>(out.data()) =
            static_cast<u8>(*reinterpret_cast<const int*>(in.data()));
    } else if (out.size() == sizeof(u16_le) && in.size() == sizeof(int)) {
        *reinterpret_cast<u16_le*>(out.data()) =
            static_cast<u16_le>(*reinterpret_cast<const int*>(in.data()));
    } else if (out.size() == sizeof(u32_le) && in.size() == sizeof(int)) {
        *reinterpret_cast<u32_le*>(out.data()) =
            static_cast<u32_le>(*reinterpret_cast<const int*>(in.data()));
    } else {
        LOG_ERROR(Service_SOC,
                  "Unknown sockopt data combination: in_size={}, out_size={}, platform_level={}, "
                  "platform_opt={}",
                  in.size(), out.size(), platform_level, platform_opt);
        out = in;
        return;
    }
}

bool SOC_U::GetSocketBlocking(const SocketHolder& socket_holder) {
    return socket_holder.blocking;
}
u32 SOC_U::SetSocketBlocking(SocketHolder& socket_holder, bool blocking) {
    u32 posix_ret = 0;
#ifdef _WIN32
    unsigned long nonblocking = (blocking) ? 0 : 1;
    int ret = ioctlsocket(socket_holder.socket_fd, FIONBIO, &nonblocking);
    if (ret == SOCKET_ERROR_VALUE) {
        posix_ret = TranslateError(GET_ERRNO);
        return posix_ret;
    }
    socket_holder.blocking = blocking;
#else
    int flags = ::fcntl(socket_holder.socket_fd, F_GETFL, 0);
    if (flags == SOCKET_ERROR_VALUE) {
        posix_ret = TranslateError(GET_ERRNO);
        return posix_ret;
    }

    flags &= ~O_NONBLOCK;
    if (!blocking) { // O_NONBLOCK
        flags |= O_NONBLOCK;
    }

    socket_holder.blocking = blocking;

    const int ret = ::fcntl(socket_holder.socket_fd, F_SETFL, flags);
    if (ret == SOCKET_ERROR_VALUE) {
        posix_ret = TranslateError(GET_ERRNO);
        return posix_ret;
    }
#endif
    return posix_ret;
}

std::optional<std::reference_wrapper<SocketHolder>> SOC_U::GetSocketHolder(u32 ctr_socket_fd,
                                                                           u32 process_id,
                                                                           IPC::RequestParser& rp) {
    if (initialized_processes.find(process_id) == initialized_processes.end()) {
        LOG_DEBUG(Service_SOC, "Process not initialized: pid={}", process_id);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ResultNotInitialized);
        return std::nullopt;
    }
    auto fd_info = created_sockets.find(ctr_socket_fd);
    if (fd_info == created_sockets.end()) {
        LOG_DEBUG(Service_SOC, "Invalid socket: pid={}, fd={}", process_id, ctr_socket_fd);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ResultInvalidSocketDescriptor);
        return std::nullopt;
    }
    if (fd_info->second.ownerProcess != process_id && !fd_info->second.isGlobal) {
        LOG_DEBUG(Service_SOC, "Invalid process owner: pid={}, fd={}, owner_pid={}", process_id,
                  ctr_socket_fd, fd_info->second.ownerProcess);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ResultWrongProcess);
        return std::nullopt;
    }
    return std::ref(fd_info->second);
}

void SOC_U::CloseAndDeleteAllSockets(s32 process_id) {
    std::erase_if(created_sockets, [process_id](const auto& entry) {
        if (process_id == -1 || entry.second.ownerProcess == static_cast<u32>(process_id)) {
            closesocket(entry.second.socket_fd);
            return true;
        }
        return false;
    });
}

static u32 SendRecvFlagsToPlatform(u32 flags) {
    u32 ret = 0;
    if (flags & 1) {
        ret |= MSG_OOB;
    }
    if (flags & 2) {
        ret |= MSG_PEEK;
    }
    if (flags & 4) {
        // Magic value to decide platform specific action
        ret |= MSGCUSTOM_HANDLE_DONTWAIT;
    }
    return ret;
}

static s32 ShutdownHowToPlatform(s32 how) {
    if (how == 0) {
        return SHUT_RD;
    }
    if (how == 1) {
        return SHUT_WR;
    }
    if (how == 2) {
        return SHUT_RDWR;
    }
    return -1;
}

/// Structure to represent the 3ds' pollfd structure, which is different than most implementations
struct CTRPollFD {
    u32 fd; ///< Socket handle

    union Events {
        u32 hex; ///< The complete value formed by the flags
        BitField<0, 1, u32> pollrdnorm;
        BitField<1, 1, u32> pollrdband; // The 3DS OS mistakenly uses POLLRDBAND as POLLPRI,
        BitField<2, 1, u32> pollpri;    // this is reflected in the translate functions below.
        BitField<3, 1, u32> pollwrnorm;
        BitField<4, 1, u32> pollwrband;
        BitField<5, 1, u32> pollerr;
        BitField<6, 1, u32> pollhup;
        BitField<7, 1, u32> pollnval;

        /// Translates the resulting events of a Poll operation from platform-specific to 3ds
        /// specific
        static Events TranslateTo3DS(u32 input_event, u8 haslibctrbug) {
            Events ev = {};

            if (input_event & POLLRDNORM)
                ev.pollrdnorm.Assign(1);
            if (input_event & POLLRDBAND)
                ev.pollpri.Assign(1);
            if (input_event & POLLHUP)
                ev.pollhup.Assign(1);
            if (input_event & POLLERR)
                ev.pollerr.Assign(1);
            if (input_event & POLLWRNORM) {
                if (haslibctrbug) {
                    ev.pollwrband.Assign(1);
                } else {
                    ev.pollwrnorm.Assign(1);
                }
            }
            if (input_event & POLLWRBAND)
                ev.pollwrband.Assign(1);
            if (input_event & POLLNVAL)
                ev.pollnval.Assign(1);
            return ev;
        }

        /// Translates the resulting events of a Poll operation from 3ds specific to platform
        /// specific
        static u32 TranslateToPlatform(Events input_event, bool isOutput, u8& has_libctru_bug) {
#if _WIN32
            constexpr bool isWin = true;
#else
            constexpr bool isWin = false;
#endif
            has_libctru_bug = 0;
            if (!input_event.pollwrnorm && input_event.pollwrband) {
                // Fixes a bug in libctru and some homebrew using libcurl
                // having the wrong value for POLLOUT
                input_event.pollwrnorm.Assign(1);
                input_event.pollwrband.Assign(0);
                has_libctru_bug = 1;
            }
            u32 ret = 0;
            if (input_event.pollrdnorm)
                ret |= POLLRDNORM;
            if (input_event.pollrdband && !isWin)
                ret |= POLLPRI;
            if (input_event.pollhup && isOutput)
                ret |= POLLHUP;
            if (input_event.pollerr && isOutput)
                ret |= POLLERR;
            if (input_event.pollwrnorm)
                ret |= POLLWRNORM;
            if (input_event.pollwrband && !isWin)
                ret |= POLLWRBAND;
            if (input_event.pollnval && isOutput)
                ret |= POLLNVAL;
            return ret;
        }
    };
    Events events;  ///< Events to poll for (input)
    Events revents; ///< Events received (output)

    /// Converts a platform-specific pollfd to a 3ds specific structure
    static CTRPollFD FromPlatform(SOC::SOC_U& socu, pollfd const& fd, u8 has_libctru_bug) {
        CTRPollFD result;
        result.events.hex = Events::TranslateTo3DS(fd.events, has_libctru_bug).hex;
        result.revents.hex = Events::TranslateTo3DS(fd.revents, has_libctru_bug).hex;
        for (const auto& socket : socu.created_sockets) {
            if (socket.second.socket_fd == fd.fd) {
                result.fd = socket.first;
                break;
            }
        }
        return result;
    }

    /// Converts a 3ds specific pollfd to a platform-specific structure
    static pollfd ToPlatform(SOC::SOC_U& socu, CTRPollFD const& fd, u8& haslibctrbug) {
        pollfd result;
        u8 unused = 0;
        result.events = Events::TranslateToPlatform(fd.events, false, haslibctrbug);
        result.revents = Events::TranslateToPlatform(fd.revents, true, unused);
        auto iter = socu.created_sockets.find(fd.fd);
        ASSERT(iter != socu.created_sockets.end());
        result.fd = iter->second.socket_fd;
        return result;
    }
};
static_assert(std::is_trivially_copyable_v<CTRPollFD>,
              "CTRPollFD is used with std::memcpy and must be trivially copyable");

/// Union to represent the 3ds' sockaddr structure
union CTRSockAddr {
    /// Structure to represent a raw sockaddr
    struct {
        u8 len;       ///< The length of the entire structure, only the set fields count
        u8 sa_family; ///< The address family of the sockaddr
        std::array<u8, 0x1A>
            sa_data; ///< The extra data, this varies, depending on the address family
    } raw;

    /// Structure to represent the 3ds' sockaddr_in structure
    struct CTRSockAddrIn {
        u8 len;        ///< The length of the entire structure
        u8 sin_family; ///< The address family of the sockaddr_in
        u16 sin_port;  ///< The port associated with this sockaddr_in
        u32 sin_addr;  ///< The actual address of the sockaddr_in
    } in;
    static_assert(sizeof(CTRSockAddrIn) == 8, "Invalid CTRSockAddrIn size");

    struct CTRSockAddrIn6 {
        u8 len;                         ///< The length of the entire structure
        u8 sin6_family;                 ///< The address family of the sockaddr_in6
        u16 sin6_port;                  ///< The port associated with this sockaddr_in6
        std::array<u8, 0x10> sin6_addr; ///< The actual address of the sockaddr_in6
        u32 sin6_flowinfo;              ///< The flow info of the sockaddr_in6
        u32 sin6_scope_id;              ///< The scope ID of the sockaddr_in6
    } in6;
    static_assert(sizeof(CTRSockAddrIn6) == 28, "Invalid CTRSockAddrIn6 size");

    /// Convert a 3DS CTRSockAddr to a platform-specific sockaddr
    static std::pair<sockaddr_storage, socklen_t> ToPlatform(CTRSockAddr const& ctr_addr) {
        sockaddr_storage result{};
        socklen_t result_len = sizeof(result.ss_family);
        ASSERT_MSG(ctr_addr.raw.len == sizeof(CTRSockAddrIn) ||
                       ctr_addr.raw.len == sizeof(CTRSockAddrIn6),
                   "Unhandled address size (len) in CTRSockAddr::ToPlatform");
        result.ss_family = SocketDomainToPlatform(ctr_addr.raw.sa_family);

        // We can not guarantee ABI compatibility between platforms so we copy the fields manually
        switch (result.ss_family) {
        case AF_INET: {
            sockaddr_in* result_in = reinterpret_cast<sockaddr_in*>(&result);
            result_in->sin_port = ctr_addr.in.sin_port;
            result_in->sin_addr.s_addr = ctr_addr.in.sin_addr;
            result_len = sizeof(sockaddr_in);
            break;
        }
        case AF_INET6: {
            sockaddr_in6* result_in6 = reinterpret_cast<sockaddr_in6*>(&result);
            result_in6->sin6_port = ctr_addr.in6.sin6_port;
            memcpy(&result_in6->sin6_addr, ctr_addr.in6.sin6_addr.data(),
                   sizeof(result_in6->sin6_addr));
            result_in6->sin6_flowinfo = ctr_addr.in6.sin6_flowinfo;
            result_in6->sin6_scope_id = ctr_addr.in6.sin6_scope_id;
            result_len = sizeof(sockaddr_in6);
            break;
        }
        default:
            ASSERT_MSG(false, "Unhandled address family (sa_family) in CTRSockAddr::ToPlatform");
            break;
        }
        return std::make_pair(result, result_len);
    }

    /// Convert a platform-specific sockaddr to a 3DS CTRSockAddr
    static CTRSockAddr FromPlatform(sockaddr_storage const& addr) {
        CTRSockAddr result;
        result.raw.sa_family = static_cast<u8>(SocketDomainFromPlatform(addr.ss_family));
        // We can not guarantee ABI compatibility between platforms so we copy the fields manually
        switch (addr.ss_family) {
        case AF_INET: {
            sockaddr_in const* addr_in = reinterpret_cast<sockaddr_in const*>(&addr);
            result.raw.len = sizeof(CTRSockAddrIn);
            result.in.sin_port = addr_in->sin_port;
            result.in.sin_addr = addr_in->sin_addr.s_addr;
            break;
        }
        case AF_INET6: {
            sockaddr_in6 const* addr_in6 = reinterpret_cast<sockaddr_in6 const*>(&addr);
            result.raw.len = sizeof(CTRSockAddrIn6);
            result.in6.sin6_port = addr_in6->sin6_port;
            memcpy(result.in6.sin6_addr.data(), &addr_in6->sin6_addr, sizeof(result.in6.sin6_addr));
            result.in6.sin6_flowinfo = addr_in6->sin6_flowinfo;
            result.in6.sin6_scope_id = addr_in6->sin6_scope_id;
            break;
        }
        default:
            ASSERT_MSG(false, "Unhandled address family (sa_family) in CTRSockAddr::ToPlatform");
            break;
        }
        return result;
    }
};

struct CTRAddrInfo {
    s32_le ai_flags;
    s32_le ai_family;
    s32_le ai_socktype;
    s32_le ai_protocol;
    s32_le ai_addrlen;
    char ai_canonname[256];
    CTRSockAddr ai_addr;

    static u32 AddressInfoFlagsToPlatform(u32 flags) {
        u32 ret = 0;
        if (flags & 1) {
            ret |= AI_PASSIVE;
        }
        if (flags & 2) {
            ret |= AI_CANONNAME;
        }
        if (flags & 4) {
            ret |= AI_NUMERICHOST;
        }
        if (flags & 8) {
            ret |= AI_NUMERICSERV;
        }
        return ret;
    }

    static u32 AddressInfoFlagsFromPlatform(u32 flags) {
        u32 ret = 0;
        if (flags & AI_PASSIVE) {
            ret |= 1;
        }
        if (flags & AI_CANONNAME) {
            ret |= 2;
        }
        if (flags & AI_NUMERICHOST) {
            ret |= 4;
        }
        if (flags & AI_NUMERICSERV) {
            ret |= 8;
        }
        return ret;
    }

    /// Converts a platform-specific addrinfo to a 3ds addrinfo.
    static CTRAddrInfo FromPlatform(const addrinfo& addr) {
        CTRAddrInfo ctr_addr{
            .ai_flags = static_cast<s32_le>(AddressInfoFlagsFromPlatform(addr.ai_flags)),
            .ai_family = static_cast<s32_le>(SocketDomainFromPlatform(addr.ai_family)),
            .ai_socktype = static_cast<s32_le>(SocketTypeFromPlatform(addr.ai_socktype)),
            .ai_protocol = static_cast<s32_le>(SocketProtocolFromPlatform(addr.ai_protocol)),
            .ai_addr =
                CTRSockAddr::FromPlatform(*reinterpret_cast<sockaddr_storage*>(addr.ai_addr)),
        };
        ctr_addr.ai_addrlen = static_cast<s32_le>(ctr_addr.ai_addr.raw.len);
        if (addr.ai_canonname)
            std::strncpy(ctr_addr.ai_canonname, addr.ai_canonname,
                         sizeof(ctr_addr.ai_canonname) - 1);
        return ctr_addr;
    }

    /// Converts a platform-specific addrinfo to a 3ds addrinfo.
    static addrinfo ToPlatform(const CTRAddrInfo& ctr_addr) {
        // Only certain fields are meaningful in hints, copy them manually
        addrinfo addr = {
            .ai_flags = static_cast<int>(AddressInfoFlagsToPlatform(ctr_addr.ai_flags)),
            .ai_family = static_cast<int>(SocketDomainToPlatform(ctr_addr.ai_family)),
            .ai_socktype = static_cast<int>(SocketTypeToPlatform(ctr_addr.ai_socktype)),
            .ai_protocol = static_cast<int>(SocketProtocolToPlatform(ctr_addr.ai_protocol)),
        };
        return addr;
    }
};

struct HostByNameData {
    static constexpr u32 max_entries = 24;

    u16_le addr_type;
    u16_le addr_len;
    u16_le addr_count;
    u16_le alias_count;
    std::array<char, 256> h_name;
    std::array<std::array<char, 256>, max_entries> aliases;
    std::array<std::array<u8, 16>, max_entries> addresses;
};
static_assert(sizeof(HostByNameData) == 0x1A88, "Invalid HostByNameData size");

static u32 NameInfoFlagsToPlatform(u32 flags) {
    u32 ret = 0;
    if (flags & 1) {
        ret |= NI_NOFQDN;
    }
    if (flags & 2) {
        ret |= NI_NUMERICHOST;
    }
    if (flags & 4) {
        ret |= NI_NAMEREQD;
    }
    if (flags & 8) {
        ret |= NI_NUMERICSERV;
    }
    if (flags & 16) {
        ret |= NI_DGRAM;
    }
    return ret;
}

static_assert(sizeof(CTRAddrInfo) == 0x130, "Size of CTRAddrInfo is not correct");

void SOC_U::Socket(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 domain = SocketDomainToPlatform(rp.Pop<u32>()); // Address family
    const u32 type = SocketTypeToPlatform(rp.Pop<u32>());
    const u32 protocol = SocketProtocolToPlatform(rp.Pop<u32>());
    const u32 pid = rp.PopPID();

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);

    // Only 0 is allowed according to 3dbrew, using 0 will let the OS decide which protocol to use
    if (protocol != 0) {
        rb.Push(UnimplementedFunction(ErrorModule::SOC)); // TODO(Subv): Correct error code
        rb.Skip(1, false);
        return;
    }

    if (domain != AF_INET) {
        rb.Push(UnimplementedFunction(ErrorModule::SOC)); // TODO(Subv): Correct error code
        rb.Skip(1, false);
        return;
    }

    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        rb.Push(UnimplementedFunction(ErrorModule::SOC)); // TODO(Subv): Correct error code
        rb.Skip(1, false);
        return;
    }

    u64 ret = static_cast<u64>(::socket(domain, type, protocol));
    u32 socketHandle = GetNextSocketID();

    if ((s64)ret != SOCKET_ERROR_VALUE) {
        created_sockets[socketHandle] = {
            .socket_fd = static_cast<decltype(SocketHolder::socket_fd)>(ret),
            .blocking = true,
            .isGlobal = false,
            .shutdown_rd = false,
            .ownerProcess = pid,
        };
#if _WIN32
        // Disable UDP connection reset
        int new_behavior = 0;
        unsigned long bytes_returned = 0;
        WSAIoctl(static_cast<SOCKET>(ret), _WSAIOW(IOC_VENDOR, 12), &new_behavior,
                 sizeof(new_behavior), NULL, 0, &bytes_returned, NULL, NULL);
#endif
    }

    if ((s64)ret == SOCKET_ERROR_VALUE) {
        ret = TranslateError(GET_ERRNO);
    } else {
        ret = socketHandle;
    }

    if (static_cast<s32>(ret) > 0 && created_sockets.contains(socketHandle)) {
        HenOnline::Socket(static_cast<long long>(created_sockets[socketHandle].socket_fd), pid, type == SOCK_DGRAM);
    }
    HEN_SOC("socket: process %u, %s -> %d", pid, type == SOCK_DGRAM ? "UDP" : "TCP", static_cast<s32>(ret));
    LOG_DEBUG(Service_SOC, "called, pid={}, ret={}", pid, static_cast<s32>(ret));

    rb.Push(ResultSuccess);
    rb.Push(static_cast<s32>(ret));
}

void SOC_U::Bind(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 socket_handle = rp.Pop<u32>();
    const u32 len = rp.Pop<u32>();
    const u32 pid = rp.PopPID();
    auto sock_addr_buf = rp.PopStaticBuffer();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    CTRSockAddr ctr_sock_addr;
    std::memcpy(&ctr_sock_addr, sock_addr_buf.data(), std::min<size_t>(len, sizeof(ctr_sock_addr)));

    auto [sock_addr, sock_addr_len] = CTRSockAddr::ToPlatform(ctr_sock_addr);

    s32 ret = ::bind(holder.socket_fd, reinterpret_cast<sockaddr*>(&sock_addr), sock_addr_len);
    {
        const int bind_error = ret != 0 ? GET_ERRNO : 0;
        sockaddr_in asked{};
        std::memcpy(&asked, &sock_addr, std::min(sizeof(asked), sizeof(sock_addr)));
        HenOnline::Bound(static_cast<long long>(holder.socket_fd), asked, bind_error);
        errno = bind_error;
    }

    if (ret != 0)
        ret = TranslateError(GET_ERRNO);

    HEN_SOC("bind: socket %u to %s -> %d", socket_handle,
            HenAddr(reinterpret_cast<sockaddr*>(&sock_addr)).c_str(), ret);
    LOG_DEBUG(Service_SOC, "called, pid={}, fd={}, ret={}", pid, socket_handle,
              static_cast<s32>(ret));

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(ret);
}

void SOC_U::Fcntl(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 socket_handle = rp.Pop<u32>();
    const u32 ctr_cmd = rp.Pop<u32>();
    const u32 ctr_arg = rp.Pop<u32>();
    const u32 pid = rp.PopPID();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    u32 posix_ret = 0; // TODO: Check what hardware returns for F_SETFL (unspecified by POSIX)
    SCOPE_EXIT({
        LOG_DEBUG(Service_SOC, "called, pid={}, fd={}, ret={}", pid, socket_handle,
                  static_cast<s32>(posix_ret));

        IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
        rb.Push(ResultSuccess);
        rb.Push(posix_ret);
    });

    if (ctr_cmd == 3) { // F_GETFL
        posix_ret = 0;
        if (GetSocketBlocking(holder) == false)
            posix_ret |= 4;    // O_NONBLOCK
    } else if (ctr_cmd == 4) { // F_SETFL
        posix_ret = SetSocketBlocking(holder, !(ctr_arg & 4));
    } else {
        LOG_ERROR(Service_SOC, "Unsupported command ({}) in fcntl call", ctr_cmd);
        posix_ret = TranslateError(EINVAL); // TODO: Find the correct error
        return;
    }
}

void SOC_U::Listen(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 socket_handle = rp.Pop<u32>();
    const u32 backlog = rp.Pop<u32>();
    const u32 pid = rp.PopPID();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    s32 ret = ::listen(holder.socket_fd, backlog);
    if (ret != 0)
        ret = TranslateError(GET_ERRNO);

    LOG_DEBUG(Service_SOC, "called, pid={}, fd={}, ret={}", pid, socket_handle,
              static_cast<s32>(ret));

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(ret);
}

void SOC_U::Accept(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const auto socket_handle = rp.Pop<u32>();
    const auto max_addr_len = rp.Pop<u32>();
    const u32 pid = rp.PopPID();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    struct AsyncData {
        // Input
        u32 max_addr_len{};
        SocketHolder* fd_info;
        u32 pid;
        u32 socket_handle;

        // Output
        s32 ret{};
        int accept_error;
        sockaddr_storage addr;
    };

    auto async_data = std::make_shared<AsyncData>();
    async_data->max_addr_len = max_addr_len;
    async_data->fd_info = &holder;
    async_data->pid = pid;
    async_data->socket_handle = socket_handle;

    ctx.RunAsync(
        [async_data](Kernel::HLERequestContext& ctx) {
            socklen_t addr_len = sizeof(async_data->addr);
            async_data->ret = static_cast<u32>(
                ::accept(async_data->fd_info->socket_fd,
                         reinterpret_cast<sockaddr*>(&async_data->addr), &addr_len));
            async_data->accept_error = (async_data->ret == SOCKET_ERROR_VALUE) ? GET_ERRNO : 0;
            return 0;
        },
        [this, async_data](Kernel::HLERequestContext& ctx) {
            if (static_cast<s32>(async_data->ret) != SOCKET_ERROR_VALUE) {
                u32 socketID = GetNextSocketID();
                created_sockets[socketID] = {
                    .socket_fd = static_cast<decltype(SocketHolder::socket_fd)>(async_data->ret),
                    .blocking = true,
                    .isGlobal = false,
                    .shutdown_rd = false,
                    .ownerProcess = async_data->pid,
                };
                async_data->ret = socketID;
            }

            CTRSockAddr ctr_addr;
            std::vector<u8> ctr_addr_buf(sizeof(ctr_addr));
            if (static_cast<s32>(async_data->ret) == SOCKET_ERROR_VALUE) {
                async_data->ret = TranslateError(async_data->accept_error);
            } else {
                ctr_addr = CTRSockAddr::FromPlatform(async_data->addr);
                std::memcpy(ctr_addr_buf.data(), &ctr_addr, sizeof(ctr_addr));
            }

            if (ctr_addr_buf.size() > async_data->max_addr_len) {
                LOG_DEBUG(Service_SOC, "CTRSockAddr is too long, truncating data.");
                ctr_addr_buf.resize(async_data->max_addr_len);
            }

            LOG_DEBUG(Service_SOC, "called, pid={}, fd={}, ret={}", async_data->pid,
                      async_data->socket_handle, static_cast<s32>(async_data->ret));

            IPC::RequestBuilder rb(ctx, 0x04, 2, 2);
            rb.Push(ResultSuccess);
            rb.Push(async_data->ret);
            rb.PushStaticBuffer(std::move(ctr_addr_buf), 0);
        });
}

void SOC_U::SockAtMark(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const auto socket_handle = rp.Pop<u32>();
    const u32 pid = rp.PopPID();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    [[maybe_unused]] SocketHolder& holder = socket_holder_optional->get();

    bool is_at_mark = false;
    int func_res = 0;
#ifdef _WIN32
    u_long atMark = 0;
    func_res = ::ioctlsocket(holder.socket_fd, SIOCATMARK, &atMark);
    is_at_mark = atMark != 0;
#else
#ifdef ANDROID
    func_res = 0;
    LOG_WARNING(Service_SOC, "(STUBBED) called");
#else
    func_res = ::sockatmark(holder.socket_fd);
#endif
    is_at_mark = func_res > 0;
#endif // _WIN32

    u32 ret;
    if (func_res == SOCKET_ERROR_VALUE) {
        ret = TranslateError(GET_ERRNO);
    } else {
        ret = is_at_mark ? 1 : 0;
    }

    LOG_POLL(Service_SOC, "called, pid={}, fd={}, ret={}", pid, socket_handle,
             static_cast<s32>(ret));

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(ret);
}

void SOC_U::GetHostId(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    u32 host_id = 0;
    auto info = GetDefaultInterfaceInfo();
    if (info.has_value()) {
        host_id = info->address;
    }

    HEN_SOC("gethostid -> %u.%u.%u.%u", host_id & 0xFF, (host_id >> 8) & 0xFF, (host_id >> 16) & 0xFF,
            host_id >> 24);
    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(host_id);
}

void SOC_U::Close(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 socket_handle = rp.Pop<u32>();
    const u32 pid = rp.PopPID();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    s32 ret = 0;
    HenOnline::Closed(static_cast<long long>(holder.socket_fd));
    ret = closesocket(holder.socket_fd);

    if (ret != 0) {
        ret = TranslateError(GET_ERRNO);
    } else {
        created_sockets.erase(socket_handle);
    }

    HEN_SOC("close: socket %u -> %d", socket_handle, ret);
    LOG_DEBUG(Service_SOC, "pid={}, fd={}, ret={}", pid, socket_handle, static_cast<s32>(ret));

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(ret);
}

void SOC_U::SendToOther(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 socket_handle = rp.Pop<u32>();
    const u32 len = rp.Pop<u32>();
    u32 flags = SendRecvFlagsToPlatform(rp.Pop<u32>());
    const u32 addr_len = rp.Pop<u32>();
    const u32 pid = rp.PopPID();
    const auto dest_addr_buffer = rp.PopStaticBuffer();
    auto input_mapped_buff = rp.PopMappedBuffer();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    bool dont_wait = (flags & MSGCUSTOM_HANDLE_DONTWAIT) != 0;
    flags &= ~MSGCUSTOM_HANDLE_DONTWAIT;
#ifdef _WIN32
    bool was_blocking = GetSocketBlocking(holder);
    if (dont_wait && was_blocking) {
        SetSocketBlocking(holder, false);
    }
#else
    if (dont_wait) {
        flags |= MSG_DONTWAIT;
    }
#endif // _WIN32
    std::vector<u8> input_buff(len);
    input_mapped_buff.Read(input_buff.data(), 0,
                           std::min(input_mapped_buff.GetSize(), static_cast<std::size_t>(len)));

    s32 ret = -1;
    if (addr_len > 0) {
        CTRSockAddr ctr_dest_addr;
        std::memcpy(&ctr_dest_addr, dest_addr_buffer.data(),
                    std::min<size_t>(addr_len, sizeof(ctr_dest_addr)));
        auto [dest_addr, dest_addr_len] = CTRSockAddr::ToPlatform(ctr_dest_addr);
        ret = static_cast<s32>(
            ::sendto(holder.socket_fd, reinterpret_cast<const char*>(input_buff.data()), len, flags,
                     reinterpret_cast<sockaddr*>(&dest_addr), dest_addr_len));
    } else {
        ret = static_cast<s32>(::sendto(holder.socket_fd,
                                        reinterpret_cast<const char*>(input_buff.data()), len,
                                        flags, nullptr, 0));
    }

    const auto send_error = (ret == SOCKET_ERROR_VALUE) ? GET_ERRNO : 0;
    {
        u32 to_ip = 0;
        const bool has_to = addr_len >= 8 && dest_addr_buffer.size() >= 8;
        if (has_to) {
            std::memcpy(&to_ip, dest_addr_buffer.data() + 4, 4);
        }
        HenOnline::Sent(static_cast<long long>(holder.socket_fd), has_to, to_ip,
                        has_to ? static_cast<u16>((dest_addr_buffer[2] << 8) | dest_addr_buffer[3]) : u16(0), ret, send_error);
    }
    HEN_SOC("sendto: socket %u, %u bytes to %s, flags %x -> %d%s", socket_handle, len,
            addr_len >= 8 ? HenCtrAddr(dest_addr_buffer).c_str() : "(its peer)", flags, ret,
            ret < 0 ? fmt::format(" (host errno {})", send_error).c_str() : "");

#ifdef _WIN32
    if (dont_wait && was_blocking) {
        SetSocketBlocking(holder, true);
    }
#endif

    if (ret == SOCKET_ERROR_VALUE) {
        ret = TranslateError(send_error);
    }

    LOG_SEND_RECV(Service_SOC, "called, fd={}, ret={}", socket_handle, static_cast<s32>(ret));

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);
    rb.Push(ResultSuccess);
    rb.Push(ret);
    rb.PushMappedBuffer(input_mapped_buff);
}

s32 SOC_U::SendToImpl(SocketHolder& holder, u32 len, u32 flags, u32 addr_len,
                      const std::vector<u8>& input_buff, const u8* dest_addr_buff) {

    bool dont_wait = (flags & MSGCUSTOM_HANDLE_DONTWAIT) != 0;
    flags &= ~MSGCUSTOM_HANDLE_DONTWAIT;
#ifdef _WIN32
    bool was_blocking = GetSocketBlocking(holder);
    if (dont_wait && was_blocking) {
        SetSocketBlocking(holder, false);
    }
#else
    if (dont_wait) {
        flags |= MSG_DONTWAIT;
    }
#endif // _WIN32

    s32 ret = -1;
    if (addr_len > 0) {
        CTRSockAddr ctr_dest_addr;
        std::memcpy(&ctr_dest_addr, dest_addr_buff,
                    std::min<size_t>(addr_len, sizeof(ctr_dest_addr)));
        auto [dest_addr, dest_addr_len] = CTRSockAddr::ToPlatform(ctr_dest_addr);
        ret = static_cast<s32>(
            ::sendto(holder.socket_fd, reinterpret_cast<const char*>(input_buff.data()), len, flags,
                     reinterpret_cast<sockaddr*>(&dest_addr), dest_addr_len));
    } else {
        ret = static_cast<s32>(::sendto(holder.socket_fd,
                                        reinterpret_cast<const char*>(input_buff.data()), len,
                                        flags, nullptr, 0));
    }

    auto send_error = (ret == SOCKET_ERROR_VALUE) ? GET_ERRNO : 0;
    {
        u32 to_ip = 0;
        const bool has_to = addr_len >= 8 && dest_addr_buff != nullptr;
        if (has_to) {
            std::memcpy(&to_ip, dest_addr_buff + 4, 4);
        }
        HenOnline::Sent(static_cast<long long>(holder.socket_fd), has_to, to_ip,
                        has_to ? static_cast<u16>((dest_addr_buff[2] << 8) | dest_addr_buff[3]) : u16(0), ret, send_error);
    }
    HEN_SOC("sendto: fd %d, %u bytes to %s, flags %x -> %d%s", static_cast<int>(holder.socket_fd), len,
            addr_len >= 8 ? HenCtrAddr(std::vector<u8>(dest_addr_buff, dest_addr_buff + 8)).c_str()
                          : "(its peer)",
            flags, ret, ret < 0 ? fmt::format(" (host errno {})", send_error).c_str() : "");

#ifdef _WIN32
    if (dont_wait && was_blocking) {
        SetSocketBlocking(holder, true);
    }
#endif

    if (ret == SOCKET_ERROR_VALUE)
        ret = TranslateError(send_error);

    return ret;
}

void SOC_U::SendToSingle(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 socket_handle = rp.Pop<u32>();
    u32 len = rp.Pop<u32>();
    u32 flags = SendRecvFlagsToPlatform(rp.Pop<u32>());
    u32 addr_len = rp.Pop<u32>();
    const u32 pid = rp.PopPID();
    auto input_buff = rp.PopStaticBuffer();
    auto dest_addr_buff = rp.PopStaticBuffer();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    s32 ret = SendToImpl(holder, len, flags, addr_len, input_buff, dest_addr_buff.data());

    LOG_SEND_RECV(Service_SOC, "called, fd={}, ret={}", socket_handle, static_cast<s32>(ret));

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(ret);
}

// A poll busy loop has to be used when recieving from a blocking socket
// because calling shutdown on a socket that's currently in a blocking
// recv call does not make it return, which causes some games to hang.
void SOC_U::RecvBusyWaitForEvent(SocketHolder& holder) {
    constexpr int timeout_ms = 100;

    while (true) {
        pollfd poll_fd{};
        poll_fd.fd = holder.socket_fd;
        poll_fd.events = POLLIN; // Check for available data.
        int res = ::poll(&poll_fd, 1, timeout_ms);
        // Break if there is any event, error or socket RD shutdown.
        if (res != 0 || holder.shutdown_rd) {
            break;
        }
    }
}

void SOC_U::RecvFromOther(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 socket_handle = rp.Pop<u32>();
    u32 len = rp.Pop<u32>();
    u32 flags = SendRecvFlagsToPlatform(rp.Pop<u32>());
    u32 addr_len = rp.Pop<u32>();
    const u32 pid = rp.PopPID();
    Kernel::MappedBuffer& buffer = rp.PopMappedBuffer();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    bool dont_wait = (flags & MSGCUSTOM_HANDLE_DONTWAIT) != 0;
    flags &= ~MSGCUSTOM_HANDLE_DONTWAIT;
#ifdef _WIN32
    bool was_blocking = GetSocketBlocking(holder);
    if (dont_wait && was_blocking) {
        SetSocketBlocking(holder, false);
    }
#else
    if (dont_wait) {
        flags |= MSG_DONTWAIT;
    }
#endif // _WIN32

    bool needs_async = GetSocketBlocking(holder) && !dont_wait;
    struct AsyncData {
        // Input
        u32 len{};
        u32 flags{};
        u32 addr_len{};
        SocketHolder* fd_info;
        u32 socket_handle;
#ifdef _WIN32
        bool dont_wait;
        bool was_blocking;
#endif
        bool is_blocking;

        // Output
        s32 ret{};
        int recv_error;
        Kernel::MappedBuffer* buffer;
        std::vector<u8> output_buff;
        std::vector<u8> addr_buff;
    };

    auto async_data = std::make_shared<AsyncData>();
    async_data->buffer = &buffer;
    async_data->ret = -1;
    async_data->len = len;
    async_data->flags = flags;
    async_data->addr_len = addr_len;
    async_data->output_buff.resize(len);
    async_data->addr_buff.resize(addr_len);
    async_data->fd_info = &holder;
    async_data->socket_handle = socket_handle;
#ifdef _WIN32
    async_data->dont_wait = dont_wait;
    async_data->was_blocking = was_blocking;
#endif
    async_data->is_blocking = needs_async;

    ctx.RunAsync(
        [async_data](Kernel::HLERequestContext& ctx) {
            sockaddr_storage src_addr;
            socklen_t src_addr_len = sizeof(src_addr);
            CTRSockAddr ctr_src_addr;
            // Windows, why do you have to be so special...
            if (async_data->is_blocking) {
                RecvBusyWaitForEvent(*async_data->fd_info);
            }
            if (async_data->addr_len > 0) {
                async_data->ret = static_cast<s32>(::recvfrom(
                    async_data->fd_info->socket_fd,
                    reinterpret_cast<char*>(async_data->output_buff.data()), async_data->len,
                    async_data->flags, reinterpret_cast<sockaddr*>(&src_addr), &src_addr_len));
                if (async_data->ret >= 0 && src_addr_len > 0) {
                    ctr_src_addr = CTRSockAddr::FromPlatform(src_addr);
                    std::memcpy(async_data->addr_buff.data(), &ctr_src_addr,
                                std::min<size_t>(async_data->addr_len, sizeof(ctr_src_addr)));
                }
            } else {
                async_data->ret = static_cast<s32>(
                    ::recvfrom(async_data->fd_info->socket_fd,
                               reinterpret_cast<char*>(async_data->output_buff.data()),
                               async_data->len, async_data->flags, NULL, 0));
                async_data->addr_buff.resize(0);
            }
            async_data->recv_error = (async_data->ret == SOCKET_ERROR_VALUE) ? GET_ERRNO : 0;
            return 0;
        },
        [this, async_data](Kernel::HLERequestContext& ctx) {
            if (async_data->ret == SOCKET_ERROR_VALUE) {
                async_data->ret = TranslateError(async_data->recv_error);
            } else {
                async_data->buffer->Write(async_data->output_buff.data(), 0, async_data->ret);
            }
#ifdef _WIN32
            if (async_data->dont_wait && async_data->was_blocking) {
                SetSocketBlocking(*async_data->fd_info, true);
            }
#else
            (void)this;
#endif
            {
                // (the address is as the emulated console gets it: port at 2, four bytes at 4)
                u32 from_ip = 0;
                u16 from_port = 0;
                if (async_data->addr_buff.size() >= 8) {
                    std::memcpy(&from_ip, async_data->addr_buff.data() + 4, 4);
                    from_port = static_cast<u16>((async_data->addr_buff[2] << 8) | async_data->addr_buff[3]);
                }
                HenOnline::Received(static_cast<long long>(async_data->fd_info->socket_fd), from_ip, from_port,
                                    async_data->recv_error != 0 ? -1 : static_cast<int>(async_data->ret), async_data->recv_error,
                                    async_data->output_buff.data());
            }
#if defined(__SWITCH__) && HEN_DEV
            if (hen_trace_sockets) {
                u32& quiet = hen_soc_quiet[async_data->socket_handle];
                if (async_data->ret == -6) {
                    if (quiet++ % 200 == 0) {
                        HenSoc("recvfrom: socket %u, room for %u bytes: nothing there (EAGAIN -> -6), %u time(s) in a row",
                               async_data->socket_handle, static_cast<u32>(async_data->len), quiet);
                    }
                } else {
                    HenSoc("recvfrom: socket %u, room for %u bytes -> %d from %s%s%s", async_data->socket_handle,
                           static_cast<u32>(async_data->len), static_cast<s32>(async_data->ret),
                           HenCtrAddr(async_data->addr_buff).c_str(),
                           quiet != 0 ? fmt::format(" (after {} with nothing there)", quiet).c_str() : "",
                           async_data->ret < 0 ? fmt::format(" (host errno {})", async_data->recv_error).c_str() : "");
                    quiet = 0;
                }
            }
#endif
            LOG_SEND_RECV(Service_SOC, "called, fd={}, ret={}", async_data->socket_handle,
                          static_cast<s32>(async_data->ret));

            IPC::RequestBuilder rb(ctx, 0x07, 2, 4);
            rb.Push(ResultSuccess);
            rb.Push(async_data->ret);
            rb.PushStaticBuffer(std::move(async_data->addr_buff), 0);
            rb.PushMappedBuffer(*async_data->buffer);
        },
        needs_async);
}

void SOC_U::RecvFrom(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 socket_handle = rp.Pop<u32>();
    u32 len = rp.Pop<u32>();
    u32 flags = SendRecvFlagsToPlatform(rp.Pop<u32>());
    u32 addr_len = rp.Pop<u32>();
    const u32 pid = rp.PopPID();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    bool dont_wait = (flags & MSGCUSTOM_HANDLE_DONTWAIT) != 0;
    flags &= ~MSGCUSTOM_HANDLE_DONTWAIT;
#ifdef _WIN32
    bool was_blocking = GetSocketBlocking(holder);
    if (dont_wait && was_blocking) {
        SetSocketBlocking(holder, false);
    }
#else
    if (dont_wait) {
        flags |= MSG_DONTWAIT;
    }
#endif // _WIN32

    bool needs_async = GetSocketBlocking(holder) && !dont_wait;
    struct AsyncData {
        // Input
        u32 len{};
        u32 flags{};
        u32 addr_len{};
        SocketHolder* fd_info;
        u32 socket_handle;
#ifdef _WIN32
        bool dont_wait;
        bool was_blocking;
#endif
        bool is_blocking;

        // Output
        s32 ret{};
        int recv_error;
        std::vector<u8> output_buff;
        std::vector<u8> addr_buff;
    };

    auto async_data = std::make_shared<AsyncData>();
    async_data->ret = -1;
    async_data->len = len;
    async_data->flags = flags;
    async_data->addr_len = addr_len;
    async_data->output_buff.resize(len);
    async_data->addr_buff.resize(addr_len);
    async_data->fd_info = &holder;
    async_data->socket_handle = socket_handle;
#ifdef _WIN32
    async_data->dont_wait = dont_wait;
    async_data->was_blocking = was_blocking;
#endif
    async_data->is_blocking = needs_async;

    ctx.RunAsync(
        [async_data](Kernel::HLERequestContext& ctx) {
            sockaddr_storage src_addr;
            socklen_t src_addr_len = sizeof(src_addr);
            CTRSockAddr ctr_src_addr;
            if (async_data->is_blocking) {
                RecvBusyWaitForEvent(*async_data->fd_info);
            }
            if (async_data->addr_len > 0) {
                // Only get src adr if input adr available
                async_data->ret = static_cast<s32>(::recvfrom(
                    async_data->fd_info->socket_fd,
                    reinterpret_cast<char*>(async_data->output_buff.data()), async_data->len,
                    async_data->flags, reinterpret_cast<sockaddr*>(&src_addr), &src_addr_len));
                if (async_data->ret >= 0 && src_addr_len > 0) {
                    ctr_src_addr = CTRSockAddr::FromPlatform(src_addr);
                    std::memcpy(async_data->addr_buff.data(), &ctr_src_addr,
                                std::min<size_t>(async_data->addr_len, sizeof(ctr_src_addr)));
                }
            } else {
                async_data->ret = static_cast<s32>(
                    ::recvfrom(async_data->fd_info->socket_fd,
                               reinterpret_cast<char*>(async_data->output_buff.data()),
                               async_data->len, async_data->flags, NULL, 0));
                async_data->addr_buff.resize(0);
            }
            async_data->recv_error = (async_data->ret == SOCKET_ERROR_VALUE) ? GET_ERRNO : 0;
            return 0;
        },
        [this, async_data](Kernel::HLERequestContext& ctx) {

#ifdef _WIN32
            if (async_data->dont_wait && async_data->was_blocking) {
                SetSocketBlocking(*async_data->fd_info, true);
            }
#else
            (void)this;
#endif
            s32 total_received = async_data->ret;
            if (async_data->ret == SOCKET_ERROR_VALUE) {
                async_data->ret = TranslateError(async_data->recv_error);
                total_received = 0;
            }

            // Write only the data we received to avoid overwriting parts of the buffer with zeros
            async_data->output_buff.resize(total_received);

            {
                // (the address is as the emulated console gets it: port at 2, four bytes at 4)
                u32 from_ip = 0;
                u16 from_port = 0;
                if (async_data->addr_buff.size() >= 8) {
                    std::memcpy(&from_ip, async_data->addr_buff.data() + 4, 4);
                    from_port = static_cast<u16>((async_data->addr_buff[2] << 8) | async_data->addr_buff[3]);
                }
                HenOnline::Received(static_cast<long long>(async_data->fd_info->socket_fd), from_ip, from_port,
                                    async_data->recv_error != 0 ? -1 : static_cast<int>(async_data->ret), async_data->recv_error,
                                    async_data->output_buff.data());
            }
#if defined(__SWITCH__) && HEN_DEV
            if (hen_trace_sockets) {
                u32& quiet = hen_soc_quiet[async_data->socket_handle];
                if (async_data->ret == -6) {
                    if (quiet++ % 200 == 0) {
                        HenSoc("recvfrom: socket %u, room for %u bytes: nothing there (EAGAIN -> -6), %u time(s) in a row",
                               async_data->socket_handle, static_cast<u32>(async_data->len), quiet);
                    }
                } else {
                    HenSoc("recvfrom: socket %u, room for %u bytes -> %d from %s%s%s", async_data->socket_handle,
                           static_cast<u32>(async_data->len), static_cast<s32>(async_data->ret),
                           HenCtrAddr(async_data->addr_buff).c_str(),
                           quiet != 0 ? fmt::format(" (after {} with nothing there)", quiet).c_str() : "",
                           async_data->ret < 0 ? fmt::format(" (host errno {})", async_data->recv_error).c_str() : "");
                    quiet = 0;
                }
            }
#endif
            LOG_SEND_RECV(Service_SOC, "called, fd={}, ret={}", async_data->socket_handle,
                          static_cast<s32>(async_data->ret));

            IPC::RequestBuilder rb(ctx, 0x08, 3, 4);
            rb.Push(ResultSuccess);
            rb.Push(async_data->ret);
            rb.Push(total_received);
            rb.PushStaticBuffer(std::move(async_data->output_buff), 0);
            rb.PushStaticBuffer(std::move(async_data->addr_buff), 1);
        },
        needs_async);
}

void SOC_U::Poll(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    u32 nfds = rp.Pop<u32>();
    s32 timeout = rp.Pop<s32>();
    const u32 pid = rp.PopPID();
    auto input_fds = rp.PopStaticBuffer();

    struct AsyncData {
        // Input
        s32 timeout;
        u32 nfds;

        // Input/Output
        std::vector<pollfd> platform_pollfd;
        std::vector<u8> has_libctru_bug;
        std::vector<CTRPollFD> ctr_fds;

        // Output
        s32 ret;
        int poll_error;
    };
    auto async_data = std::make_shared<AsyncData>();
    async_data->timeout = timeout;
    async_data->nfds = nfds;

    async_data->ctr_fds.resize(nfds);
    std::memcpy(async_data->ctr_fds.data(), input_fds.data(), nfds * sizeof(CTRPollFD));

    // The 3ds_pollfd and the pollfd structures may be different (Windows/Linux have different
    // sizes)
    // so we have to copy the data in order
    async_data->platform_pollfd.resize(nfds);
    async_data->has_libctru_bug.resize(nfds, false);
    for (u32 i = 0; i < nfds; i++) {
        if (!GetSocketHolder(async_data->ctr_fds[i].fd, pid, rp)) {
            return;
        }
        async_data->platform_pollfd[i] =
            CTRPollFD::ToPlatform(*this, async_data->ctr_fds[i], async_data->has_libctru_bug[i]);
    }

    ctx.RunAsync(
        [async_data](Kernel::HLERequestContext& ctx) {
            async_data->ret =
                ::poll(async_data->platform_pollfd.data(), async_data->nfds, async_data->timeout);
            if (async_data->ret == SOCKET_ERROR_VALUE) {
                async_data->poll_error = GET_ERRNO;
            }
            return 0;
        },
        [this, async_data](Kernel::HLERequestContext& ctx) {
            // Now update the output 3ds_pollfd structure
            for (u32 i = 0; i < async_data->nfds; i++) {
                async_data->ctr_fds[i] = CTRPollFD::FromPlatform(
                    *this, async_data->platform_pollfd[i], async_data->has_libctru_bug[i]);
            }

            std::vector<u8> output_fds(async_data->nfds * sizeof(CTRPollFD));
            std::memcpy(output_fds.data(), async_data->ctr_fds.data(),
                        async_data->nfds * sizeof(CTRPollFD));

            if (async_data->ret == SOCKET_ERROR_VALUE) {
                async_data->ret = TranslateError(async_data->poll_error);
            }

            IPC::RequestBuilder rb(ctx, static_cast<u16>(ctx.CommandHeader().command_id.Value()), 2,
                                   2);
            rb.Push(ResultSuccess);
            rb.Push(async_data->ret);
            rb.PushStaticBuffer(std::move(output_fds), 0);

#if defined(__SWITCH__) && HEN_DEV
            if (hen_trace_sockets) {
                if (async_data->ret == 0) {
                    if (hen_poll_quiet++ % 200 == 0) {
                        HenSoc("poll: %u socket(s), first %u, timeout %d ms: nothing ready, %u time(s) in a row",
                               async_data->nfds, async_data->nfds ? static_cast<u32>(async_data->ctr_fds[0].fd) : 0u,
                               static_cast<int>(async_data->timeout), hen_poll_quiet);
                    }
                } else {
                    std::string ready;
                    for (u32 i = 0; i < async_data->nfds && i < 8; i++) {
                        ready += fmt::format(" {}:{:x}", static_cast<u32>(async_data->ctr_fds[i].fd),
                                             static_cast<u32>(async_data->platform_pollfd[i].revents));
                    }
                    HenSoc("poll: %u socket(s), timeout %d ms -> %d; socket:host revents%s%s", async_data->nfds,
                           static_cast<int>(async_data->timeout), static_cast<s32>(async_data->ret), ready.c_str(),
                           hen_poll_quiet != 0 ? fmt::format(" (after {} with nothing ready)", hen_poll_quiet).c_str() : "");
                    hen_poll_quiet = 0;
                }
            }
#endif
            LOG_POLL(Service_SOC, "called, fd_count={}, ret={}", async_data->nfds,
                     static_cast<s32>(async_data->ret));
        },
        timeout != 0);
}

void SOC_U::GetSockName(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const auto socket_handle = rp.Pop<u32>();
    const auto max_addr_len = rp.Pop<u32>();
    const u32 pid = rp.PopPID();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    sockaddr_storage dest_addr;
    socklen_t dest_addr_len = sizeof(dest_addr);
    s32 ret =
        ::getsockname(holder.socket_fd, reinterpret_cast<sockaddr*>(&dest_addr), &dest_addr_len);

    CTRSockAddr ctr_dest_addr = CTRSockAddr::FromPlatform(dest_addr);
    std::vector<u8> dest_addr_buff(sizeof(ctr_dest_addr));
    std::memcpy(dest_addr_buff.data(), &ctr_dest_addr, sizeof(ctr_dest_addr));

    if (ret != 0)
        ret = TranslateError(GET_ERRNO);

    if (dest_addr_buff.size() > max_addr_len) {
        LOG_DEBUG(Service_SOC, "CTRSockAddr is too long, truncating data.");
        dest_addr_buff.resize(max_addr_len);
    }

    HEN_SOC("getsockname: socket %u -> %d, %s", socket_handle, ret,
            HenAddr(reinterpret_cast<sockaddr*>(&dest_addr)).c_str());
    LOG_DEBUG(Service_SOC, "called, pid={}, fd={}, ret={}", pid, socket_handle,
              static_cast<s32>(ret));

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);
    rb.Push(ResultSuccess);
    rb.Push(ret);
    rb.PushStaticBuffer(std::move(dest_addr_buff), 0);
}

void SOC_U::Shutdown(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 socket_handle = rp.Pop<u32>();
    s32 how = ShutdownHowToPlatform(rp.Pop<s32>());
    const u32 pid = rp.PopPID();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    s32 ret = ::shutdown(holder.socket_fd, how);
    if (ret != 0) {
        ret = TranslateError(GET_ERRNO);
    } else {
        if (how == SHUT_RD || how == SHUT_RDWR) {
            holder.shutdown_rd = true;
        }
    }

    LOG_DEBUG(Service_SOC, "called, pid={}, fd={}, ret={}", pid, socket_handle,
              static_cast<s32>(ret));

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(ret);
}

// (Kaizhar, hen_soc_rules) Names the emulated console looks up go through the
// HTTP service's URL replacement rules first, as its HTTP requests' hosts do.
// Why: on a console set to Pretendo, Pretendo's patch of the socket module rewrites the NAT
// check servers' names in gethostbyname and getaddrinfo (nncs1/nncs2.app.nintendowifi.net ->
// .app.pretendo.cc; github.com/PretendoNetwork/nimbus, patches/socket). On Azahar Nimbus only
// registers rules with the HTTP service ("nintendowifi\.net" -> "pretendo.cc" and two more), and
// this service is not the console's own module, so the patch never applies: Mario Kart 7 looks
// up nncs1.app.nintendowifi.net, sends its six 16-byte NAT probes there and gets no answer (a
// home network that blocks Nintendo's names answers 127.0.0.1; one that does not sends the
// probes to Nintendo). With the rules applied the name becomes nncs1.app.pretendo.cc.
// No rules (Nintendo selected in Nimbus, or never used): names stay as they are.
// --no-soc-rules: not done, to compare.
} // namespace Service::SOC
namespace Service::HTTP {
std::string HenApplyUrlRules(const std::string& host);
}
extern "C" {
int hen_soc_rules = 1;
}
namespace Service::SOC {
static std::string HenLookupName(const char* name) {
    std::string asked(name);
    if (!hen_soc_rules || asked.empty()) {
        return asked;
    }
    std::string now = Service::HTTP::HenApplyUrlRules(asked);
    if (now != asked) {
        LOG_INFO(Service_SOC, "name lookup: {} is looked up as {} (the URL replacement rules)", asked, now);
    }
    return now;
}

// (Kaizhar) A frontend's own answer to a name the emulated console looks up, asked before the
// platform's resolver: 1 with an IPv4 address in network byte order. Null (always, outside
// "Link a 3DS" on the Switch 2 port: citra_switch/guest_dns.h): no such thing. The HTTP
// service asks it too (http_c.cpp).
extern "C" {
int (*hen_guest_lookup)(const char* name, unsigned* ipv4) = nullptr;
}

static std::vector<u8> HostEntToHostByNameData(struct hostent* result) {
    std::vector<u8> hbn_data_out(sizeof(HostByNameData));
    HostByNameData& hbn_data = *reinterpret_cast<HostByNameData*>(hbn_data_out.data());

    hbn_data.addr_type = result->h_addrtype;
    hbn_data.addr_len = result->h_length;
    std::strncpy(hbn_data.h_name.data(), result->h_name, 255);
    u16 count;
    for (count = 0; count < HostByNameData::max_entries; count++) {
        char* curr = result->h_aliases[count];
        if (!curr) {
            break;
        }
        std::strncpy(hbn_data.aliases[count].data(), curr, 255);
    }
    hbn_data.alias_count = count;
    for (count = 0; count < HostByNameData::max_entries; count++) {
        char* curr = result->h_addr_list[count];
        if (!curr) {
            break;
        }
        std::memcpy(hbn_data.addresses[count].data(), curr, result->h_length);
    }
    hbn_data.addr_count = count;

    return hbn_data_out;
}

void SOC_U::GetHostByAddr(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    [[maybe_unused]] int addr_len = static_cast<int>(rp.Pop<u32>());
    int type = static_cast<int>(SocketDomainToPlatform(rp.Pop<u32>()));
    [[maybe_unused]] u32 out_buf_len = rp.Pop<u32>();
    auto addr = rp.PopStaticBuffer();

    auto [platform_addr, platform_addr_len] =
        CTRSockAddr::ToPlatform(*reinterpret_cast<CTRSockAddr*>(addr.data()));

    struct hostent* result =
        ::gethostbyaddr(reinterpret_cast<char*>(&platform_addr), platform_addr_len, type);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);
    rb.Push(ResultSuccess);
    s32 ret;
    if (!result) {
        rb.Push(ret = TranslateError(GET_ERRNO));
    } else {
        rb.Push(ret = 0);
        rb.PushStaticBuffer(HostEntToHostByNameData(result), 0);
    }

    LOG_DEBUG(Service_SOC, "called, ret={}", static_cast<s32>(ret));
}

void SOC_U::GetHostByName(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    [[maybe_unused]] u32 name_len = rp.Pop<u32>();
    [[maybe_unused]] u32 out_buf_len = rp.Pop<u32>();
    auto host_name = rp.PopStaticBuffer();

    struct hostent* result = nullptr;
    // (Kaizhar) the frontend's answer, if it has one (hen_guest_lookup above)
    struct hostent own {};
    in_addr own_address{};
    char* own_addresses[2] = {reinterpret_cast<char*>(&own_address), nullptr};
    char* own_aliases[1] = {nullptr};
    unsigned own_ipv4 = 0;
    if (hen_guest_lookup != nullptr && !host_name.empty() &&
        std::find(host_name.begin(), host_name.end(), u8{0}) != host_name.end() &&
        hen_guest_lookup(reinterpret_cast<const char*>(host_name.data()), &own_ipv4) == 1) {
        own_address.s_addr = own_ipv4;
        own.h_name = reinterpret_cast<char*>(host_name.data());
        own.h_aliases = own_aliases;
        own.h_addrtype = AF_INET;
        own.h_length = sizeof(own_address);
        own.h_addr_list = own_addresses;
        result = &own;
    } else if (std::find(host_name.begin(), host_name.end(), u8{0}) != host_name.end()) {
        const std::string looked_up = HenLookupName(reinterpret_cast<const char*>(host_name.data()));
        result = ::gethostbyname(looked_up.c_str());
        HenOnline::LookedUp(reinterpret_cast<const char*>(host_name.data()), looked_up.c_str(), result);
        HEN_SOC("gethostbyname: %s%s -> %s", reinterpret_cast<const char*>(host_name.data()),
                looked_up != reinterpret_cast<const char*>(host_name.data())
                    ? fmt::format(" (looked up as {})", looked_up).c_str()
                    : "",
                result != nullptr && result->h_addr_list != nullptr && result->h_addr_list[0] != nullptr
                    ? fmt::format("{}.{}.{}.{}", static_cast<u8>(result->h_addr_list[0][0]),
                                  static_cast<u8>(result->h_addr_list[0][1]),
                                  static_cast<u8>(result->h_addr_list[0][2]),
                                  static_cast<u8>(result->h_addr_list[0][3]))
                          .c_str()
                    : "no address");
    } else {
        result = ::gethostbyname(reinterpret_cast<char*>(host_name.data()));
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);
    rb.Push(ResultSuccess);
    s32 ret;
    if (!result) {
        rb.Push(ret = TranslateError(GET_ERRNO));
    } else {
        rb.Push(ret = 0);
        rb.PushStaticBuffer(HostEntToHostByNameData(result), 0);
    }

    LOG_DEBUG(Service_SOC, "called, ret={}", static_cast<s32>(ret));
}

void SOC_U::GetPeerName(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const auto socket_handle = rp.Pop<u32>();
    const auto max_addr_len = rp.Pop<u32>();
    const u32 pid = rp.PopPID();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    sockaddr_storage dest_addr;
    socklen_t dest_addr_len = sizeof(dest_addr);
    const int ret =
        ::getpeername(holder.socket_fd, reinterpret_cast<sockaddr*>(&dest_addr), &dest_addr_len);

    CTRSockAddr ctr_dest_addr = CTRSockAddr::FromPlatform(dest_addr);
    std::vector<u8> dest_addr_buff(sizeof(ctr_dest_addr));
    std::memcpy(dest_addr_buff.data(), &ctr_dest_addr, sizeof(ctr_dest_addr));

    int result = 0;
    if (ret != 0) {
        result = TranslateError(GET_ERRNO);
    }

    if (dest_addr_buff.size() > max_addr_len) {
        LOG_DEBUG(Service_SOC, "CTRSockAddr is too long, truncating data.");
        dest_addr_buff.resize(max_addr_len);
    }

    LOG_DEBUG(Service_SOC, "called, pid={}, fd={}, ret={}", pid, socket_handle,
              static_cast<s32>(result));

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);
    rb.Push(ResultSuccess);
    rb.Push(result);
    rb.PushStaticBuffer(std::move(dest_addr_buff), 0);
}

void SOC_U::Connect(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const auto socket_handle = rp.Pop<u32>();
    const auto input_addr_len = rp.Pop<u32>();
    const u32 pid = rp.PopPID();
    auto input_addr_buf = rp.PopStaticBuffer();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    struct AsyncData {
        // Input
        SocketHolder* fd_info;
        std::pair<sockaddr_storage, socklen_t> input_addr;
        u32 socket_handle;
        u32 pid;

        // Output
        s32 ret{};
        int connect_error;
    };

    auto async_data = std::make_shared<AsyncData>();
    async_data->fd_info = &holder;
    async_data->pid = pid;

    CTRSockAddr ctr_input_addr;
    std::memcpy(&ctr_input_addr, input_addr_buf.data(),
                std::min<size_t>(input_addr_len, sizeof(ctr_input_addr)));

    async_data->input_addr = CTRSockAddr::ToPlatform(ctr_input_addr);
    async_data->socket_handle = socket_handle;

    ctx.RunAsync(
        [async_data](Kernel::HLERequestContext& ctx) {
            async_data->ret = ::connect(async_data->fd_info->socket_fd,
                                        reinterpret_cast<sockaddr*>(&async_data->input_addr.first),
                                        async_data->input_addr.second);
            async_data->connect_error = (async_data->ret == SOCKET_ERROR_VALUE) ? GET_ERRNO : 0;
            return 0;
        },
        [async_data](Kernel::HLERequestContext& ctx) {
            {
                sockaddr_in to{};
                std::memcpy(&to, &async_data->input_addr.first, std::min(sizeof(to), sizeof(async_data->input_addr.first)));
                HenOnline::Connected(static_cast<long long>(async_data->fd_info->socket_fd), to,
                                     async_data->ret != 0 ? async_data->connect_error : 0);
            }
            if (async_data->ret != 0) {
                async_data->ret = TranslateError(async_data->connect_error);
            }

            HEN_SOC("connect: socket %u to %s -> %d", async_data->socket_handle,
                    HenAddr(reinterpret_cast<sockaddr*>(&async_data->input_addr.first)).c_str(),
                    static_cast<s32>(async_data->ret));
            LOG_DEBUG(Service_SOC, "called, pid={}, fd={}, ret={}", async_data->pid,
                      async_data->socket_handle, static_cast<s32>(async_data->ret));

            IPC::RequestBuilder rb(ctx, 0x06, 2, 0);
            rb.Push(ResultSuccess);
            rb.Push(async_data->ret);
        });
}

void SOC_U::InitializeSockets(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    [[maybe_unused]] const auto memory_block_size = rp.Pop<u32>();
    const u32 pid = rp.PopPID();
    [[maybe_unused]] auto shared_memory = rp.PopObject<Kernel::SharedMemory>();

    Result res = ResultSuccess;
    if (initialized_processes.find(pid) == initialized_processes.end()) {
        initialized_processes.insert(pid);
    } else {
        res = ResultAlreadyInitialized;
    }

    LOG_DEBUG(Service_SOC, "called, pid={}, res={:#08X}", pid, res.raw);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(res);
}

void SOC_U::ShutdownSockets(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    // This service call does not provide a PID like the others,
    // instead SOC seems to fetch the PID from the current session.
    const u32 pid = ctx.ClientThread()->owner_process.lock()->process_id;

    if (initialized_processes.find(pid) == initialized_processes.end()) {
        LOG_DEBUG(Service_SOC, "Process not initialized: pid={}", pid);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ResultNotInitialized);
        return;
    }

    CloseAndDeleteAllSockets(pid);
    initialized_processes.erase(pid);

    LOG_DEBUG(Service_SOC, "called, pid={}", pid);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void SOC_U::GetSockOpt(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 socket_handle = rp.Pop<u32>();
    const u32 level = rp.Pop<u32>();
    const s32 optname = rp.Pop<s32>();
    u32 optlen = rp.Pop<u32>();
    const u32 pid = rp.PopPID();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    s32 err = 0;

    std::vector<u8> optval(optlen);

    if (optname < 0) {
#ifdef _WIN32
        err = WSAEINVAL;
#else
        err = EINVAL;
#endif
    } else {
        const auto level_opt = TranslateSockOpt(level, optname);
        std::vector<u8> platform_data(
            TranslateSockOptSizeToPlatform(level_opt.first, level_opt.second));
        socklen_t platform_data_size = static_cast<socklen_t>(platform_data.size());
        err = ::getsockopt(holder.socket_fd, level_opt.first, level_opt.second,
                           reinterpret_cast<char*>(platform_data.data()), &platform_data_size);
        if (err == SOCKET_ERROR_VALUE) {
            err = TranslateError(GET_ERRNO);
        } else {
            platform_data.resize(static_cast<std::size_t>(platform_data_size));
            TranslateSockOptDataFromPlatform(optval, platform_data, level_opt.first,
                                             level_opt.second);
        }
    }

    LOG_DEBUG(Service_SOC, "called, pid={}, fd={}, ret={}", pid, socket_handle,
              static_cast<s32>(err));

    IPC::RequestBuilder rb = rp.MakeBuilder(3, 2);
    rb.Push(ResultSuccess);
    rb.Push(err);
    rb.Push(static_cast<u32>(optval.size()));
    rb.PushStaticBuffer(std::move(optval), 0);
}

void SOC_U::SetSockOpt(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const auto socket_handle = rp.Pop<u32>();
    const auto level = rp.Pop<u32>();
    const auto optname = rp.Pop<s32>();
    const auto optlen = rp.Pop<u32>();
    const u32 pid = rp.PopPID();
    auto optval = rp.PopStaticBuffer();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    optval.resize(optlen);

    s32 err = 0;

    if (optname < 0) {
#ifdef _WIN32
        err = WSAEINVAL;
#else
        err = EINVAL;
#endif
    } else {
        std::vector<u8> platform_data;
        const auto levelopt = TranslateSockOpt(level, optname);
        TranslateSockOptDataToPlatform(platform_data, optval, levelopt.first, levelopt.second);
        err = static_cast<u32>(::setsockopt(holder.socket_fd, levelopt.first, levelopt.second,
                                            reinterpret_cast<char*>(platform_data.data()),
                                            static_cast<socklen_t>(platform_data.size())));
        if (err == SOCKET_ERROR_VALUE) {
            err = TranslateError(GET_ERRNO);
        }
    }

    HEN_SOC("setsockopt: socket %u, level %x, option %x, %u bytes -> %d", socket_handle, level,
            static_cast<u32>(optname), optlen, static_cast<s32>(err));
    LOG_DEBUG(Service_SOC, "called, pid={}, fd={}, ret={}", pid, socket_handle,
              static_cast<s32>(err));

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(err);
}

void SOC_U::GetNetworkOpt(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    u32 level = rp.Pop<u32>();
    u32 opt_name = rp.Pop<u32>();
    u32 opt_len = rp.Pop<u32>();
    std::vector<u8> opt_data(opt_len);

    u32 err = SOC_ERR_INAVLID_ENUM_VALUE;

    /// Only available level is SOC_SOL_CONFIG, any other value returns an error
    if (level == SOC_SOL_CONFIG) {
        switch (static_cast<NetworkOpt>(opt_name)) {
        case NetworkOpt::NETOPT_MAC_ADDRESS: {
            if (opt_len >= 6) {
                std::array<u8, 6> fake_mac = {0};
                std::memcpy(opt_data.data(), fake_mac.data(), fake_mac.size());
            }
            LOG_WARNING(Service_SOC, "(STUBBED) called, level={} opt_name={}", level, opt_name);
            err = 0;
        } break;
        case NetworkOpt::NETOPT_IP_INFO: {
            if (opt_len >= sizeof(InterfaceInfo)) {
                InterfaceInfo& out_info = *reinterpret_cast<InterfaceInfo*>(opt_data.data());
                auto info = GetDefaultInterfaceInfo();
                if (info.has_value()) {
                    out_info = info.value();
                }
            }
            // Extra data not used normally, takes 0xC bytes more
            if (opt_len >= sizeof(InterfaceInfo) + 0xC) {
                LOG_ERROR(Service_SOC, "(STUBBED) called, level={} opt_name={} opt_len >= 24",
                          level, opt_name);
            }
            err = 0;
        } break;
        default:
            LOG_ERROR(Service_SOC, "(STUBBED) called, level={} opt_name={}", level, opt_name);
            break;
        }
    } else {
        LOG_ERROR(Service_SOC, "Unknown level={}", level);
    }

    if (err != 0) {
        opt_data.resize(0);
        opt_len = 0;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(3, 2);
    rb.Push(ResultSuccess);
    rb.Push(err);
    rb.Push(static_cast<u32>(opt_len));
    rb.PushStaticBuffer(std::move(opt_data), 0);
}

void SOC_U::GetAddrInfoImpl(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    u32 node_length = rp.Pop<u32>();
    u32 service_length = rp.Pop<u32>();
    u32 hints_size = rp.Pop<u32>();
    u32 out_size = rp.Pop<u32>();
    auto node = rp.PopStaticBuffer();
    auto service = rp.PopStaticBuffer();
    auto hints_buff = rp.PopStaticBuffer();

    const char* node_data = node_length > 0 ? reinterpret_cast<const char*>(node.data()) : nullptr;
    const char* service_data =
        service_length > 0 ? reinterpret_cast<const char*>(service.data()) : nullptr;

    // (Kaizhar) the frontend's answer, if it has one (hen_guest_lookup above): the name becomes
    // that address, which the platform's resolver takes as it is.
    char own_node[INET_ADDRSTRLEN] = {};
    unsigned own_ipv4 = 0;
    if (hen_guest_lookup != nullptr && node_data != nullptr &&
        std::find(node.begin(), node.end(), u8{0}) != node.end() &&
        hen_guest_lookup(node_data, &own_ipv4) == 1) {
        in_addr own_address{};
        own_address.s_addr = own_ipv4;
        if (inet_ntop(AF_INET, &own_address, own_node, sizeof(own_node)) != nullptr) {
            node_data = own_node;
        }
    }

    // (hen_soc_rules, above)
    std::string hen_node;
    if (node_data != nullptr && node_data != own_node &&
        std::find(node.begin(), node.end(), u8{0}) != node.end()) {
        hen_node = HenLookupName(node_data);
        HEN_SOC("getaddrinfo: %s%s", node_data,
                hen_node != node_data ? fmt::format(" (looked up as {})", hen_node).c_str() : "");
        node_data = hen_node.c_str();
    }

    s32 ret = -1;
    addrinfo* out = nullptr;
    if (hints_size > 0) {
        CTRAddrInfo ctr_hints;
        std::memcpy(&ctr_hints, hints_buff.data(), hints_size);
        addrinfo hints = CTRAddrInfo::ToPlatform(ctr_hints);
        // Mimic what soc does in real hardware
        if (hints.ai_socktype != 0 && hints.ai_protocol == 0) {
            hints.ai_protocol = hints.ai_socktype == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
        }
        if (hints.ai_protocol != 0 && hints.ai_socktype == 0) {
            hints.ai_socktype = hints.ai_protocol == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM;
        }
        ret = getaddrinfo(node_data, service_data, &hints, &out);
    } else {
        ret = getaddrinfo(node_data, service_data, nullptr, &out);
    }

    std::vector<u8> out_buff(out_size);
    u32 count = 0;

    if (ret != 0) {
#ifdef _WIN32
        ret = TranslateGaiError(ret);
#else
        ret = ret == EAI_SYSTEM ? TranslateError(GET_ERRNO) : TranslateGaiError(ret);
#endif
        out_buff.resize(0);
    } else {
        std::size_t pos = 0;
        addrinfo* cur = out;
        while (cur != nullptr) {
            if (sizeof(CTRAddrInfo) <= out_size - pos) {
                // According to 3dbrew, this function fills whatever it can and does not error even
                // if the buffer is not big enough. However the count returned is always correct.
                CTRAddrInfo ctr_addr = CTRAddrInfo::FromPlatform(*cur);
                std::memcpy(out_buff.data() + pos, &ctr_addr, sizeof(ctr_addr));
                pos += sizeof(ctr_addr);
            }
            cur = cur->ai_next;
            count++;
        }
        if (out != nullptr)
            freeaddrinfo(out);
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(3, 2);
    rb.Push(ResultSuccess);
    rb.Push(ret);
    rb.Push(count);
    rb.PushStaticBuffer(std::move(out_buff), 0);
}

void SOC_U::GetNameInfoImpl(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    u32 socklen = rp.Pop<u32>();
    u32 hostlen = rp.Pop<u32>();
    u32 servlen = rp.Pop<u32>();
    s32 flags = NameInfoFlagsToPlatform(rp.Pop<s32>());
    auto sa_buff = rp.PopStaticBuffer();

    CTRSockAddr ctr_sa;
    std::memcpy(&ctr_sa, sa_buff.data(), socklen);
    auto [sa, sa_len] = CTRSockAddr::ToPlatform(ctr_sa);

    std::vector<u8> host(hostlen);
    std::vector<u8> serv(servlen);
    char* host_data = hostlen > 0 ? reinterpret_cast<char*>(host.data()) : nullptr;
    char* serv_data = servlen > 0 ? reinterpret_cast<char*>(serv.data()) : nullptr;

    s32 ret = getnameinfo(reinterpret_cast<sockaddr*>(&sa), sa_len, host_data, hostlen, serv_data,
                          servlen, flags);
    if (ret != 0) {
#ifdef _WIN32
        ret = TranslateGaiError(ret);
#else
        ret = ret == EAI_SYSTEM ? TranslateError(GET_ERRNO) : TranslateGaiError(ret);
#endif
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 4);
    rb.Push(ResultSuccess);
    rb.Push(ret);
    rb.PushStaticBuffer(std::move(host), 0);
    rb.PushStaticBuffer(std::move(serv), 1);
}

void SOC_U::SendToMultiple(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 socket_handle = rp.Pop<u32>();
    u32 len = rp.Pop<u32>();
    u32 flags = SendRecvFlagsToPlatform(rp.Pop<u32>());
    u32 addr_len = rp.Pop<u32>();
    u32 total_addr_len = rp.Pop<u32>();
    const u32 pid = rp.PopPID();
    auto input_buff = rp.PopStaticBuffer();
    auto dest_addr_buff = rp.PopStaticBuffer();

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    u32 count = total_addr_len / addr_len;
    s32 ret = 0;
    u32 i = 0;
    do {
        ret = SendToImpl(holder, len, flags, addr_len, input_buff,
                         dest_addr_buff.data() + (i * addr_len));
        i++;
    } while (i < count && ret >= 0);

    LOG_DEBUG(Service_SOC, "called, pid={}, fd_count={}, ret={}", pid, count,
              static_cast<s32>(ret));

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(ret);
}

void SOC_U::CloseSockets(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 pid = rp.PopPID();

    if (initialized_processes.find(pid) == initialized_processes.end()) {
        LOG_DEBUG(Service_SOC, "Process not initialized: pid={}", pid);
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ResultNotInitialized);
        return;
    }

    CloseAndDeleteAllSockets(pid);

    LOG_DEBUG(Service_SOC, "called, pid={}", pid);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

void SOC_U::AddGlobalSocket(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const auto socket_handle = rp.Pop<u32>();

    // This service call does not provide a PID like the others,
    // instead SOC seems to fetch the PID from the current session.
    const u32 pid = ctx.ClientThread()->owner_process.lock()->process_id;

    auto socket_holder_optional = GetSocketHolder(socket_handle, pid, rp);
    if (!socket_holder_optional) {
        return;
    }
    SocketHolder& holder = socket_holder_optional->get();

    holder.isGlobal = true;

    LOG_DEBUG(Service_SOC, "called, pid={}, fd={}", pid, socket_handle);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(ResultSuccess);
}

SOC_U::SOC_U() : ServiceFramework("soc:U", 18) {
    static const FunctionInfo functions[] = {
        // clang-format off
        {0x0001, &SOC_U::InitializeSockets, "InitializeSockets"},
        {0x0002, &SOC_U::Socket, "Socket"},
        {0x0003, &SOC_U::Listen, "Listen"},
        {0x0004, &SOC_U::Accept, "Accept"},
        {0x0005, &SOC_U::Bind, "Bind"},
        {0x0006, &SOC_U::Connect, "Connect"},
        {0x0007, &SOC_U::RecvFromOther, "recvfrom_other"},
        {0x0008, &SOC_U::RecvFrom, "RecvFrom"},
        {0x0009, &SOC_U::SendToOther, "SendToOther"},
        {0x000A, &SOC_U::SendToSingle, "SendToSingle"},
        {0x000B, &SOC_U::Close, "Close"},
        {0x000C, &SOC_U::Shutdown, "Shutdown"},
        {0x000D, &SOC_U::GetHostByName, "GetHostByName"},
        {0x000E, &SOC_U::GetHostByAddr, "GetHostByAddr"},
        {0x000F, &SOC_U::GetAddrInfoImpl, "GetAddrInfo"},
        {0x0010, &SOC_U::GetNameInfoImpl, "GetNameInfo"},
        {0x0011, &SOC_U::GetSockOpt, "GetSockOpt"},
        {0x0012, &SOC_U::SetSockOpt, "SetSockOpt"},
        {0x0013, &SOC_U::Fcntl, "Fcntl"},
        {0x0014, &SOC_U::Poll, "Poll"},
        {0x0015, &SOC_U::SockAtMark, "SockAtMark"},
        {0x0016, &SOC_U::GetHostId, "GetHostId"},
        {0x0017, &SOC_U::GetSockName, "GetSockName"},
        {0x0018, &SOC_U::GetPeerName, "GetPeerName"},
        {0x0019, &SOC_U::ShutdownSockets, "ShutdownSockets"},
        {0x001A, &SOC_U::GetNetworkOpt, "GetNetworkOpt"},
        {0x001B, nullptr, "ICMPSocket"},
        {0x001C, nullptr, "ICMPPing"},
        {0x001D, nullptr, "ICMPCancel"},
        {0x001E, nullptr, "ICMPClose"},
        {0x001F, nullptr, "GetResolverInfo"},
        {0x0020, &SOC_U::SendToMultiple, "SendToMultiple"},
        {0x0021, &SOC_U::CloseSockets, "CloseSockets"},
        {0x0023, &SOC_U::AddGlobalSocket, "AddGlobalSocket"},
        // clang-format on
    };

    RegisterHandlers(functions);

    Network::SocketManager::EnableSockets();
}

SOC_U::~SOC_U() {
    CloseAndDeleteAllSockets();
    Network::SocketManager::DisableSockets();
}

std::optional<SOC_U::InterfaceInfo> SOC_U::GetDefaultInterfaceInfo() {
    if (this->interface_info_cached) {
        return InterfaceInfo(this->interface_info);
    }

#ifdef __SWITCH__
    // The TCP connect below blocks until it times out when the console has no route to the
    // internet (over a minute, with the emulator stopped). Connecting a UDP socket sends
    // nothing and returns at once with the local address chosen. There is no interface list
    // here, so the netmask is assumed to be /24.
    {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            return std::nullopt;
        }
        struct sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(53);
        to.sin_addr.s_addr = inet_addr("8.8.8.8");
        struct sockaddr_in local = {};
        socklen_t length = sizeof(local);
        const bool ok = ::connect(fd, reinterpret_cast<struct sockaddr*>(&to), sizeof(to)) == 0 &&
                        ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&local), &length) ==
                            0 &&
                        local.sin_addr.s_addr != 0;
        closesocket(fd);
        if (!ok) {
            return std::nullopt;
        }
        InterfaceInfo info;
        info.address = local.sin_addr.s_addr;
        info.netmask = inet_addr("255.255.255.0");
        info.broadcast = info.address | ~info.netmask;
        this->interface_info = info;
        this->interface_info_cached = true;
        return info;
    }
#endif
    InterfaceInfo ret;

    SocketHolder::SOCKET sock_fd = -1;
    bool interface_found = false;
    struct sockaddr_in s_in = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr = {}};
    s_in.sin_addr.s_addr = inet_addr("8.8.8.8");
    socklen_t s_info_len = sizeof(struct sockaddr_in);
    sockaddr_in s_info;

    if ((sock_fd = ::socket(AF_INET, SOCK_STREAM, 0)) == static_cast<SocketHolder::SOCKET>(-1)) {
        return std::nullopt;
    }

    if (::connect(sock_fd, (struct sockaddr*)(&s_in), sizeof(struct sockaddr_in)) != 0) {
        closesocket(sock_fd);
        return std::nullopt;
    }

    if (::getsockname(sock_fd, (struct sockaddr*)&s_info, &s_info_len) != 0 ||
        s_info_len != sizeof(struct sockaddr_in)) {
        closesocket(sock_fd);
        return std::nullopt;
    }
    closesocket(sock_fd);

#ifdef _WIN32
    sock_fd = WSASocket(AF_INET, SOCK_DGRAM, 0, 0, 0, 0);
    if (sock_fd == static_cast<SocketHolder::SOCKET>(SOCKET_ERROR)) {
        return std::nullopt;
    }

    const int max_interfaces = 100;
    std::vector<INTERFACE_INFO> interface_list_vec(max_interfaces);
    INTERFACE_INFO* interface_list = reinterpret_cast<INTERFACE_INFO*>(interface_list_vec.data());
    unsigned long bytes_used;
    if (WSAIoctl(sock_fd, SIO_GET_INTERFACE_LIST, 0, 0, interface_list,
                 max_interfaces * sizeof(INTERFACE_INFO), &bytes_used, 0, 0) == SOCKET_ERROR) {
        closesocket(sock_fd);
        return std::nullopt;
    }
    closesocket(sock_fd);

    int num_interfaces = bytes_used / sizeof(INTERFACE_INFO);
    for (int i = 0; i < num_interfaces; i++) {
        if (((sockaddr*)&(interface_list[i].iiAddress))->sa_family == AF_INET &&
            std::memcmp(&((sockaddr_in*)&(interface_list[i].iiAddress))->sin_addr.s_addr,
                        &s_info.sin_addr.s_addr, sizeof(s_info.sin_addr.s_addr)) == 0) {
            ret.address = ((sockaddr_in*)&(interface_list[i].iiAddress))->sin_addr.s_addr;
            ret.netmask = ((sockaddr_in*)&(interface_list[i].iiNetmask))->sin_addr.s_addr;
            ret.broadcast =
                ((sockaddr_in*)&(interface_list[i].iiBroadcastAddress))->sin_addr.s_addr;
            interface_found = true;
            {
                char address[16] = {0}, netmask[16] = {0}, broadcast[16] = {0};
                std::strncpy(address,
                             inet_ntoa(((sockaddr_in*)&(interface_list[i].iiAddress))->sin_addr),
                             sizeof(address) - 1);
                std::strncpy(netmask,
                             inet_ntoa(((sockaddr_in*)&(interface_list[i].iiNetmask))->sin_addr),
                             sizeof(netmask) - 1);
                std::strncpy(
                    broadcast,
                    inet_ntoa(((sockaddr_in*)&(interface_list[i].iiBroadcastAddress))->sin_addr),
                    sizeof(broadcast) - 1);

                LOG_DEBUG(Service_SOC, "Found interface: (addr: {}, netmask: {}, broadcast: {})",
                          address, netmask, broadcast);
            }
            break;
        }
    }
#elif !(defined(ANDROID) && defined(HAVE_LIBRETRO))
    // Libretro Android builds target API 21, but getifaddrs() requires API 24+.
    // Standalone Android (minSdk 29) and other platforms have getifaddrs().
    struct ifaddrs* ifaddr;
    struct ifaddrs* ifa;
    if (getifaddrs(&ifaddr) == -1) {
        return std::nullopt;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* in_address = (struct sockaddr_in*)ifa->ifa_addr;
            struct sockaddr_in* in_netmask = (struct sockaddr_in*)ifa->ifa_netmask;
            struct sockaddr_in* in_broadcast = (struct sockaddr_in*)ifa->ifa_broadaddr;
            if (in_address->sin_addr.s_addr == s_info.sin_addr.s_addr) {
                ret.address = in_address->sin_addr.s_addr;
                ret.netmask = in_netmask->sin_addr.s_addr;
                ret.broadcast = in_broadcast->sin_addr.s_addr;
                interface_found = true;
                {
                    char address[16] = {0}, netmask[16] = {0}, broadcast[16] = {0};
                    std::strncpy(address, inet_ntoa(in_address->sin_addr), sizeof(address) - 1);
                    std::strncpy(netmask, inet_ntoa(in_netmask->sin_addr), sizeof(netmask) - 1);
                    std::strncpy(broadcast, inet_ntoa(in_broadcast->sin_addr),
                                 sizeof(broadcast) - 1);

                    LOG_DEBUG(Service_SOC,
                              "Found interface: (addr: {}, netmask: {}, broadcast: {})", address,
                              netmask, broadcast);
                }
            }
        }
    }
    freeifaddrs(ifaddr);
#endif // _WIN32
    if (interface_found) {
        this->interface_info = ret;
        this->interface_info_cached = true;
        return ret;
    } else {
        LOG_DEBUG(Service_SOC, "Interface not found");
        return std::nullopt;
    }
}

std::shared_ptr<SOC_U> GetService(Core::System& system) {
    return system.ServiceManager().GetService<SOC_U>("soc:U");
}

void InstallInterfaces(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    std::make_shared<SOC_U>()->InstallAsService(service_manager);
}

} // namespace Service::SOC
