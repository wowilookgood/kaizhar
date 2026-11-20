// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/kernel/hen_ipc_trace.h"

#if HEN_DEV

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <tuple>
#include <unordered_map>

namespace Kernel::HenIpc {

namespace {

/// One request in the list of recent ones. The session is a number into `names`.
struct Entry {
    u64 sent_us;
    u32 header;
    u16 process_id;
    u16 name;
};

constexpr std::size_t RING_SIZE = 65536; // (a title asks a few hundred times a second)

std::unordered_map<u32, Last> last_by_thread;

/// What is still to be printed about a thread's last request.
struct Open {
    u32 process_id = 0;
    bool lle = false;     ///< its answer gets a "hen: lle:" line
    bool mod = false;     ///< a system module's request: its answer gets a "hen: mod:" line
    u64 req_number = 0;   ///< its number among the "hen: req:" lines; 0: not one of them
    bool req_printed = false;
    std::string req_text; ///< the "hen: req:" line up to the answer
};
std::unordered_map<u32, Open> open_by_thread;
std::unordered_map<std::string, u32> req_kinds; ///< "session header" -> lines printed
u64 req_printed = 0, req_left_out = 0;
std::unordered_map<u32, Signal> signals;

/// The words after the first that the header says are there, four at most.
std::string Words(const u32* words, unsigned from) {
    const unsigned count = (words[0] & 0x3F) + ((words[0] >> 6) & 0x3F);
    std::string out;
    char one[16];
    for (unsigned i = from; i <= count && i <= 4; i++) {
        std::snprintf(one, sizeof(one), " %08x", words[i]);
        out += one;
    }
    return out;
}
std::unordered_map<std::string, u16> name_ids;
std::vector<std::string> names;
std::array<Entry, RING_SIZE> ring;
u64 ring_written = 0; ///< entries ever written; the next goes to ring_written % RING_SIZE

u16 NameId(const std::string& session) {
    const auto found = name_ids.find(session);
    if (found != name_ids.end()) {
        return found->second;
    }
    if (names.size() >= 0xFFFF) {
        return 0xFFFF; // (never seen: a console has a few hundred sessions)
    }
    const u16 id = static_cast<u16>(names.size());
    names.push_back(session);
    name_ids.emplace(session, id);
    return id;
}

} // namespace

static void Sent(u32 thread_id, u32 process_id, const std::string& session, u32 header, u64 now_us) {
    Last& last = last_by_thread[thread_id];
    if (last.session != session) {
        last.session = session;
    }
    last.header = header;
    last.sent_us = now_us;
    last.answered = false;

    ring[ring_written % RING_SIZE] = {now_us, header, static_cast<u16>(process_id), NameId(session)};
    ring_written++;
}

u32 signalling_thread = 0;

} // namespace Kernel::HenIpc
// (--trace-modules) The requests the system modules themselves make (the friends
// module asking the file system which title a process is, say) are not an application's and
// were never printed. With the option: "hen: mod: ..." lines, the first MOD_PER_KIND of each
// kind (process, session and header) with their answers.
extern "C" {
int hen_trace_modules = 0;
}
namespace Kernel::HenIpc {
namespace {
constexpr u32 MOD_PER_KIND = 12;
std::unordered_map<std::string, u32> mod_kinds;
} // namespace

void Request(u32 thread_id, u32 process_id, bool app, bool lle, const std::string& session,
             const u32* words, u64 now_us) {
    Sent(thread_id, process_id, session, words[0], now_us);
    Open& open = open_by_thread[thread_id];
    open = {};
    if (!app) {
        if (hen_trace_modules) {
            char kind[32];
            std::snprintf(kind, sizeof(kind), "%u %08x ", process_id, words[0]);
            u32& of_kind = mod_kinds[kind + session];
            if (of_kind < MOD_PER_KIND) {
                of_kind++;
                open.process_id = process_id;
                open.mod = true;
                std::printf("hen: mod: %.6f s process %u thread %u asks %s %08x:%s%s\n", now_us / 1e6,
                            process_id, thread_id, session.c_str(), words[0], Words(words, 1).c_str(),
                            of_kind == MOD_PER_KIND ? " (the last of this kind printed)" : "");
            }
        }
        return;
    }
    open.process_id = process_id;
    const std::string asked = Words(words, 1);
    if (lle) {
        open.lle = true;
        std::printf("hen: lle: %.6f s process %u thread %u asks %s %08x:%s\n", now_us / 1e6, process_id,
                    thread_id, session.c_str(), words[0], asked.c_str());
    }
    char header[16];
    std::snprintf(header, sizeof(header), " %08x", words[0]);
    u32& of_kind = req_kinds[session + header];
    if (req_printed >= REQ_LINES || of_kind >= REQ_PER_KIND) {
        req_left_out++;
        return;
    }
    of_kind++;
    open.req_number = ++req_printed;
    char start[96];
    std::snprintf(start, sizeof(start), "hen: req: %llu %.6f s thread %u ",
                  static_cast<unsigned long long>(open.req_number), now_us / 1e6, thread_id);
    open.req_text = start + session + header + ":" + asked;
}

void Pending(u32 thread_id) {
    const auto found = open_by_thread.find(thread_id);
    if (found == open_by_thread.end() || found->second.req_number == 0 || found->second.req_printed) {
        return;
    }
    found->second.req_printed = true;
    std::printf("%s -> answered later\n", found->second.req_text.c_str());
}

void Answer(u32 thread_id, const u32* words, u64 now_us) {
    const auto last = last_by_thread.find(thread_id);
    if (last == last_by_thread.end()) {
        return;
    }
    last->second.answered = true;
    const auto found = open_by_thread.find(thread_id);
    if (found == open_by_thread.end()) {
        return;
    }
    Open& open = found->second;
    if (open.mod) {
        std::printf("hen: mod: %.6f s process %u thread %u answer from %s to %08x: result %08x,%s\n",
                    now_us / 1e6, open.process_id, thread_id, last->second.session.c_str(),
                    last->second.header, words[1], Words(words, 2).c_str());
        open = {};
        return;
    }
    if (!open.lle && open.req_number == 0) {
        return;
    }
    const std::string more = Words(words, 2);
    if (open.lle) {
        std::printf("hen: lle: %.6f s process %u thread %u answer from %s to %08x: result %08x,%s (after %.3f ms)\n",
                    now_us / 1e6, open.process_id, thread_id, last->second.session.c_str(),
                    last->second.header, words[1], more.c_str(), (now_us - last->second.sent_us) / 1e3);
    }
    if (open.req_number != 0) {
        if (open.req_printed) {
            std::printf("hen: req: %llu answered at %.6f s: result %08x,%s\n",
                        static_cast<unsigned long long>(open.req_number), now_us / 1e6, words[1], more.c_str());
        } else {
            std::printf("%s -> result %08x,%s\n", open.req_text.c_str(), words[1], more.c_str());
        }
    }
    open = {};
}

void RequestLineStats(u64& printed, u64& left_out) {
    printed = req_printed;
    left_out = req_left_out;
}

void Signalled(u32 object_id, u32 thread_id, u64 now_us) {
    Signal& signal = signals[object_id];
    signal.thread_id = thread_id;
    signal.at_us = now_us;
    signal.count++;
}

const Signal* FindSignal(u32 object_id) {
    const auto found = signals.find(object_id);
    return found == signals.end() ? nullptr : &found->second;
}

const Last* Find(u32 thread_id) {
    const auto found = last_by_thread.find(thread_id);
    return found == last_by_thread.end() ? nullptr : &found->second;
}

std::vector<Count> Recent(u64 since_us, u64& total, u64& kept_from_us) {
    total = 0;
    kept_from_us = since_us;
    const u64 kept = std::min<u64>(ring_written, RING_SIZE);
    std::map<std::tuple<u16, u16, u32>, u32> counts;
    for (u64 i = ring_written - kept; i < ring_written; i++) {
        const Entry& entry = ring[i % RING_SIZE];
        if (entry.sent_us < since_us) {
            continue;
        }
        counts[{entry.process_id, entry.name, entry.header}]++;
        total++;
    }
    // Every entry kept is inside the time asked for and older ones were overwritten: the list
    // starts later than asked.
    if (kept == RING_SIZE && total == RING_SIZE) {
        kept_from_us = ring[ring_written % RING_SIZE].sent_us;
    }
    std::vector<Count> out;
    out.reserve(counts.size());
    for (const auto& [key, count] : counts) {
        const u16 name = std::get<1>(key);
        out.push_back({std::get<0>(key), name < names.size() ? names[name] : std::string("?"),
                       std::get<2>(key), count});
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Count& a, const Count& b) { return a.count > b.count; });
    return out;
}

void Reset() {
    last_by_thread.clear();
    name_ids.clear();
    names.clear();
    ring_written = 0;
    open_by_thread.clear();
    req_kinds.clear();
    req_printed = req_left_out = 0;
    signals.clear();
    signalling_thread = 0;
}

} // namespace Kernel::HenIpc

#endif // HEN_DEV
