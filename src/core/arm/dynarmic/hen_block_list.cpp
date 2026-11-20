// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <dynarmic/interface/A32/a32.h>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/thread_worker.h"
#include "core/arm/dynarmic/hen_block_list.h"

extern "C" unsigned long long hen_jit_now_ns(void); // hen/jit/hen_jit.c
extern "C" int hen_translation_worker;
extern "C" int hen_tier;
extern "C" void hen_resort_title_start(void); // hen/jit/hen_jit.c

namespace Core::HenBlockList {

namespace {

/// As dynarmic's AddressSpace::HenBlock; also the file's record.
struct Block {
    u64 descriptor;
    u32 hash;
    u16 words;
    u16 tag;
};
static_assert(sizeof(Block) == 16);

struct JitList {
    Dynarmic::A32::Jit* jit;
    u16 tag;
    std::vector<Block> blocks;
    std::vector<u8> state; ///< 0 to do, 1 done, 2 code not there (yet)
    std::size_t used_count = 0; ///< the first blocks are those the program ran (or nearly)
    std::size_t cursor = 0;
    std::size_t done = 0;
    u64 next_pass_ns = 0;
};

std::recursive_mutex mutex; // (the JIT reports blocks compiled from Work, too)
std::vector<Dynarmic::A32::Jit*> upgrade_jits; // (emulation thread only: see UpgradeLinks)
std::string path;
std::vector<Block> loaded;
std::unordered_set<u64> known;
std::vector<Block> fresh;
std::vector<JitList> jits;
std::size_t recorded_total = 0;
std::size_t compiled_total = 0;
std::size_t speculated_total = 0;
bool speculate = true;
u64 start_ns = 0, idle_ns = 0;

u64 Key(const Block& b) {
    u64 k = b.descriptor * 0x9E3779B97F4A7C15ull;
    k ^= (static_cast<u64>(b.hash) << 16 | b.tag) + (k >> 29);
    return k * 0xBF58476D1CE4E5B9ull;
}

/// The compile worker has the lists (dynarmic's AddressSpace::HenSetList) and the interpreter
/// runs what isn't compiled yet: nothing is compiled ahead on the emulation thread, neither at
/// the start (that held a cached launch at half speed for its first seconds, where a launch
/// with no list at all now starts near full speed) nor in its idle time.
bool WorkerHasLists() {
    return hen_translation_worker == 3 && hen_tier > 0;
}

// (HenSetList takes the JIT's compile lock, and the worker reports blocks, taking this file's
// mutex, while it holds that: so never call it with the mutex held.)
void TakeListsBack() {
    for (JitList& list : jits) {
        list.jit->HenSetList(nullptr, nullptr, 0);
    }
}
void GiveLists() {
    if (!WorkerHasLists()) {
        return;
    }
    for (JitList& list : jits) {
        if (!list.blocks.empty()) {
            list.jit->HenSetList(list.blocks.data(), list.state.data(), list.blocks.size());
        }
    }
}

void Assign(JitList& list) {
    // Blocks the program ran first, in the order it needed them; then those compiled on
    // speculation (the tag's top bit), which only idle time is spent on.
    list.blocks.clear();
    for (const Block& b : loaded) {
        if (b.tag == list.tag) {
            list.blocks.push_back(b);
        }
    }
    list.used_count = list.blocks.size();
    for (const Block& b : loaded) {
        if (b.tag == (list.tag | 0x8000)) {
            list.blocks.push_back(b);
        }
    }
    list.state.assign(list.blocks.size(), 0);
    list.cursor = 0;
    list.done = 0;
    list.next_pass_ns = 0;
}

/// Compiles from one JIT's list until the deadline. Returns false when there is nothing (more)
/// to do for it now.
bool Work(JitList& list, u64 deadline_ns, bool used_only = false) {
    if (list.done >= list.blocks.size() || (used_only && list.cursor >= list.used_count)) {
        return false;
    }
    const u64 now = hen_jit_now_ns();
    if (list.cursor >= list.blocks.size()) {
        // The rest is code that wasn't there (modules loaded later): look again now and then.
        if (now < list.next_pass_ns) {
            return false;
        }
        list.cursor = 0;
        list.next_pass_ns = now + 2'000'000'000ull;
    }
    std::size_t compiled = 0;
    const std::size_t looked =
        list.jit->HenPrecompile(list.blocks.data() + list.cursor, list.state.data() + list.cursor,
                                (used_only ? list.used_count : list.blocks.size()) - list.cursor,
                                deadline_ns, &compiled);
    list.done = static_cast<std::size_t>(std::count(list.state.begin(), list.state.end(), 1));
    compiled_total += compiled;
    if (looked == 0 && hen_jit_now_ns() < deadline_ns) {
        // Stopped for another reason than time (the code cache is nearly full): leave it be.
        list.next_pass_ns = now + 5'000'000'000ull;
        list.cursor = list.blocks.size();
        return false;
    }
    list.cursor += looked;
    return list.cursor < list.blocks.size() && hen_jit_now_ns() < deadline_ns;
}

} // namespace

void SetSpeculation(bool enabled) {
    speculate = enabled;
}

void Open(u64 title_id, unsigned start_budget_ms, const char* folder) {
    hen_resort_title_start(); // (what "started at" and --jit-resort-at count from)
    TakeListsBack();
    struct GiveAfter {
        ~GiveAfter() { GiveLists(); }
    } give_after; // (after the lock below is released)
    std::scoped_lock lock{mutex};
    compiled_total = 0;
    speculated_total = 0;
    recorded_total = 0;
    idle_ns = 0;
    const std::string dir = FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + folder;
    FileUtil::CreateFullPath(dir);
    path = dir + fmt::format("{:016X}.blocks", title_id);
    loaded.clear();
    known.clear();
    fresh.clear();
    const u64 read_t0 = hen_jit_now_ns();
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        // In one read: a record at a time through stdio took 1.3 s for Ocarina's 114,000 (and
        // delayed the first frame of every cached launch by that much).
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<Block> all(size > 0 ? static_cast<std::size_t>(size) / sizeof(Block) : 0);
        const std::size_t got = all.empty() ? 0 : std::fread(all.data(), sizeof(Block), all.size(), f);
        std::fclose(f);
        all.resize(got);
        known.reserve(got);
        loaded.reserve(got);
        for (const Block& b : all) {
            if (known.insert(Key(b)).second) {
                loaded.push_back(b);
            }
        }
    }
    std::printf("hen: jit list: read %zu blocks in %.0f ms\n", loaded.size(), (hen_jit_now_ns() - read_t0) / 1e6);
    const u64 t0 = hen_jit_now_ns();
    const u64 deadline = t0 + static_cast<u64>(start_budget_ms) * 1'000'000ull;
    for (JitList& list : jits) {
        Assign(list);
        while (!WorkerHasLists() && Work(list, deadline, true)) {
        }
    }
    start_ns = hen_jit_now_ns() - t0;
}

// The file is written by a thread of its own: an append to the card takes 30 to 90 ms (open,
// write, close), and this is called from the emulation thread every few seconds while a title
// compiles new code, which is all through a first launch. (Seen as two stalls a run, 52 + 33 ms
// in Kid Icarus and 54 + 39 ms in Smash, among those long enough to be reported.) The mutex is
// not held across the write either: the compile worker reports blocks under it.
// wait: the last call of a title; returns when everything is on the card and ends the thread.
static std::unique_ptr<Common::ThreadWorker> writer;

void Flush(bool wait) {
    std::vector<Block> out;
    std::string to;
    {
        std::scoped_lock lock{mutex};
        if (!fresh.empty() && !path.empty()) {
            out.swap(fresh);
            to = path;
        }
    }
    if (!out.empty()) {
        if (!writer) {
            writer = std::make_unique<Common::ThreadWorker>(1, "Block list writer");
        }
        writer->QueueWork([out = std::move(out), to = std::move(to)] {
            if (std::FILE* f = std::fopen(to.c_str(), "ab")) {
                std::fwrite(out.data(), sizeof(Block), out.size(), f);
                std::fclose(f);
            }
        });
    }
    if (wait && writer) {
        writer->WaitForRequests();
        writer.reset();
    }
}

void Attach(Dynarmic::A32::Jit* jit, u16 tag) {
    {
        std::scoped_lock lock{mutex};
        jit->HenSetTag(tag);
        JitList& list = jits.emplace_back();
        list.jit = jit;
        list.tag = tag;
        Assign(list);
        upgrade_jits.push_back(jit);
    }
    TakeListsBack(); // (the lists moved)
    GiveLists();
}

void Detach(Dynarmic::A32::Jit* jit) {
    TakeListsBack();
    struct GiveAfter {
        ~GiveAfter() { GiveLists(); }
    } give_after;
    std::scoped_lock lock{mutex};
    std::erase_if(jits, [jit](const JitList& list) { return list.jit == jit; });
    std::erase(upgrade_jits, jit);
}

// Once a frame, from the emulation thread (FrameLimiter::DoFrameLimiting): block links still
// in the table form become direct branches where their targets have been compiled since
// (dynarmic, AddressSpace::HenUpgradeLinks). A small budget a frame rather than idle time
// only: the titles that need the speed most never have idle time.
void UpgradeLinks() {
    // (No lock: this file's mutex is held by the worker while it reports blocks and during
    // file writes, and waiting for it here stalled the emulation thread for up to 45 ms a
    // time. The first version held it across the call into the JIT, which hung outright:
    // the worker takes the two locks the other way round. upgrade_jits is only touched on
    // this thread, in Attach, Detach and here.)
    const u64 now_ns = hen_jit_now_ns();
    const u64 deadline_ns = now_ns + 300'000;
    for (Dynarmic::A32::Jit* jit : upgrade_jits) {
        jit->HenUpgradeLinks(deadline_ns, 384);
        jit->HenResortTick(now_ns); // (whether a background re-sort of its code is to begin)
    }
}

void Idle(std::chrono::steady_clock::time_point deadline) {
    using namespace std::chrono;
    // Making the new code executable takes about half a millisecond after the last block.
    const auto left = duration_cast<nanoseconds>(deadline - steady_clock::now()).count() - 1'500'000;
    if (left < 500'000 || WorkerHasLists()) {
        return;
    }
    std::scoped_lock lock{mutex};
    const u64 t0 = hen_jit_now_ns();
    const u64 deadline_ns = t0 + static_cast<u64>(left);
    for (JitList& list : jits) {
        while (Work(list, deadline_ns)) {
        }
        if (hen_jit_now_ns() >= deadline_ns) {
            break;
        }
    }
    // With the lists done: on from the compiled blocks, along their branches.
    if (speculate) {
        for (JitList& list : jits) {
            while (hen_jit_now_ns() < deadline_ns) {
                const std::size_t compiled = list.jit->HenCompileFrontier(deadline_ns);
                if (compiled == 0) {
                    break;
                }
                speculated_total += compiled;
            }
        }
    }
    idle_ns += hen_jit_now_ns() - t0;
}

Stats GetStats() {
    std::scoped_lock lock{mutex};
    Stats stats{};
    stats.listed = loaded.size();
    stats.recorded = recorded_total;
    stats.compiled = compiled_total;
    stats.speculated = speculated_total;
    if (WorkerHasLists()) {
        stats.compiled = 0;
    }
    for (const JitList& list : jits) {
        if (WorkerHasLists()) {
            // (the worker writes the states: read as they are)
            const std::size_t done = static_cast<std::size_t>(std::count(list.state.begin(), list.state.end(), 1));
            stats.compiled += done;
            stats.waiting += list.blocks.size() - done;
        } else {
            stats.waiting += list.blocks.size() - list.done;
        }
    }
    stats.start_ms = start_ns / 1e6;
    stats.idle_ms = idle_ns / 1e6;
    return stats;
}

} // namespace Core::HenBlockList

/// From the JIT (dynarmic's AddressSpace::HenRecord), for every block it compiles.
extern "C" void hen_jit_record_block(unsigned tag, unsigned long long descriptor, unsigned words,
                                     unsigned hash) {
    using namespace Core::HenBlockList;
    const Block b{descriptor, hash, static_cast<u16>(words), static_cast<u16>(tag)};
    std::scoped_lock lock{mutex};
    if (known.insert(Key(b)).second) {
        fresh.push_back(b);
        recorded_total++;
    }
}
