// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The background re-sort of a JIT's code: what does not need the JIT.
//
// Generated code laid out densely in the order of the program's addresses runs 8 to 31% faster
// than the same blocks in the order they happened to be compiled (six titles). So while a
// title runs, the JIT's compile worker builds a second copy of all compiled blocks in
// that order in a spare code buffer, by copying each block's bytes and writing its links again
// for the new addresses (a block's code does not depend on where it lies until it is linked),
// and the emulation thread changes over to the copy between two slices. The whole of it
// is dynarmic's AddressSpace::HenResort... (backend/arm64/address_space.cpp, which includes this
// file by its path); here are the parts that can be checked on the computer
// (hen/tests/layout/resort_test.cpp):
//   - the order ("pc": blocks the program ran, by location; then speculation, by location);
//   - the instructions a link is made of, written and read back;
//   - the checks a development build makes on the copy before it is used;
//   - when to re-sort.
// Nothing here knows dynarmic, the console or the emulator.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace HenResort {

// ---- The order -------------------------------------------------------------------------------

struct Item {
    std::uint64_t descriptor;  ///< the block's location (upper half: mode bits; lower: address)
    std::uint32_t old_offset;  ///< of its code in the buffer it is copied from
    std::uint32_t size;        ///< of its code, bytes (a multiple of 4)
    std::uint32_t new_offset;  ///< in the copy (Assign)
    std::uint32_t log_index;   ///< its place in compile order
    std::uint8_t speculative;  ///< compiled without the program having asked for it
};

/// The order "pc" (hen/tests/layout/hen_block_order.h): the blocks the program ran first,
/// then those compiled on speculation, each group by location (the descriptor as a number: its
/// upper half, then the address). Ties (there are none among live blocks) by compile order.
inline void Sort(std::vector<Item>& items) {
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.speculative != b.speculative) {
            return a.speculative < b.speculative;
        }
        if (a.descriptor != b.descriptor) {
            return a.descriptor < b.descriptor;
        }
        return a.log_index < b.log_index;
    });
}

/// Each block right after the one before it, from start (the end of the prelude): no gaps, no
/// alignment beyond the 4 bytes of an instruction. Returns where the blocks end.
inline std::uint64_t Assign(std::vector<Item>& items, std::uint64_t start) {
    std::uint64_t at = start;
    for (Item& item : items) {
        item.new_offset = static_cast<std::uint32_t>(at);
        at += item.size;
    }
    return at;
}

// ---- The figure of a layout (the same formulas as hen_block_order.h; the test compares them) ----

inline std::uint64_t Mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ x >> 33;
}
inline std::uint64_t FigureStart(std::uint16_t tag) {
    return Mix(0x6c61796f75740000ull | tag);
}
inline std::uint64_t FigureAdd(std::uint64_t figure, std::uint64_t descriptor, std::uint64_t offset) {
    return Mix(figure ^ Mix(descriptor) ^ (offset + 1) * 0x9e3779b97f4a7c15ull);
}

// ---- Instructions ----------------------------------------------------------------------------

constexpr std::uint32_t kNop = 0xD503201Fu;
constexpr std::size_t kStubWords = 10;  ///< a link stub (address_space.cpp, HenLinkStub)

/// B or BL.
inline bool IsBranch(std::uint32_t word) {
    return (word & 0x7C000000u) == 0x14000000u;
}
/// Where a B or BL at address site goes.
inline std::uint64_t BranchTarget(std::uint32_t word, std::uint64_t site) {
    const std::int64_t imm = static_cast<std::int64_t>(static_cast<std::int32_t>(word << 6) >> 6);
    return site + static_cast<std::uint64_t>(imm * 4);
}
/// What a B at site can reach (the rule of address_space.cpp's HenBranchReaches).
inline bool BranchReaches(std::uint64_t site, std::uint64_t target) {
    const std::int64_t distance = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site);
    return distance > -(std::int64_t(1) << 27) && distance < (std::int64_t(1) << 27) && (distance & 3) == 0;
}
/// The same instruction (B or BL: the opcode bits of word) at site, going to target.
inline std::uint32_t BranchTo(std::uint32_t word, std::uint64_t site, std::uint64_t target) {
    const std::int64_t distance = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site);
    return (word & 0xFC000000u) | (static_cast<std::uint32_t>(distance >> 2) & 0x03FFFFFFu);
}
constexpr std::uint32_t kB = 0x14000000u;

/// ADRP.
inline bool IsAdrp(std::uint32_t word) {
    return (word & 0x9F000000u) == 0x90000000u;
}
/// "ADRL Xrd, target" at site, as oaknut writes it: ADRP Xrd, target's page; ADD Xrd, Xrd, the rest.
inline void WriteAdrl(std::uint32_t* out, unsigned rd, std::uint64_t site, std::uint64_t target) {
    const std::int64_t pages = (static_cast<std::int64_t>(target & ~std::uint64_t(0xfff)) -
                                static_cast<std::int64_t>(site & ~std::uint64_t(0xfff))) >> 12;
    const std::uint32_t imm = static_cast<std::uint32_t>(pages) & 0x1FFFFFu;
    out[0] = 0x90000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | rd;
    out[1] = 0x91000000u | (static_cast<std::uint32_t>(target & 0xfff) << 10) | (rd << 5) | rd;
}
/// False if the two words at site are not such a pair.
inline bool ReadAdrl(const std::uint32_t* in, std::uint64_t site, unsigned* rd, std::uint64_t* target) {
    if (!IsAdrp(in[0])) {
        return false;
    }
    const unsigned r = in[0] & 31u;
    if ((in[1] & 0xFFC00000u) != 0x91000000u || (in[1] & 31u) != r || ((in[1] >> 5) & 31u) != r) {
        return false;
    }
    const std::uint32_t imm = (((in[0] >> 5) & 0x7FFFFu) << 2) | ((in[0] >> 29) & 3u);
    const std::int64_t pages = static_cast<std::int64_t>(static_cast<std::int32_t>(imm << 11) >> 11);
    *rd = r;
    *target = (site & ~std::uint64_t(0xfff)) + static_cast<std::uint64_t>(pages * 4096) + ((in[1] >> 10) & 0xfffu);
    return true;
}

/// An address of up to 48 bits into Xrd, always three instructions (MOVZ and two MOVK): how a
/// link site or a stub names its target's link slot.
inline void WriteLoad48(std::uint32_t* out, unsigned rd, std::uint64_t address) {
    out[0] = 0xD2800000u | (0u << 21) | (static_cast<std::uint32_t>(address & 0xffff) << 5) | rd;
    out[1] = 0xF2800000u | (1u << 21) | (static_cast<std::uint32_t>((address >> 16) & 0xffff) << 5) | rd;
    out[2] = 0xF2800000u | (2u << 21) | (static_cast<std::uint32_t>((address >> 32) & 0xffff) << 5) | rd;
}
inline bool ReadLoad48(const std::uint32_t* in, unsigned* rd, std::uint64_t* address) {
    const unsigned r = in[0] & 31u;
    if ((in[0] & 0xFFE00000u) != 0xD2800000u || (in[1] & 0xFFE00000u) != 0xF2A00000u || (in[2] & 0xFFE00000u) != 0xF2C00000u ||
        (in[1] & 31u) != r || (in[2] & 31u) != r) {
        return false;
    }
    *rd = r;
    *address = static_cast<std::uint64_t>((in[0] >> 5) & 0xffffu) | (static_cast<std::uint64_t>((in[1] >> 5) & 0xffffu) << 16) |
               (static_cast<std::uint64_t>((in[2] >> 5) & 0xffffu) << 32);
    return true;
}
/// LDR Xr, [Xr].
inline std::uint32_t LoadThrough(unsigned r) {
    return 0xF9400000u | (r << 5) | r;
}

// ---- Link sites ------------------------------------------------------------------------------

/// What a recorded place in a block is (dynarmic's Relocation and BlockRelocationType).
enum SiteKind : std::uint8_t {
    Prelude = 0,      ///< one instruction: a B or BL into the buffer's prelude (or one that names no address)
    LinkBranch = 1,   ///< six instructions: "B target", or the slot form (load 48, LDR, CBZ, BR)
    LinkMove = 2,     ///< four: "ADRL X, target; NOP; NOP", or load 48 of the slot's second word and LDR
    LinkCompact = 3,  ///< one: "B target" or "B the target's stub"
};
inline unsigned SiteWords(std::uint8_t kind) {
    return kind == LinkBranch ? 6 : kind == LinkMove ? 4 : 1;
}

/// A link site written for the copy. direct: the target's entry point in the copy, or 0 when the
/// target is not in the copy; then through (LinkCompact: the target's stub in the copy; the
/// other two: the target's link slot). words: the site in the copy's write area, holding the
/// bytes copied from the old block. False: out of a branch's reach (the caller gives up).
inline bool WriteSite(std::uint32_t* words, std::uint8_t kind, std::uint64_t site, std::uint64_t direct, std::uint64_t through,
                      unsigned r_branch, unsigned r_move) {
    switch (kind) {
    case LinkCompact:
        if (!BranchReaches(site, direct != 0 ? direct : through)) {
            return false;
        }
        words[0] = BranchTo(kB, site, direct != 0 ? direct : through);
        return true;
    case LinkBranch:
        if (direct != 0) {
            if (!BranchReaches(site, direct)) {
                return false;
            }
            words[0] = BranchTo(kB, site, direct);  // (the five after it are never reached)
            return true;
        }
        // (the three after these are the emitter's and were never changed: LDR, CBZ, BR)
        WriteLoad48(words, r_branch, through);
        return true;
    case LinkMove:
        if (direct != 0) {
            WriteAdrl(words, r_move, site, direct);
            words[2] = kNop;
            words[3] = kNop;
            return true;
        }
        WriteLoad48(words, r_move, through + 8);
        words[3] = LoadThrough(r_move);
        return true;
    default:
        return false;
    }
}

// ---- Checks (development builds: before a copy is used) ----------------------------------------

struct Bounds {
    std::uint64_t new_base;     ///< execute address of the copy's buffer
    std::uint64_t old_base;     ///< of the buffer copied from
    std::uint64_t prelude_end;  ///< bytes: the prelude is [0, prelude_end) of either buffer
    std::uint64_t top;          ///< bytes: the copy's code ends here
};

/// A link site is right in either of its two forms: leading straight to its target's copy
/// (direct: that entry point, 0 if the target is not in the copy), or through the target's
/// link slot (through: the slot's address, 0 if the copy has none for it; a compact site by way
/// of a stub in the copy). Which form it has depends on whether the target was in the copy
/// when the site was written; a target added later is reached through the slot until the site
/// is upgraded, as in the code the copy was made from.
struct Site {
    std::uint32_t offset;   ///< in the block, bytes
    std::uint8_t kind;      ///< SiteKind
    std::uint64_t direct;
    std::uint64_t through;
};

/// One block of the copy against the block it was copied from. Every word outside the recorded
/// sites must be the old word, and must not be an instruction whose meaning depends on where it
/// lies and leads out of the block; every site must lead where the copy's tables say.
/// neu, old: the block's words in the two write areas; copy: the copy's whole write area (to
/// read a stub). marks: scratch. Returns nullptr, or what is wrong (*at: the word).
inline const char* CheckBlock(const std::uint32_t* neu, const std::uint32_t* old, std::uint32_t size, std::uint64_t neu_addr,
                              std::uint64_t old_addr, const Site* sites, std::size_t count, const Bounds& b,
                              const std::uint32_t* copy, std::vector<std::uint8_t>& marks, std::uint32_t* at, unsigned r_branch,
                              unsigned r_move) {
    const std::uint32_t words = size / 4;
    *at = 0;
    if ((size & 3) != 0 || neu_addr < b.new_base + b.prelude_end || neu_addr + size > b.new_base + b.top) {
        return "the block is not inside the copy";
    }
    marks.assign(words, 0);
    for (std::size_t s = 0; s < count; s++) {
        const std::uint32_t first = sites[s].offset / 4;
        const unsigned n = SiteWords(sites[s].kind);
        if ((sites[s].offset & 3) != 0 || first + n > words) {
            *at = first;
            return "a recorded site is outside its block";
        }
        for (unsigned k = 0; k < n; k++) {
            if (marks[first + k]) {
                *at = first + k;
                return "two recorded sites overlap";
            }
            marks[first + k] = 1;
        }
    }
    for (std::uint32_t i = 0; i < words; i++) {
        if (marks[i]) {
            continue;
        }
        *at = i;
        if (neu[i] != old[i]) {
            return "a word outside the recorded sites differs from the old block";
        }
        if (IsBranch(neu[i])) {
            const std::uint64_t target = BranchTarget(neu[i], neu_addr + 4 * std::uint64_t(i));
            if (target < neu_addr || target >= neu_addr + size) {
                return "a branch that is not a recorded site leaves its block";
            }
        }
        if (IsAdrp(neu[i])) {
            return "an ADRP that is not a recorded site";
        }
    }
    const auto in_prelude = [&b](std::uint64_t address) { return address >= b.new_base && address < b.new_base + b.prelude_end; };
    for (std::size_t s = 0; s < count; s++) {
        const std::uint32_t first = sites[s].offset / 4;
        const std::uint64_t site = neu_addr + sites[s].offset;
        const std::uint32_t* const w = neu + first;
        *at = first;
        switch (sites[s].kind) {
        case Prelude: {
            if (!IsBranch(w[0])) {
                if (w[0] != old[first]) {
                    return "a prelude site that is no branch differs from the old block";
                }
                break;
            }
            if (!IsBranch(old[first])) {
                return "a prelude site is a branch only in the copy";
            }
            const std::uint64_t target = BranchTarget(w[0], site);
            const std::uint64_t old_target = BranchTarget(old[first], old_addr + sites[s].offset);
            if (!in_prelude(target) || target - b.new_base != old_target - b.old_base || (w[0] >> 26) != (old[first] >> 26)) {
                return "a prelude site does not lead to the same place in the copy's prelude";
            }
            break;
        }
        case LinkCompact: {
            if (!IsBranch(w[0]) || (w[0] >> 26) != (kB >> 26)) {
                return "a compact link site is not a branch";
            }
            const std::uint64_t target = BranchTarget(w[0], site);
            if (sites[s].direct != 0 && target == sites[s].direct) {
                break;
            }
            if (sites[s].through == 0) {
                return "a compact link site leads neither to its target's copy nor to a stub of a known link slot";
            }
            if (target < b.new_base + b.prelude_end || target + 4 * kStubWords > b.new_base + b.top || (target & 3) != 0) {
                return "a compact link site's stub is not inside the copy";
            }
            const std::uint32_t* const stub = copy + (target - b.new_base) / 4;
            unsigned rd = 0;
            std::uint64_t slot = 0;
            if (!ReadLoad48(stub, &rd, &slot) || slot != sites[s].through) {
                return "a link stub does not name its target's link slot";
            }
            if (!IsBranch(stub[kStubWords - 1]) || !in_prelude(BranchTarget(stub[kStubWords - 1], target + 4 * (kStubWords - 1)))) {
                return "a link stub's exit does not lead into the copy's prelude";
            }
            break;
        }
        case LinkBranch: {
            if (IsBranch(w[0])) {
                if (sites[s].direct == 0 || BranchTarget(w[0], site) != sites[s].direct) {
                    return "a link site does not lead to its target's copy";
                }
                break;
            }
            unsigned rd = 0;
            std::uint64_t slot = 0;
            if (sites[s].through == 0 || !ReadLoad48(w, &rd, &slot) || rd != r_branch || slot != sites[s].through) {
                return "a link site does not name its target's link slot";
            }
            if (w[3] != old[first + 3] || w[4] != old[first + 4] || w[5] != old[first + 5] || w[3] != LoadThrough(r_branch)) {
                return "the rest of a link site in the slot form is not what the emitter wrote";
            }
            break;
        }
        case LinkMove: {
            unsigned rd = 0;
            std::uint64_t value = 0;
            if (IsAdrp(w[0])) {
                if (sites[s].direct == 0 || !ReadAdrl(w, site, &rd, &value) || rd != r_move || value != sites[s].direct ||
                    w[2] != kNop || w[3] != kNop) {
                    return "a return address site does not name its target's copy";
                }
                break;
            }
            if (sites[s].through == 0 || !ReadLoad48(w, &rd, &value) || rd != r_move || value != sites[s].through + 8 ||
                w[3] != LoadThrough(r_move)) {
                return "a return address site does not name its target's link slot";
            }
            break;
        }
        default:
            return "a recorded site of an unknown kind";
        }
    }
    return nullptr;
}

/// Every branch and every ADRP in the copy's code, [from, to) bytes of its write area, must stay
/// inside the copy: nothing may lead into the buffer it was copied from, or anywhere else.
/// (Stub runs are zeroed when they are taken, so what was never written is no instruction.)
inline const char* CheckRange(const std::uint32_t* copy, std::uint64_t from, std::uint64_t to, const Bounds& b, std::uint64_t* at) {
    for (std::uint64_t offset = from; offset + 4 <= to; offset += 4) {
        const std::uint32_t word = copy[offset / 4];
        const std::uint64_t site = b.new_base + offset;
        if (IsBranch(word)) {
            const std::uint64_t target = BranchTarget(word, site);
            if (target < b.new_base || target >= b.new_base + b.top) {
                *at = offset;
                return "a branch in the copy leads out of the copy";
            }
        } else if (IsAdrp(word)) {
            const std::uint32_t imm = (((word >> 5) & 0x7FFFFu) << 2) | ((word >> 29) & 3u);
            const std::int64_t pages = static_cast<std::int64_t>(static_cast<std::int32_t>(imm << 11) >> 11);
            const std::uint64_t page = (site & ~std::uint64_t(0xfff)) + static_cast<std::uint64_t>(pages * 4096);
            if (page + 0x1000 <= b.new_base || page >= b.new_base + b.top) {
                *at = offset;
                return "an ADRP in the copy names a page outside the copy";
            }
        }
    }
    return nullptr;
}

/// A value kept in a table or a link slot of the copy: 0, or an address inside the copy.
inline bool InCopy(std::uint64_t value, const Bounds& b) {
    return value == 0 || (value >= b.new_base && value < b.new_base + b.top);
}

// ---- When to re-sort ---------------------------------------------------------------------------
//
// Nothing here is set per title. Where each number comes from:
//   - kMinBytes: 12 MiB. A re-sort needs a second code buffer, 258 MiB
//     of heap, and the gain was only ever measured on a title's main JIT. In six titles
//     those have 15.2 to 71 MiB of code at their first re-sort; everything else that reaches
//     1 MiB has 1.0 to 7.6 MiB (a title's second core, and with the online modules on each
//     system module's process). At 1 MiB a session with the modules on re-sorted five such JITs,
//     held nine second buffers in two titles and ended in std::bad_alloc. 12 MiB lies between
//     the two groups; a title with less code than that is not re-sorted until it has more.
//   - an idle JIT's copy is thrown away after two seconds (address_space.cpp, HenResortTick);
//     each such time in a row doubles the wait before the next try (kMinIntervalNs, up to 16
//     times): one module's JIT was copied 18 times in one session for nothing.
//   - kShare: a re-sort when a tenth of the code lies outside the sorted part. The measured gain
//     of sorted code over compile order is 8 to 31% of the time in generated code; with a tenth
//     unsorted about a tenth of that is being lost, 1 to 3%, against a cost to the emulation
//     thread of one pause under 2 ms and one refill of the dispatcher's table and the caches (a
//     few ms): paid back within a second. A tenth also bounds the number of re-sorts by the
//     code's growth: at most 25 a tenfold growth.
//   - kWindowNs, kQuietBlocks: "the compile burst has settled" means the JIT compiled no more
//     than 256 blocks a second over the last two seconds. That is the rate at which the blocks
//     that arrive while a copy is being built (about a second) still fit the catch-up at the
//     switch (kCatchUp blocks, about 2 us each: half a millisecond).
//   - kMinIntervalNs: at most three starts a minute for one JIT: a build is up to about a second
//     of the worker, so at most 5% of the spare core.
//   - kCatchUp: more blocks than this waiting at the switch go back to the worker.
//   - kRounds: a copy that is still being added to after this many rounds is given up.
constexpr std::uint64_t kMinBytes = 12u << 20;
constexpr std::uint64_t kShare = 10;
constexpr std::uint64_t kWindowNs = 2'000'000'000ull;
constexpr std::uint64_t kQuietBlocks = 512;
constexpr std::uint64_t kMinIntervalNs = 20'000'000'000ull;
constexpr std::size_t kCatchUp = 256;
constexpr unsigned kRounds = 64;

struct PolicyIn {
    std::uint64_t now_ns;
    std::uint64_t last_start_ns;    ///< when this JIT's last re-sort began (0: never)
    std::uint64_t busy_last_ns;     ///< when the title was last seen compiling a lot or interpreting new code (0: never)
    std::uint64_t window_blocks;    ///< blocks this JIT compiled in the last whole window
    bool window_known;              ///< a whole window has passed
    bool list_pending;              ///< the worker is still working through the saved list
    bool parked;
    std::uint64_t total_bytes;      ///< of all this JIT's blocks
    std::uint64_t unsorted_bytes;   ///< of them, compiled since the last re-sort (all of it before the first)
    unsigned idle_abandons = 0;     ///< copies in a row thrown away because the JIT ran no slice to take them
};

/// nullptr: start a re-sort now. Otherwise why not.
inline const char* PolicyWait(const PolicyIn& in) {
    if (in.parked) {
        return "the JIT is parked";
    }
    if (in.total_bytes < kMinBytes) {
        return "under 12 MiB of code";
    }
    if (in.unsorted_bytes * kShare < in.total_bytes) {
        return "under a tenth of the code is unsorted";
    }
    if (in.list_pending) {
        return "the saved list is still being compiled";
    }
    if (!in.window_known || in.window_blocks > kQuietBlocks) {
        return "the JIT is compiling";
    }
    if (in.busy_last_ns != 0 && in.now_ns - in.busy_last_ns < kWindowNs) {
        return "the title is compiling or interpreting new code";
    }
    if (in.last_start_ns != 0 && in.now_ns - in.last_start_ns < (kMinIntervalNs << (in.idle_abandons < 4 ? in.idle_abandons : 4))) {
        return "the last re-sort began less than 20 s ago (longer after copies an idle JIT did not take)";
    }
    return nullptr;
}

}  // namespace HenResort
