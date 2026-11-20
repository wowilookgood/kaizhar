// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "citra_switch/tool_scan.h"
#include "common/common_types.h"
#ifdef __SWITCH__
#include "citra_switch/switch_platform.h"
#endif

namespace ToolScan {

namespace {

constexpr int Group = 64;    ///< connections tried at a time, at most
constexpr int SmallGroup = 8;
constexpr int GroupMs = 350; ///< how long a group is given
constexpr int QuickMs = 350; ///< ...and the remembered address by itself
constexpr int LookMs = 25;   ///< how often a group's sockets are looked at
constexpr int Workers = 16;  ///< threads of the blocking way

long long NowMsLater(int ms);

struct Try {
    int fd = -1;
    in_addr address{};
};

/// A connection the search made and still holds: a setup tool serves one client and, by its
/// source, never listens again once that client just goes away (it has to be told to stop, or
/// restarted on the 3DS). So the connection of the address that is chosen is the one the setup
/// then uses (Take), and the others are ended the tool's way (Leave).
struct Held {
    std::string address;
    int fd = -1;
};
std::vector<Held> held;
int handed_over = 0; ///< for the log

/// Ends a connection as the setup's own client does: the protocol's "$STOP" request, a moment
/// for the answer, then closed. The tool then goes back to listening after half a second.
void Leave(int fd) {
    unsigned char stop[0x28] = {};
    std::memcpy(stop + 4, "$STOP", 5);
    const long long until = NowMsLater(300);
    std::size_t sent = 0;
    while (sent < sizeof(stop) && NowMsLater(0) < until) {
        const ssize_t now = send(fd, stop + sent, sizeof(stop) - sent, 0);
        if (now > 0) {
            sent += static_cast<std::size_t>(now);
        } else if (now < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
            break;
        } else {
            usleep(5000);
        }
    }
    while (sent == sizeof(stop) && NowMsLater(0) < until) {
        unsigned char answer[256];
        const ssize_t got = recv(fd, answer, sizeof(answer), 0);
        if (got >= 0 || (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)) {
            break; // (answered, or the tool has closed its end)
        }
        usleep(5000);
    }
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

/// What a search did, for the log: one run on the console has to tell which of the
/// platform's socket calls doesn't behave as on a computer.
struct Detail {
    int groups = 0, smaller = 0;
    int opened = 0, no_socket = 0, no_socket_errno = 0;
    int nonblocking = 0, nonblocking_refused = 0;
    std::map<int, int> at_once;      ///< connect()'s result at once: 0, or errno
    int connected = 0, silent = 0;
    std::map<int, int> ended;        ///< SO_ERROR of those that failed
    int select_calls = 0, select_failed = 0, select_errno = 0, select_writable = 0, select_asked = 0;
    int too_high = 0;                ///< sockets with a number select can't take
    long long open_ms = 0, wait_ms = 0;
    bool blocking_way = false;
    int blocking_timeouts_set = 0;
};

long long NowMsLater(int ms) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
               .count() +
           ms;
}

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string TextOf(in_addr address) {
    char text[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &address, text, sizeof(text));
    return text;
}

std::string Counts(const std::map<int, int>& counts) {
    std::string text;
    for (const auto& [value, times] : counts) {
        text += (text.empty() ? "" : ", ") + std::to_string(value) + " x" + std::to_string(times);
    }
    return text.empty() ? "none" : text;
}

/// Errors of connect() that say the system can't take more connections now, not that the
/// address doesn't answer.
bool OutOfRoom(int error) {
    return error == ENOBUFS || error == EMFILE || error == ENFILE || error == ENOMEM || error == EADDRNOTAVAIL;
}

/// Whether the socket's connection stands: it then has a peer. (Asked directly instead of
/// through select and the socket's error alone: neither is known to behave on every system
/// this runs on.)
bool Connected(int fd) {
    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    return getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &length) == 0;
}

/// Starts connections to the addresses from `from` on, `size` at most, without blocking,
/// looks at them every LookMs for wait_ms, and adds those that connected to found. Returns how
/// many addresses it dealt with: fewer than size when the system had no room for more (they
/// are for the next group); -1 when sockets can't be made non-blocking here.
int TryGroup(const std::vector<in_addr>& addresses, std::size_t from, int size, int wait_ms,
             std::vector<std::string>& found, Detail& detail) {
    const long long opening = NowMs();
    std::vector<Try> tries;
    detail.groups++;
    for (std::size_t i = from; i < addresses.size() && static_cast<int>(tries.size()) < size; i++) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            detail.no_socket++;
            detail.no_socket_errno = errno;
            break; // (no more sockets to be had now: the rest waits for the next group)
        }
        detail.opened++;
        const int flags = fcntl(fd, F_GETFL, 0);
        const bool set = flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
        const int now = fcntl(fd, F_GETFL, 0);
        if (!set || now == -1 || (now & O_NONBLOCK) == 0) {
            detail.nonblocking_refused++;
            close(fd);
            if (tries.empty()) {
                return -1;
            }
            break;
        }
        detail.nonblocking++;
        sockaddr_in where{};
        where.sin_family = AF_INET;
        where.sin_port = htons(Port);
        where.sin_addr = addresses[i];
        errno = 0;
        const int result = connect(fd, reinterpret_cast<const sockaddr*>(&where), sizeof(where));
        const int error = result == 0 ? 0 : errno;
        detail.at_once[error]++;
        if (result != 0 && OutOfRoom(error)) {
            close(fd);
            break;
        }
        // Whatever else connect said, the socket is kept and looked at: which error means "under
        // way" differs between systems, and one that failed says so below.
        Try one;
        one.fd = fd;
        one.address = addresses[i];
        tries.push_back(one);
    }
    const long long waiting = NowMs();
    detail.open_ms += waiting - opening;
    const long long until = waiting + wait_ms;
    for (;;) {
        int pending = 0;
        for (Try& one : tries) {
            if (one.fd < 0) {
                continue;
            }
            int error = 0;
            socklen_t length = sizeof(error);
            const bool have_error = getsockopt(one.fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error != 0;
            if (Connected(one.fd)) {
                // (kept open: see Held)
                detail.connected++;
                found.push_back(TextOf(one.address));
                held.push_back({TextOf(one.address), one.fd});
                one.fd = -1;
                continue;
            } else if (have_error && error != EINPROGRESS && error != EALREADY && error != EINTR &&
                       error != EWOULDBLOCK && error != EAGAIN) {
                detail.ended[error]++;
            } else {
                pending++;
                continue;
            }
            close(one.fd);
            one.fd = -1;
        }
        if (pending == 0 || NowMs() >= until) {
            break;
        }
        usleep(LookMs * 1000);
    }
    // For the log only: what select makes of the sockets still open (those that never answered).
    {
        fd_set writable;
        FD_ZERO(&writable);
        int highest = -1, asked = 0;
        for (const Try& one : tries) {
            if (one.fd >= 0 && one.fd < FD_SETSIZE) {
                FD_SET(one.fd, &writable);
                highest = std::max(highest, one.fd);
                asked++;
            } else if (one.fd >= 0) {
                detail.too_high++;
            }
        }
        if (highest >= 0) {
            timeval none{};
            errno = 0;
            const int result = select(highest + 1, nullptr, &writable, nullptr, &none);
            detail.select_calls++;
            detail.select_asked += asked;
            if (result < 0) {
                detail.select_failed++;
                detail.select_errno = errno;
            } else {
                detail.select_writable += result;
            }
        }
    }
    for (const Try& one : tries) {
        if (one.fd >= 0) {
            detail.silent++;
            close(one.fd);
        }
    }
    detail.wait_ms += NowMs() - waiting;
    return static_cast<int>(tries.size());
}

/// The other way, for a system whose sockets can't be made non-blocking: connections that
/// block, each given GroupMs by the socket's own time limits, on a few threads that are all
/// joined before this returns. Nothing is tried unless the limits can be set: a connection to
/// an address nobody has would otherwise hold its thread for a minute and more.
void TryBlocking(const std::vector<in_addr>& addresses, std::vector<std::string>& found, Detail& detail) {
    detail.blocking_way = true;
    std::atomic<std::size_t> next{0};
    std::atomic<int> limits_set{0};
    std::atomic<bool> give_up{false};
    std::mutex mutex;
    const auto work = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= addresses.size() || give_up) {
                return;
            }
            const int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                return;
            }
            timeval limit{};
            limit.tv_usec = GroupMs * 1000;
            timeval read_back{};
            socklen_t length = sizeof(read_back);
            if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit)) != 0 ||
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)) != 0 ||
                getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &read_back, &length) != 0 ||
                (read_back.tv_sec == 0 && read_back.tv_usec == 0)) {
                give_up = true;
                close(fd);
                return;
            }
            limits_set++;
            sockaddr_in where{};
            where.sin_family = AF_INET;
            where.sin_port = htons(Port);
            where.sin_addr = addresses[i];
            const long long before = NowMs();
            const bool connected = connect(fd, reinterpret_cast<const sockaddr*>(&where), sizeof(where)) == 0;
            if (connected) {
                std::scoped_lock lock{mutex};
                found.push_back(TextOf(addresses[i]));
                held.push_back({TextOf(addresses[i]), fd});
            } else {
                close(fd);
            }
            if (connected) {
            } else if (NowMs() - before > GroupMs * 4) {
                give_up = true; // (the limit isn't kept: no more addresses are started)
                return;
            }
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < Workers; i++) {
        threads.emplace_back(work);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    detail.blocking_timeouts_set = limits_set;
}

/// All of the addresses, in groups that get smaller when the system has no room.
/// False: sockets can't be made non-blocking.
bool Sweep(const std::vector<in_addr>& addresses, int wait_ms, std::vector<std::string>& found,
           Detail& detail, std::size_t& tried) {
    int size = Group;
    std::size_t at = 0;
    while (at < addresses.size()) {
        const int done = TryGroup(addresses, at, size, wait_ms, found, detail);
        if (done < 0) {
            return false;
        }
        if (done < size && at + done < addresses.size()) {
            // (no room for a whole group: smaller ones; an address that can't even be tried by
            // itself in a small one is passed over)
            detail.smaller++;
            if (done == 0 && size == SmallGroup) {
                at++;
                tried++;
            }
            size = std::max(SmallGroup, size / 2);
        }
        at += done;
        tried += done;
    }
    return true;
}

bool OwnAddress(in_addr& own) {
    char text[64] = "";
#ifdef __SWITCH__
    if (!sw_own_address(text, sizeof(text))) {
        return false;
    }
#else
    // Tests on a computer: the network to look in is given, never the computer's own.
    const char* given = std::getenv("KAIZHAR_TEST_SCAN_OWN");
    if (given == nullptr) {
        return false;
    }
    std::snprintf(text, sizeof(text), "%s", given);
#endif
    return inet_pton(AF_INET, text, &own) == 1;
}

} // namespace

std::vector<std::string> Find(const std::string& remembered, std::string& note) {
    note.clear();
    // What an earlier search still holds is let go first; a tool told to stop takes half a
    // second and a little to listen again.
    if (!held.empty()) {
        Drop();
        usleep(900 * 1000);
    }
    std::vector<std::string> found;
    Detail detail;
    const long long start = NowMs();
    std::size_t tried = 0;
    bool nonblocking = true;
    // The address used last time, by itself: the usual case is over in a moment.
    in_addr last{};
    const std::string host = remembered.substr(0, remembered.find(':'));
    const bool have_last = !host.empty() && inet_pton(AF_INET, host.c_str(), &last) == 1;
    if (have_last) {
        nonblocking = Sweep({last}, QuickMs, found, detail, tried);
    }
    const long long quick_ms = NowMs() - start;
    std::string network = "none";
    {
        in_addr own{};
        if (OwnAddress(own)) {
            network = TextOf(own);
        }
    }
    if (found.empty()) {
        in_addr own{};
        if (!OwnAddress(own)) {
            note = "this console has no network address";
        } else {
            network = TextOf(own);
            const u32 base = ntohl(own.s_addr) & 0xFFFFFF00u;
            std::vector<in_addr> addresses;
            for (u32 last_part = 1; last_part <= 254; last_part++) {
                in_addr one{};
                one.s_addr = htonl(base | last_part);
                if (one.s_addr != own.s_addr && !(have_last && one.s_addr == last.s_addr)) {
                    addresses.push_back(one);
                }
            }
            if (nonblocking) {
                nonblocking = Sweep(addresses, GroupMs, found, detail, tried);
            }
            if (!nonblocking) {
                TryBlocking(addresses, found, detail);
                tried = addresses.size();
                if (detail.blocking_timeouts_set == 0) {
                    note = "this console's network sockets can't be given a time limit";
                }
            } else if (detail.opened == 0) {
                note = "no network sockets could be opened";
            }
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    // More "consoles" than a home has: the system says yes to everything, and none is believed.
    const std::size_t answered = found.size();
    if (answered > 16) {
        for (const Held& one : held) {
            close(one.fd);
        }
        held.clear();
        found.clear();
        note = "this console's network calls can't tell which addresses answer";
    }
    std::string hits;
    for (const std::string& one : found) {
        hits += " " + one;
    }
    std::printf("hen: link: search: %zu address(es) tried on port %u in %lld ms: %zu answered%s%s%s\n", tried,
                static_cast<unsigned>(Port), NowMs() - start, answered, hits.c_str(),
                note.empty() ? "" : "; ", note.c_str());
    std::printf("hen: link: search: detail: own address %s; remembered \"%s\" (%s, %lld ms); %d group(s), %d made "
                "smaller; sockets opened %d, refused %d (errno %d), non-blocking %d, not taken %d; connect at once "
                "[0 is connected, else errno]: %s; ended: connected %d, failed by errno: %s, silent %d; select on "
                "the silent ones: %d call(s), %d of %d writable, %d failed (errno %d), %d numbered past its %d; "
                "opening %lld ms, waiting %lld ms%s; errno here: in progress %d, would block %d, refused %d, no "
                "route %d, timed out %d\n",
                network.c_str(), remembered.c_str(), have_last ? "tried first" : "nothing to try first", quick_ms,
                detail.groups, detail.smaller, detail.opened, detail.no_socket, detail.no_socket_errno,
                detail.nonblocking, detail.nonblocking_refused, Counts(detail.at_once).c_str(), detail.connected,
                Counts(detail.ended).c_str(), detail.silent, detail.select_calls, detail.select_writable,
                detail.select_asked, detail.select_failed, detail.select_errno, detail.too_high, FD_SETSIZE,
                detail.open_ms, detail.wait_ms,
                detail.blocking_way
                    ? (detail.blocking_timeouts_set ? "; the blocking way was used" : "; the blocking way had no time limits")
                    : "",
                EINPROGRESS, EWOULDBLOCK, ECONNREFUSED, EHOSTUNREACH, ETIMEDOUT);
    return found;
}

void Keep(const std::string& address) {
    std::vector<Held> kept;
    for (const Held& one : held) {
        if (one.address == address && kept.empty()) {
            kept.push_back(one);
        } else {
            Leave(one.fd);
        }
    }
    std::printf("hen: link: search: %zu connection(s) held; %s is kept for the setup, the others were told to stop\n",
                held.size(), kept.empty() ? "none" : address.c_str());
    held = std::move(kept);
}

void Drop() {
    if (!held.empty()) {
        std::printf("hen: link: search: %zu connection(s) not used: told to stop and closed\n", held.size());
    }
    for (const Held& one : held) {
        Leave(one.fd);
    }
    held.clear();
}

bool Holds(const std::string& address) {
    return std::any_of(held.begin(), held.end(), [&](const Held& one) { return one.address == address; });
}

bool Hold(const std::string& address, int total_ms) {
    if (Holds(address)) {
        return true;
    }
    in_addr where{};
    if (inet_pton(AF_INET, address.c_str(), &where) != 1) {
        return false;
    }
    const long long start = NowMs();
    int tries = 0;
    bool connected = false;
    for (;;) {
        Detail detail;
        std::vector<std::string> found;
        tries++;
        TryGroup({where}, 0, 1, QuickMs, found, detail);
        connected = !found.empty();
        if (connected || NowMs() - start >= total_ms) {
            break;
        }
        usleep(500 * 1000);
    }
    std::printf("hen: link: connecting to %s: %s after %d tr%s, %lld ms\n", address.c_str(),
                connected ? "connected" : "no answer", tries, tries == 1 ? "y" : "ies", NowMs() - start);
    return connected;
}

int Take(const char* address, unsigned port) {
    if (address == nullptr || port != Port) {
        return -1;
    }
    for (auto it = held.begin(); it != held.end(); ++it) {
        if (it->address == address) {
            const int fd = it->fd;
            held.erase(it);
            handed_over++;
            std::printf("hen: link: the search's connection to %s is the setup's now (socket %d)\n", address, fd);
            return fd;
        }
    }
    return -1;
}

} // namespace ToolScan
