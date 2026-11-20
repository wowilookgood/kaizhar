// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The background re-sort's parts that do not need the JIT (hen/jit/hen_resort.h), checked
// on the computer:
//   c++ -std=c++20 -O1 -I hen/jit -I externals/oaknut/include \
//       hen/tests/layout/resort_test.cpp -o /tmp/resort_test && /tmp/resort_test
//
//   1. The instructions a link is made of are the ones oaknut writes (which is what the code in
//      use is written with), and read back to the address they were written for.
//   2. The order is "pc" (hen_block_order.h), and the figure is its figure.
//   3. A made-up code buffer the size of a small title's (blocks in a compile order with link
//      stubs between, links of all three kinds in both forms, calls into a prelude, branches
//      inside blocks, blocks invalidated, blocks compiled while the copy is made) is re-sorted
//      the way dynarmic's AddressSpace::HenResort... does it, with this file's own small tables
//      in place of dynarmic's. Then: the copy passes the checks a development build makes;
//      every link of the copy leads to the copy of the block the old link led to (followed
//      through stubs and slots, not through the checks' own reading); the sorted part is dense
//      and in order; nothing in the copy names an address of the old buffer.
//   4. The checks find what they are for: each of a set of faults put into a good copy.
//   5. The rules for when to re-sort.
// What this cannot check is the JIT's side: the tables swapped, the publish, the change-over.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <oaknut/oaknut.hpp>

#include "hen_block_order.h"
#include "hen_resort.h"

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

static int g_failed = 0;
static int g_checks = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        g_checks++;                                       \
        if (!(cond)) {                                    \
            g_failed++;                                   \
            std::printf("FAILED %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
        }                                                 \
    } while (0)

// ---- 1. instructions -----------------------------------------------------------------------------

static void TestInstructions() {
    std::mt19937_64 rng(1);
    std::vector<u32> buffer(64);
    // (oaknut writes at a write pointer for an execute address: any address will do)
    for (int i = 0; i < 20000; i++) {
        const u64 base = (rng() & 0x0000007ffffff000ull) + 0x10000000ull;
        const u64 site = base + ((rng() % (64u << 20)) & ~u64(3));
        const u64 near = base + ((rng() % (127u << 20)) & ~u64(3));
        const u64 far = (rng() & 0x0000007ffffffffcull) + 0x10000000ull;
        // B
        {
            oaknut::CodeGenerator c{buffer.data(), reinterpret_cast<u32*>(site)};
            c.B(reinterpret_cast<const void*>(near));
            const u32 mine = HenResort::BranchTo(HenResort::kB, site, near);
            CHECK(mine == buffer[0], "B %llx -> %llx: %08x, oaknut %08x", (unsigned long long)site, (unsigned long long)near, mine, buffer[0]);
            CHECK(HenResort::IsBranch(mine) && HenResort::BranchTarget(mine, site) == near && HenResort::BranchReaches(site, near), "B read back");
            c.BL(reinterpret_cast<const void*>(near));
            const u32 bl = HenResort::BranchTo(buffer[1], site + 4, near);
            CHECK(bl == buffer[1] && HenResort::IsBranch(bl) && HenResort::BranchTarget(bl, site + 4) == near, "BL");
        }
        // ADRL (the page offset must fit ADRP: within 4 GiB)
        {
            const u64 target = (far & 0xfffff) + (site & ~u64(0xfffff)) + ((rng() % 2001) - 1000) * 0x100000ull;
            oaknut::CodeGenerator c{buffer.data(), reinterpret_cast<u32*>(site)};
            c.ADRL(oaknut::XReg{17}, reinterpret_cast<const void*>(target));
            u32 mine[2];
            HenResort::WriteAdrl(mine, 17, site, target);
            CHECK(mine[0] == buffer[0] && mine[1] == buffer[1], "ADRL %llx -> %llx: %08x %08x, oaknut %08x %08x", (unsigned long long)site,
                  (unsigned long long)target, mine[0], mine[1], buffer[0], buffer[1]);
            unsigned rd = 0;
            u64 back = 0;
            CHECK(HenResort::ReadAdrl(mine, site, &rd, &back) && rd == 17 && back == target, "ADRL read back");
        }
        // the 48-bit load, and LDR Xr, [Xr]
        {
            const u64 slot = rng() & 0x0000fffffffffff8ull;
            u32 mine[3];
            HenResort::WriteLoad48(mine, 16, slot);
            unsigned rd = 0;
            u64 back = 0;
            CHECK(HenResort::ReadLoad48(mine, &rd, &back) && rd == 16 && back == slot, "load 48 read back");
            oaknut::CodeGenerator c{buffer.data(), reinterpret_cast<u32*>(site)};
            c.LDR(oaknut::XReg{16}, oaknut::XReg{16});
            c.LDR(oaknut::XReg{17}, oaknut::XReg{17});
            c.NOP();
            CHECK(buffer[0] == HenResort::LoadThrough(16) && buffer[1] == HenResort::LoadThrough(17) && buffer[2] == HenResort::kNop, "LDR, NOP");
        }
    }
    CHECK(!HenResort::BranchReaches(0x10000000, 0x10000000 + (u64(1) << 27)), "reach: 128 MiB is too far");
    CHECK(HenResort::BranchReaches(0x10000000 + (u64(1) << 27), 0x10000004), "reach: just under 128 MiB back");
}

// ---- 2. order and figure ---------------------------------------------------------------------------

static void TestOrder() {
    std::mt19937_64 rng(2);
    // A list as the block list file has it: the blocks the program ran first, then speculation.
    const std::size_t used = 3000, count = 5000;
    std::vector<u64> descriptors(count);
    std::set<u64> seen;
    for (auto& d : descriptors) {
        do {
            d = (0x00100000u + (rng() % 0x80000) * 4) | (u64(rng() % 3 == 0 ? 0x01000001u : 0x01000000u) << 32);
        } while (!seen.insert(d).second);
    }
    const std::vector<u32> pc = Core::HenBlockOrder::Make(count, used, [&](u32 i) { return descriptors[i]; });
    std::vector<HenResort::Item> items;
    // (handed over in another order: the order compiled)
    std::vector<u32> shuffled(count);
    for (u32 i = 0; i < count; i++) {
        shuffled[i] = i;
    }
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    for (u32 k = 0; k < count; k++) {
        const u32 i = shuffled[k];
        items.push_back({descriptors[i], 0, 4 * (1 + static_cast<u32>(rng() % 100)), 0, k, static_cast<u8>(i >= used)});
    }
    HenResort::Sort(items);
    bool same = items.size() == pc.size();
    for (std::size_t k = 0; same && k < count; k++) {
        same = items[k].descriptor == descriptors[pc[k]];
    }
    CHECK(same, "the order is not hen_block_order.h's pc");
    const u64 end = HenResort::Assign(items, 0xc00);
    u64 at = 0xc00, figure = HenResort::FigureStart(0x001), theirs = Core::HenBlockOrder::LayoutStart(0x001);
    bool dense = true;
    for (const auto& item : items) {
        dense = dense && item.new_offset == at;
        at += item.size;
        figure = HenResort::FigureAdd(figure, item.descriptor, item.new_offset);
        theirs = Core::HenBlockOrder::LayoutAdd(theirs, item.descriptor, item.new_offset);
    }
    CHECK(dense && end == at, "the blocks are not one after the other");
    CHECK(figure == theirs, "the figure is not hen_block_order.h's");
}

// ---- 3. a made-up buffer, re-sorted ------------------------------------------------------------------

constexpr unsigned kRBranch = 16, kRMove = 17;
constexpr u64 kPreludeBytes = 0xc00;
constexpr u64 kExitUnlinked = 0x800;  // (offsets in the prelude that blocks and stubs lead to)
constexpr u64 kDispatcher = 0x700;
constexpr u64 kStateAnyRegister = 0xB9004790u;  // "STR W16, [X28, #0x44]": any word that is no branch

struct LinkSite {
    u32 offset;
    u8 kind;
    u64 target;  // descriptor
};
struct Block {
    u64 descriptor;
    u32 size;
    u8 speculative;
    std::vector<u32> prelude_sites;  // offsets
    std::vector<LinkSite> links;
    u64 entry = 0;  // in the buffer it lies in
    bool alive = true;
};

/// A code buffer with what dynarmic keeps beside it, as far as links go.
struct World {
    u64 base = 0;
    std::vector<u32> w;  // the write area
    u64 top = 0;
    std::unordered_map<u64, u64> entries;         // descriptor -> entry point
    std::map<u64, std::vector<u64>> slot_values;  // slot address -> its two words
    std::unordered_map<u64, u64> slot_of;         // descriptor -> slot address
    std::unordered_map<u64, u64> stub_of;
    std::unordered_set<u64> stubs;                // their addresses
    u64 next_slot = 0;
    u64 stub_next = 0, stub_end = 0;

    u32* At(u64 address) { return w.data() + (address - base) / 4; }
    u64 Slot(u64 target) {
        if (const auto have = slot_of.find(target); have != slot_of.end()) {
            return have->second;
        }
        const u64 slot = next_slot;
        next_slot += 16;
        const auto there = entries.find(target);
        const u64 entry = there != entries.end() ? there->second : 0;
        slot_values[slot] = {entry, entry != 0 ? entry : base + kDispatcher};
        slot_of[target] = slot;
        return slot;
    }
    u64 Stub(u64 target) {
        if (const auto have = stub_of.find(target); have != stub_of.end()) {
            return have->second;
        }
        if (stub_next == 0 || stub_end - stub_next < 40) {
            const u64 run = 400 * 40;
            std::fill(w.begin() + top / 4, w.begin() + (top + run) / 4, 0u);
            stub_next = base + top;
            stub_end = stub_next + run;
            top += run;
        }
        const u64 stub = stub_next;
        stub_next += 40;
        u32* const s = At(stub);
        HenResort::WriteLoad48(s, kRBranch, Slot(target));
        s[3] = HenResort::LoadThrough(kRBranch);
        s[4] = 0xB4000000u | (2u << 5) | 16u;
        s[5] = 0xD61F0200u;
        s[6] = 0x52800000u | ((static_cast<u32>(target) & 0xffff) << 5) | 16u;
        s[7] = 0x72a00000u | (((static_cast<u32>(target) >> 16) & 0xffff) << 5) | 16u;
        s[8] = static_cast<u32>(kStateAnyRegister);
        s[9] = HenResort::BranchTo(HenResort::kB, stub + 36, base + kExitUnlinked);
        stub_of[target] = stub;
        stubs.insert(stub);
        return stub;
    }
    /// Writes a block's link sites for where the block lies (dynarmic's HenWriteLinkSites, and
    /// HenResortLink for a copy).
    bool Link(const Block& block, u64 entry) {
        for (const LinkSite& link : block.links) {
            const auto there = entries.find(link.target);
            const u64 direct = there != entries.end() ? there->second : 0;
            const u64 through = direct != 0 ? 0 : link.kind == HenResort::LinkCompact ? Stub(link.target) : Slot(link.target);
            if (!HenResort::WriteSite(At(entry + link.offset), link.kind, entry + link.offset, direct, through, kRBranch, kRMove)) {
                return false;
            }
        }
        return true;
    }
    /// Where a link site leads when it runs: the entry point it reaches, 0 for "to the
    /// dispatcher". Follows the instructions, the stub and the slot's value.
    u64 Follow(const LinkSite& link, u64 entry) {
        const u32* const site = At(entry + link.offset);
        const u64 at = entry + link.offset;
        unsigned rd = 0;
        u64 value = 0;
        if (link.kind == HenResort::LinkMove) {
            if (HenResort::ReadAdrl(site, at, &rd, &value)) {
                return value;
            }
            if (!HenResort::ReadLoad48(site, &rd, &value) || slot_values.count(value - 8) == 0) {
                return ~u64(0);
            }
            const u64 pushed = slot_values[value - 8][1];
            return pushed == base + kDispatcher ? 0 : pushed;
        }
        if (HenResort::IsBranch(site[0])) {
            const u64 target = HenResort::BranchTarget(site[0], at);
            if (link.kind == HenResort::LinkBranch) {
                return target;
            }
            // (a compact site: its target's entry point, or a stub)
            if (stubs.count(target) != 0) {
                if (!HenResort::ReadLoad48(At(target), &rd, &value) || slot_values.count(value) == 0) {
                    return ~u64(0);
                }
                return slot_values[value][0];
            }
            return target;
        }
        if (!HenResort::ReadLoad48(site, &rd, &value) || slot_values.count(value) == 0) {
            return ~u64(0);
        }
        return slot_values[value][0];
    }
};

/// A word that is no branch and no ADRP, like the body of a block.
static u32 Filler(std::mt19937_64& rng) {
    for (;;) {
        const u32 word = static_cast<u32>(rng());
        if (!HenResort::IsBranch(word) && !HenResort::IsAdrp(word)) {
            return word;
        }
    }
}

struct Program {
    std::vector<Block> blocks;         // in the order compiled
    std::vector<u64> never_compiled;   // targets no block exists for
};

static Program MakeProgram(std::mt19937_64& rng, std::size_t count, bool compact) {
    Program program;
    std::set<u64> used;
    for (std::size_t i = 0; i < count + count / 20; i++) {
        u64 d;
        do {
            d = (0x00100000u + (rng() % (count * 8)) * 4) | (u64(rng() % 50 == 0 ? 0x01000001u : 0x01000000u) << 32);
        } while (!used.insert(d).second);
        if (i >= count) {
            program.never_compiled.push_back(d);
            continue;
        }
        Block block;
        block.descriptor = d;
        block.size = 4 * (12 + static_cast<u32>(rng() % 80));
        block.speculative = rng() % 3 == 0;
        program.blocks.push_back(block);
    }
    for (Block& block : program.blocks) {
        // What is where in the block: after four free words (an invalidated block's first four
        // are written over), sites that do not overlap, the rest filler.
        u32 at = 16;
        const u32 calls = static_cast<u32>(rng() % 3);
        for (u32 k = 0; k < calls && at + 8 < block.size; k++) {
            block.prelude_sites.push_back(at);
            at += 4 * (1 + static_cast<u32>(rng() % 3));
        }
        const u32 links = 1 + static_cast<u32>(rng() % 3);
        for (u32 k = 0; k < links; k++) {
            const u8 kind = rng() % 4 == 0 ? HenResort::LinkMove : compact ? HenResort::LinkCompact : HenResort::LinkBranch;
            const u32 words = HenResort::SiteWords(kind);
            if (at + 4 * words + 4 > block.size) {
                break;
            }
            const u64 target = rng() % 12 == 0 ? program.never_compiled[rng() % program.never_compiled.size()]
                                               : program.blocks[rng() % program.blocks.size()].descriptor;
            block.links.push_back({at, kind, target});
            at += 4 * words + 4 * static_cast<u32>(rng() % 2);
        }
    }
    return program;
}

/// The block's code as the emitter leaves it, at entry of world: filler, a branch inside the
/// block, its calls into the prelude, and its link sites as the emitter writes them.
static void EmitBlock(World& world, const Block& block, u64 entry, std::mt19937_64& rng) {
    u32* const words = world.At(entry);
    for (u32 i = 0; i < block.size / 4; i++) {
        words[i] = Filler(rng);
    }
    words[1] = HenResort::BranchTo(HenResort::kB, entry + 4, entry + 4 * (2 + rng() % (block.size / 4 - 2)));  // (to a label of the block)
    for (std::size_t k = 0; k < block.prelude_sites.size(); k++) {
        const u64 site = entry + block.prelude_sites[k];
        // (a call, a branch to the dispatcher, or "BR X17": the predicted return, which names no address)
        words[block.prelude_sites[k] / 4] = k % 3 == 0   ? HenResort::BranchTo(0x94000000u, site, world.base + 0x100 + 0x40 * (rng() % 8))
                                            : k % 3 == 1 ? HenResort::BranchTo(HenResort::kB, site, world.base + kDispatcher)
                                                         : 0xD61F0220u;
    }
    for (const LinkSite& link : block.links) {
        u32* const site = words + link.offset / 4;
        if (link.kind == HenResort::LinkBranch) {
            site[0] = site[1] = site[2] = HenResort::kNop;
            site[3] = HenResort::LoadThrough(kRBranch);
            site[4] = 0xB4000000u | (2u << 5) | 16u;
            site[5] = 0xD61F0200u;
        } else if (link.kind == HenResort::LinkMove) {
            site[0] = 0xD4200000u;
            site[1] = site[2] = site[3] = HenResort::kNop;
        } else {
            site[0] = HenResort::kNop;
        }
    }
}

struct Copy {
    World world;
    std::vector<Block> blocks;        // of the copy, in its order (entry: in the copy)
    std::vector<u64> old_entry;       // per block: where it was copied from
    u64 sorted_end = 0;
    std::size_t log_done = 0;
};

/// One block into the copy at offset (AddressSpace::HenResortAppend).
static void CopyBlock(Copy& copy, const World& old, const Block& block, u64 offset) {
    std::memcpy(copy.world.w.data() + offset / 4, old.w.data() + (block.entry - old.base) / 4, block.size);
    Block moved = block;
    moved.entry = copy.world.base + offset;
    copy.blocks.push_back(moved);
    copy.old_entry.push_back(block.entry);
}
/// Its calls into the prelude and its links (AddressSpace::HenResortLink).
static bool LinkBlock(Copy& copy, const World& old, std::size_t index) {
    const Block& block = copy.blocks[index];
    for (const u32 offset : block.prelude_sites) {
        u32* const word = copy.world.At(block.entry + offset);
        if (!HenResort::IsBranch(*word)) {
            continue;
        }
        const u64 old_target = HenResort::BranchTarget(*word, copy.old_entry[index] + offset);
        *word = HenResort::BranchTo(*word, block.entry + offset, copy.world.base + (old_target - old.base));
    }
    return copy.world.Link(block, block.entry);
}
/// Blocks compiled since (AddressSpace::HenResortAppendNew).
static bool AppendNew(Copy& copy, const World& old, const std::vector<Block>& log) {
    const std::size_t first = copy.blocks.size();
    for (; copy.log_done < log.size(); copy.log_done++) {
        const Block& block = log[copy.log_done];
        if (!block.alive) {
            continue;
        }
        const u64 at = copy.world.top;
        CopyBlock(copy, old, block, at);
        copy.world.top = at + block.size;
        copy.world.entries[block.descriptor] = copy.world.base + at;
    }
    for (std::size_t i = first; i < copy.blocks.size(); i++) {
        if (!LinkBlock(copy, old, i)) {
            return false;
        }
        if (const auto slot = copy.world.slot_of.find(copy.blocks[i].descriptor); slot != copy.world.slot_of.end()) {
            copy.world.slot_values[slot->second] = {copy.blocks[i].entry, copy.blocks[i].entry};
        }
    }
    return true;
}

static const char* CheckCopy(Copy& copy, const World& old, u64* where) {
    const HenResort::Bounds bounds{copy.world.base, old.base, kPreludeBytes, copy.world.top};
    std::vector<HenResort::Site> sites;
    std::vector<u8> marks;
    for (std::size_t i = 0; i < copy.blocks.size(); i++) {
        const Block& block = copy.blocks[i];
        sites.clear();
        for (const u32 offset : block.prelude_sites) {
            sites.push_back({offset, HenResort::Prelude, 0, 0});
        }
        for (const LinkSite& link : block.links) {
            const auto there = copy.world.entries.find(link.target);
            const auto slot = copy.world.slot_of.find(link.target);
            sites.push_back({link.offset, link.kind, there != copy.world.entries.end() ? there->second : 0,
                             slot != copy.world.slot_of.end() ? slot->second : 0});
        }
        u32 at = 0;
        if (const char* const why = HenResort::CheckBlock(copy.world.At(block.entry), old.w.data() + (copy.old_entry[i] - old.base) / 4,
                                                          block.size, block.entry, copy.old_entry[i], sites.data(), sites.size(), bounds,
                                                          copy.world.w.data(), marks, &at, kRBranch, kRMove)) {
            *where = block.descriptor;
            return why;
        }
    }
    u64 at = 0;
    if (const char* const why = HenResort::CheckRange(copy.world.w.data(), kPreludeBytes, copy.world.top, bounds, &at)) {
        *where = at;
        return why;
    }
    for (const auto& [slot, values] : copy.world.slot_values) {
        if (!HenResort::InCopy(values[0], bounds) || !HenResort::InCopy(values[1], bounds)) {
            *where = slot;
            return "a link slot of the copy holds an address outside the copy";
        }
    }
    return nullptr;
}

static void TestResort(bool compact, std::size_t count, unsigned seed) {
    std::mt19937_64 rng(seed);
    Program program = MakeProgram(rng, count, compact);
    const std::size_t words = (64u << 20) / 4;

    // The code in use: compiled in the list's order (which is no order of addresses), with
    // link stubs taken from the end in runs. The two buffers lie next to each other, so a
    // branch of one can reach the other: a link left behind would not fail by itself.
    World old;
    old.base = 0x2000000000ull;
    old.w.assign(words, 0);
    old.next_slot = 0x7000000000ull;
    for (u64 i = 0; i < kPreludeBytes / 4; i++) {
        old.w[i] = Filler(rng);
    }
    old.top = kPreludeBytes;
    const std::size_t at_snapshot = program.blocks.size() * 9 / 10;  // (the rest is compiled while the copy is made)
    const auto compile = [&](Block& block) {
        block.entry = old.base + old.top;
        old.top += block.size;
        EmitBlock(old, block, block.entry, rng);
        old.entries[block.descriptor] = block.entry;
        CHECK(old.Link(block, block.entry), "linking a block of the old code");
        if (const auto slot = old.slot_of.find(block.descriptor); slot != old.slot_of.end()) {
            old.slot_values[slot->second] = {block.entry, block.entry};
        }
    };
    for (std::size_t i = 0; i < at_snapshot; i++) {
        compile(program.blocks[i]);
    }
    // Some sites are upgraded to direct branches meanwhile (HenUpgradeLinks), and a few blocks
    // are invalidated before the snapshot (their code stays, their first words are written over).
    for (std::size_t i = 0; i < at_snapshot; i += 3) {
        CHECK(old.Link(program.blocks[i], program.blocks[i].entry), "upgrading links of the old code");
    }
    for (std::size_t i = 7; i < at_snapshot; i += 97) {
        Block& dead = program.blocks[i];
        dead.alive = false;
        old.entries.erase(dead.descriptor);
        if (const auto slot = old.slot_of.find(dead.descriptor); slot != old.slot_of.end()) {
            old.slot_values[slot->second] = {0, old.base + kDispatcher};
        }
        u32* const first = old.At(dead.entry);
        first[0] = 0x52800010u;
        first[1] = 0x72a00010u;
        first[2] = static_cast<u32>(kStateAnyRegister);
        first[3] = HenResort::BranchTo(HenResort::kB, dead.entry + 12, old.base + kDispatcher);
    }

    // The copy: the snapshot, sorted; then what is compiled meanwhile, in two rounds.
    Copy copy;
    copy.world.base = old.base + (u64(words) * 4);
    copy.world.w.assign(words, 0xDEADBEEFu);
    copy.world.next_slot = 0x7800000000ull;
    std::memcpy(copy.world.w.data(), old.w.data(), kPreludeBytes);
    std::vector<HenResort::Item> items;
    for (std::size_t i = 0; i < at_snapshot; i++) {
        const Block& block = program.blocks[i];
        if (block.alive) {
            items.push_back({block.descriptor, static_cast<u32>(block.entry - old.base), block.size, 0, static_cast<u32>(i), block.speculative});
        }
    }
    HenResort::Sort(items);
    copy.world.top = copy.sorted_end = HenResort::Assign(items, kPreludeBytes);
    for (const auto& item : items) {
        copy.world.entries[item.descriptor] = copy.world.base + item.new_offset;
    }
    for (const auto& item : items) {
        CopyBlock(copy, old, program.blocks[item.log_index], item.new_offset);
    }
    for (std::size_t i = 0; i < copy.blocks.size(); i++) {
        CHECK(LinkBlock(copy, old, i), "linking a block of the copy");
    }
    copy.log_done = at_snapshot;
    const std::size_t sorted_count = copy.blocks.size();
    const std::size_t half = at_snapshot + (program.blocks.size() - at_snapshot) / 2;
    for (std::size_t i = at_snapshot; i < half; i++) {
        compile(program.blocks[i]);
    }
    {
        const std::vector<Block> log(program.blocks.begin(), program.blocks.begin() + half);
        CHECK(AppendNew(copy, old, log), "the first round of blocks compiled meanwhile");
    }
    for (std::size_t i = half; i < program.blocks.size(); i++) {
        compile(program.blocks[i]);
    }
    CHECK(AppendNew(copy, old, program.blocks), "the second round");

    // The development build's checks pass.
    u64 where = 0;
    const char* const wrong = CheckCopy(copy, old, &where);
    CHECK(wrong == nullptr, "a good copy fails its check: %s (at %llx)", wrong ? wrong : "", (unsigned long long)where);

    // The sorted part is dense and in order; every live block is in the copy once.
    std::size_t alive = 0;
    for (const Block& block : program.blocks) {
        alive += block.alive ? 1 : 0;
    }
    CHECK(copy.blocks.size() == alive && copy.world.entries.size() == alive, "%zu blocks in the copy, %zu alive", copy.blocks.size(), alive);
    u64 expect = kPreludeBytes;
    bool ordered = true;
    for (std::size_t i = 0; i < sorted_count; i++) {
        ordered = ordered && copy.blocks[i].entry == copy.world.base + expect;
        expect += copy.blocks[i].size;
        if (i > 0) {
            const Block& a = copy.blocks[i - 1];
            const Block& b = copy.blocks[i];
            ordered = ordered && (a.speculative < b.speculative || (a.speculative == b.speculative && a.descriptor < b.descriptor));
        }
    }
    CHECK(ordered && expect == copy.sorted_end, "the sorted part is not dense and in order");

    // Every link of the copy leads where the old link led: to the copy of the same block, or
    // to the dispatcher where the target is not compiled (or was invalidated).
    std::unordered_map<u64, u64> old_descriptor_of;  // old entry -> descriptor
    for (const Block& block : program.blocks) {
        if (block.alive) {
            old_descriptor_of[block.entry] = block.descriptor;
        }
    }
    std::size_t links = 0, direct = 0, mismatched = 0;
    for (std::size_t i = 0; i < copy.blocks.size(); i++) {
        const Block& block = copy.blocks[i];
        Block before = block;
        before.entry = copy.old_entry[i];
        for (const LinkSite& link : block.links) {
            const u64 new_to = copy.world.Follow(link, block.entry);
            const u64 old_to = old.Follow(link, before.entry);
            // The old site may lead to an invalidated block's code (a direct branch is never
            // taken back: the tombstone sends it on to the dispatcher), which counts as 0.
            const auto old_block = old_descriptor_of.find(old_to);
            const u64 old_descriptor = old_to == 0 || old_block == old_descriptor_of.end() ? 0 : old_block->second;
            const auto new_entry = copy.world.entries.find(link.target);
            const u64 want = old_descriptor != 0 ? (new_entry != copy.world.entries.end() ? new_entry->second : ~u64(1)) : 0;
            // (a target compiled after the old site was written, with the old site not upgraded
            // yet, is reached through the old slot: old_to is its entry all the same)
            links++;
            direct += new_to != 0 && new_to == want ? 1 : 0;
            if (new_to != want) {
                // One case differs by design: the old site still went through a slot to a block
                // that is in the copy, or the reverse; both name the same block, so this is a fault.
                mismatched++;
            }
            if (old_descriptor != 0) {
                CHECK(old_descriptor == link.target, "an old link led to another block than its target");
            }
        }
    }
    CHECK(mismatched == 0, "%zu of %zu links of the copy do not lead where the old ones led", mismatched, links);
    CHECK(links > count && direct > links / 2, "too few links followed (%zu, %zu to blocks)", links, direct);

    // Nothing in the copy's code names the old buffer (the two are within a branch's reach).
    std::size_t into_old = 0;
    for (u64 offset = kPreludeBytes; offset < copy.world.top; offset += 4) {
        const u32 word = copy.world.w[offset / 4];
        if (HenResort::IsBranch(word)) {
            const u64 target = HenResort::BranchTarget(word, copy.world.base + offset);
            into_old += target >= old.base && target < old.base + u64(words) * 4 ? 1 : 0;
        }
    }
    CHECK(into_old == 0, "%zu branches of the copy lead into the old buffer", into_old);

    // ---- 4. the checks find faults ----
    const auto expect_fault = [&](const char* what, const auto& damage) {
        Copy broken = copy;
        damage(broken);
        u64 at = 0;
        CHECK(CheckCopy(broken, old, &at) != nullptr, "the checks did not find: %s", what);
    };
    // (a block of the sorted part with at least one link and one call)
    std::size_t victim = 0;
    for (std::size_t i = 0; i < sorted_count; i++) {
        bool branch_call = false;
        for (const u32 offset : copy.blocks[i].prelude_sites) {
            branch_call = branch_call || HenResort::IsBranch(*copy.world.At(copy.blocks[i].entry + offset));
        }
        bool direct_link = false;
        for (const LinkSite& link : copy.blocks[i].links) {
            direct_link = direct_link || (link.kind != HenResort::LinkMove && copy.world.entries.count(link.target) != 0);
        }
        if (branch_call && direct_link && copy.blocks[i].entry != copy.old_entry[i] + (copy.world.base - old.base)) {
            victim = i;
            break;
        }
    }
    const Block& v = copy.blocks[victim];
    expect_fault("a word of a block changed", [&](Copy& c) { *c.world.At(v.entry + 8) ^= 0x400; });
    expect_fault("a call into the prelude left as copied", [&](Copy& c) {
        for (const u32 offset : v.prelude_sites) {
            *c.world.At(v.entry + offset) = old.w[(c.old_entry[victim] - old.base + offset) / 4];
        }
    });
    expect_fault("a link left as copied (it leads into the old buffer)", [&](Copy& c) {
        for (const LinkSite& link : v.links) {
            for (unsigned k = 0; k < HenResort::SiteWords(link.kind); k++) {
                c.world.At(v.entry + link.offset)[k] = old.w[(c.old_entry[victim] - old.base + link.offset) / 4 + k];
            }
        }
    });
    expect_fault("a link to the wrong block", [&](Copy& c) {
        for (const LinkSite& link : v.links) {
            if (link.kind != HenResort::LinkMove && c.world.entries.count(link.target) != 0) {
                const u64 other = c.blocks[(victim + 5) % sorted_count].entry;
                *c.world.At(v.entry + link.offset) = HenResort::BranchTo(HenResort::kB, v.entry + link.offset, other);
            }
        }
    });
    expect_fault("a branch out of the copy from a block's body", [&](Copy& c) {
        *c.world.At(v.entry + 8) = HenResort::BranchTo(HenResort::kB, v.entry + 8, old.base + 0x4000);
    });
    expect_fault("a link slot with an old address", [&](Copy& c) {
        if (c.world.slot_values.empty()) {
            c.world.slot_values[1] = {old.base + 0x4000, old.base + 0x4000};
        } else {
            c.world.slot_values.begin()->second[0] = old.base + 0x4000;
        }
    });
    if (!copy.world.stub_of.empty()) {
        expect_fault("a stub that names another slot", [&](Copy& c) {
            for (const auto& [target, stub] : c.world.stub_of) {
                HenResort::WriteLoad48(c.world.At(stub), kRBranch, 0x7f00000000ull);
            }
        });
        expect_fault("a stub whose exit leads out of the prelude", [&](Copy& c) {
            for (const auto& [target, stub] : c.world.stub_of) {
                c.world.At(stub)[9] = HenResort::BranchTo(HenResort::kB, stub + 36, old.base + kExitUnlinked);
            }
        });
    }
    std::printf("  %s sites, %zu blocks (%zu alive, %zu sorted, %zu appended): %zu links followed, %zu to blocks, %zu stubs and %zu "
                "slots in the copy; code %llu -> %llu KiB\n",
                compact ? "compact" : "slot-form", program.blocks.size(), alive, sorted_count, copy.blocks.size() - sorted_count, links,
                direct, copy.world.stub_of.size(), copy.world.slot_of.size(), (unsigned long long)(old.top >> 10),
                (unsigned long long)(copy.world.top >> 10));
}

// ---- 5. when ---------------------------------------------------------------------------------------

static void TestPolicy() {
    using namespace HenResort;
    const u64 s = 1'000'000'000ull;
    PolicyIn quiet{};
    quiet.now_ns = 100 * s;
    quiet.window_known = true;
    quiet.window_blocks = 10;
    quiet.total_bytes = 30u << 20;
    quiet.unsorted_bytes = 30u << 20;
    CHECK(PolicyWait(quiet) == nullptr, "a settled title with unsorted code: %s", PolicyWait(quiet));
    PolicyIn in = quiet;
    in.total_bytes = in.unsorted_bytes = (1u << 20) - 4;
    CHECK(PolicyWait(in) != nullptr, "under a megabyte");
    in = quiet;
    in.unsorted_bytes = (3u << 20) - 4;
    CHECK(PolicyWait(in) != nullptr, "under a tenth unsorted");
    in.unsorted_bytes = 3u << 20;
    CHECK(PolicyWait(in) == nullptr, "a tenth unsorted");
    in = quiet;
    in.window_blocks = kQuietBlocks + 1;
    CHECK(PolicyWait(in) != nullptr, "compiling");
    in.window_blocks = kQuietBlocks;
    CHECK(PolicyWait(in) == nullptr, "at the quiet rate");
    in = quiet;
    in.window_known = false;
    CHECK(PolicyWait(in) != nullptr, "no whole window yet");
    in = quiet;
    in.list_pending = true;
    CHECK(PolicyWait(in) != nullptr, "the saved list's pass");
    in = quiet;
    in.busy_last_ns = 99 * s;
    CHECK(PolicyWait(in) != nullptr, "the title compiled a second ago");
    in.busy_last_ns = 98 * s;
    CHECK(PolicyWait(in) == nullptr, "two seconds ago");
    in = quiet;
    in.last_start_ns = 81 * s;
    CHECK(PolicyWait(in) != nullptr, "19 s after the last");
    in.last_start_ns = 80 * s;
    CHECK(PolicyWait(in) == nullptr, "20 s after the last");
    in = quiet;
    in.parked = true;
    CHECK(PolicyWait(in) != nullptr, "parked");
    // A session: code grows from 2 to 80 MiB; a re-sort at every tenth unsorted. How many?
    u64 total = 2u << 20, sorted = 0;
    unsigned resorts = 0;
    while (total < (80u << 20)) {
        total += 64u << 10;
        PolicyIn grow = quiet;
        grow.total_bytes = total;
        grow.unsorted_bytes = total - sorted;
        if (PolicyWait(grow) == nullptr) {
            sorted = total;
            resorts++;
        }
    }
    // (none below 12 MiB, so 18 where the 1 MiB rule gave 33)
    CHECK(resorts >= 15 && resorts <= 40, "%u re-sorts for 2 -> 80 MiB", resorts);
    std::printf("  2 -> 80 MiB of code at a tenth unsorted: %u re-sorts if the title were quiet throughout\n", resorts);
}

int main() {
    TestInstructions();
    TestOrder();
    std::printf("re-sorting made-up buffers:\n");
    TestResort(true, 20000, 3);
    TestResort(false, 20000, 4);
    TestResort(true, 60000, 5);
    TestPolicy();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) {
        std::printf("all checks passed\n");
    }
    return g_failed == 0 ? 0 : 1;
}
