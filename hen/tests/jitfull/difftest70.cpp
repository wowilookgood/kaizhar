// Differential run of dynarmic's arm64 backend with the port's link schemes, natively on the
// host (arm64 macOS): random branchy ARM programs run to their end under each configuration,
// in short time slices, with link upgrades, cache invalidations, code patches and cache clears
// in between. Every configuration must end with the same registers, flags, memory and tick
// count; the configurations without the compile worker must also stop each slice at the same
// place.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <chrono>
#include <optional>
#include <vector>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"

extern "C" {
extern int hen_link_table, hen_direct_links, hen_compact_links, hen_translation_worker, hen_exit_counts, shim_alias_active, hen_emu_ahead;
extern unsigned long long hen_exit_count[8];
extern std::atomic<unsigned long long> shim_worker_blocks, shim_compact[6], shim_links[3], shim_publishes, shim_ranges, shim_clears;
}

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 CODE_BASE = 0x00100000, CODE_SIZE = 0x00100000;  // 1 MiB
constexpr u32 DATA_BASE = 0x00300000, DATA_SIZE = 0x00001000;

enum { EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL };

static u32 dp_imm(u32 cond, u32 op, u32 s, u32 rn, u32 rd, u32 imm8) { return cond << 28 | 1u << 25 | op << 21 | s << 20 | rn << 16 | rd << 12 | imm8; }
static u32 dp_reg(u32 cond, u32 op, u32 s, u32 rn, u32 rd, u32 rm, u32 sh, u32 type) { return cond << 28 | op << 21 | s << 20 | rn << 16 | rd << 12 | sh << 7 | type << 5 | rm; }
static u32 branch(u32 cond, u32 from, u32 to, bool link) { return cond << 28 | (link ? 0xBu : 0xAu) << 24 | (((to - from - 8) >> 2) & 0xffffff); }

struct Program {
    std::vector<u32> code;        // words at CODE_BASE
    std::vector<u32> data;        // words at DATA_BASE
    std::vector<u32> patch_slots; // addresses of "ADD r0, r0, #imm" that SVC 1 rewrites
    u32 loops;
    u32 main_words;  // the words before the functions (ARM code, every one the start of some block)
};

static Program Generate(u32 seed, u32 segments) {
    std::mt19937 rng{seed};
    const auto rnd = [&](u32 n) { return static_cast<u32>(rng() % n); };
    Program p;
    p.data.assign(DATA_SIZE / 4, 0);
    for (u32 i = 0; i < 256; i++) {
        p.data[i] = rng();
    }
    struct Fix {
        u32 at;      // word index
        u32 kind;    // 0 branch, 1 branch with link, 2 data word (address of a segment), 3 BLX to a Thumb function,
                     // 4 data word (address of a Thumb function, bit 0 set)
        u32 cond;
        u32 target;  // segment or function index
        bool func;
    };
    std::vector<Fix> fixes;
    std::vector<u32> seg_at(segments + 1), func_at;
    const u32 functions = 8 + rnd(24);
    u32 table_next = 256 + functions;  // (256 + f: where function f's address is, for the Thumb ones)
    const auto body = [&](u32 count) {
        for (u32 k = 0; k < count; k++) {
            const u32 cond = rnd(10) < 7 ? AL : rnd(14);
            const u32 rd = rnd(8), rn = rnd(8), rm = rnd(8);
            switch (rnd(12)) {
            case 0: case 1: case 2: {
                static const u32 ops[] = {0, 1, 2, 3, 4, 5, 6, 12, 14};
                p.code.push_back(dp_imm(cond, ops[rnd(9)], rnd(2), rn, rd, rnd(256)));
                break;
            }
            case 3: case 4: case 5: {
                static const u32 ops[] = {0, 1, 2, 3, 4, 5, 6, 12, 14};
                p.code.push_back(dp_reg(cond, ops[rnd(9)], rnd(2), rn, rd, rm, rnd(32), rnd(3)));
                break;
            }
            case 6:
                p.code.push_back(dp_imm(cond, 10, 1, rn, 0, rnd(256)));  // CMP rn, #imm
                break;
            case 7:
                p.code.push_back(dp_reg(cond, 10 + rnd(2), 1, rn, 0, rm, 0, 0));  // CMP/CMN rn, rm
                break;
            case 8:
                p.code.push_back(cond << 28 | 0x05900000 | 8u << 16 | rd << 12 | rnd(256) * 4);  // LDR rd, [r8, #off]
                break;
            case 9:
                p.code.push_back(cond << 28 | 0x05800000 | 8u << 16 | rd << 12 | rnd(256) * 4);  // STR rd, [r8, #off]
                break;
            case 10:
                p.patch_slots.push_back(CODE_BASE + 4 * static_cast<u32>(p.code.size()));
                p.code.push_back(dp_imm(AL, 4, 0, 0, 0, rnd(256)));  // ADD r0, r0, #imm
                break;
            default:
                p.code.push_back(dp_imm(cond, 13, rnd(2), 0, rd, rnd(256)));  // MOV rd, #imm
                break;
            }
        }
    };
    for (u32 i = 0; i < segments; i++) {
        seg_at[i] = static_cast<u32>(p.code.size());
        body(rnd(6));
        const u32 forward = std::min(segments - 1, i + 1 + rnd(8));
        const u32 t = rnd(100);
        if (i == segments - 1) {
            break;
        } else if (t < 15) {
            // falls through
        } else if (t < 45) {
            fixes.push_back({static_cast<u32>(p.code.size()), 0, rnd(14), forward, false});
            p.code.push_back(0);
        } else if (t < 60) {
            fixes.push_back({static_cast<u32>(p.code.size()), 0, AL, forward, false});
            p.code.push_back(0);
        } else if (t < 70) {
            // A call. Odd functions are Thumb code: BLX to them, directly or through r9.
            const u32 f = rnd(functions);
            if (f % 2 == 0) {
                fixes.push_back({static_cast<u32>(p.code.size()), 1, rnd(4) == 0 ? rnd(14) : u32(AL), f, true});
                p.code.push_back(0);
            } else if (rnd(2) == 0) {
                fixes.push_back({static_cast<u32>(p.code.size()), 3, AL, f, true});
                p.code.push_back(0);
            } else {
                p.code.push_back(0xE5900000 | 8u << 16 | 9u << 12 | (256 + f) * 4);  // LDR r9, [r8, #table]
                p.code.push_back(0xE12FFF39);                                         // BLX r9
            }
        } else if (t < 76) {
            p.code.push_back(dp_imm(AL, 2, 1, 10, 10, 1));  // SUBS r10, r10, #1
            fixes.push_back({static_cast<u32>(p.code.size()), 0, LE, segments, false});
            p.code.push_back(0);
            // (back a few times, then on: a loop that only ended with r10 would use it all up)
            p.code.push_back(dp_imm(AL, 8, 1, 10, 0, 1u << rnd(3) | 1));  // TST r10, #mask
            fixes.push_back({static_cast<u32>(p.code.size()), 0, NE, i - std::min(i, rnd(11)), false});
            p.code.push_back(0);
        } else if (t < 92 && table_next < 1000) {
            fixes.push_back({table_next, 2, 0, forward, false});
            p.code.push_back(0xE5900000 | 8u << 16 | 9u << 12 | table_next * 4);  // LDR r9, [r8, #table]
            p.code.push_back(0xE12FFF19);                                          // BX r9
            table_next++;
        } else if (t < 97) {
            p.code.push_back(0xEF000001);  // SVC 1
        }
    }
    // The last segment: round again until r10 runs out. (The latches' way out skips this.)
    p.code.push_back(dp_imm(AL, 2, 1, 10, 10, 1));  // SUBS r10, r10, #1
    fixes.push_back({static_cast<u32>(p.code.size()), 0, GT, 0, false});
    p.code.push_back(0);
    seg_at[segments] = static_cast<u32>(p.code.size());
    p.code.push_back(0xEF000000);  // SVC 0: the end
    p.code.push_back(0xEAFFFFFE);  // B .
    p.main_words = static_cast<u32>(p.code.size());
    for (u32 f = 0; f < functions; f++) {
        func_at.push_back(static_cast<u32>(p.code.size()));
        if (f % 2 == 0) {
            body(1 + rnd(5));
            p.code.push_back(0xE12FFF1E);  // BX lr
            continue;
        }
        // Thumb: a few flag-setting operations and forward branches over one or two of them.
        std::vector<std::uint16_t> t;
        const u32 count = 2 + rnd(8);
        for (u32 k = 0; k < count; k++) {
            const u32 left = count - 1 - k;
            switch (rnd(6)) {
            case 0: case 1:
                t.push_back(static_cast<std::uint16_t>(0x3000 | rnd(8) << 8 | rnd(256)));  // ADDS rd, #imm
                break;
            case 2:
                t.push_back(static_cast<std::uint16_t>(0x4040 | rnd(8) << 3 | rnd(8)));  // EORS rd, rm
                break;
            case 3:
                t.push_back(static_cast<std::uint16_t>(0x2800 | rnd(8) << 8 | rnd(256)));  // CMP rn, #imm
                break;
            case 4:
                if (left >= 1) {
                    t.push_back(static_cast<std::uint16_t>(0xD000 | rnd(14) << 8 | (std::min(left, 1 + rnd(2)) - 1)));  // B<cond> over one or two
                    break;
                }
                [[fallthrough]];
            default:
                if (left >= 2 && rnd(3) == 0) {
                    t.push_back(static_cast<std::uint16_t>(0xE000 | 0));  // B over one
                } else {
                    t.push_back(static_cast<std::uint16_t>(0x1C00 | rnd(8) << 6 | rnd(8) << 3 | rnd(8)));  // ADDS rd, rn, #imm3
                }
                break;
            }
        }
        t.push_back(0x4770);  // BX lr
        if (t.size() % 2) {
            t.push_back(0x46C0);  // NOP
        }
        for (size_t i = 0; i < t.size(); i += 2) {
            p.code.push_back(static_cast<u32>(t[i]) | static_cast<u32>(t[i + 1]) << 16);
        }
    }
    for (u32 f = 1; f < functions; f += 2) {
        p.data[256 + f] = (CODE_BASE + 4 * func_at[f]) | 1;
    }
    for (const Fix& f : fixes) {
        const u32 to = CODE_BASE + 4 * (f.func ? func_at[f.target] : seg_at[f.target]);
        if (f.kind == 2) {
            p.data[f.at] = to;
        } else if (f.kind == 3) {
            p.code[f.at] = 0xFA000000 | (((to - (CODE_BASE + 4 * f.at) - 8) >> 2) & 0xffffff);  // BLX label
        } else {
            p.code[f.at] = branch(f.cond, CODE_BASE + 4 * f.at, to, f.kind == 1);
        }
    }
    p.loops = 20000 + rnd(60000);
    if (p.code.size() * 4 > CODE_SIZE) {
        std::fprintf(stderr, "program too large\n");
        std::abort();
    }
    return p;
}

struct Slice {
    u32 pc, cpsr;
    u64 ticks;
    u32 hash;
    bool operator==(const Slice&) const = default;
};

struct Result {
    std::array<u32, 16> regs;
    u32 cpsr;
    u64 ticks;
    u32 data_hash, code_hash, svcs;
    std::vector<Slice> slices;
    bool SameEnd(const Result& o) const {
        return regs == o.regs && cpsr == o.cpsr && ticks == o.ticks && data_hash == o.data_hash && code_hash == o.code_hash && svcs == o.svcs;
    }
};

static u32 Hash(const u8* p, size_t n) {
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

struct Env final : Dynarmic::A32::UserCallbacks {
    std::vector<u8> code = std::vector<u8>(CODE_SIZE), data = std::vector<u8>(DATA_SIZE);
    std::unique_ptr<std::array<u8*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>> table = std::make_unique<std::array<u8*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>();
    Dynarmic::A32::Jit* jit = nullptr;
    const Program* program = nullptr;
    std::mt19937 svc_rng;
    u64 ticks = 0;
    long long remaining = 0;
    u32 svcs = 0;
    bool done = false, bad = false;

    u8* At(u32 a) {
        if (a - CODE_BASE < CODE_SIZE) {
            return &code[a - CODE_BASE];
        }
        if (a - DATA_BASE < DATA_SIZE) {
            return &data[a - DATA_BASE];
        }
        bad = true;
        static u8 sink[16];
        return sink;
    }
    template<typename T>
    T Read(u32 a) {
        T v;
        std::memcpy(&v, At(a), sizeof(T));
        return v;
    }
    template<typename T>
    void Write(u32 a, T v) { std::memcpy(At(a), &v, sizeof(T)); }

    std::optional<u32> MemoryReadCode(u32 a) override { return Read<u32>(a); }
    u8 MemoryRead8(u32 a) override { return Read<u8>(a); }
    std::uint16_t MemoryRead16(u32 a) override { return Read<std::uint16_t>(a); }
    u32 MemoryRead32(u32 a) override { return Read<u32>(a); }
    u64 MemoryRead64(u32 a) override { return Read<u64>(a); }
    void MemoryWrite8(u32 a, u8 v) override { Write(a, v); }
    void MemoryWrite16(u32 a, std::uint16_t v) override { Write(a, v); }
    void MemoryWrite32(u32 a, u32 v) override { Write(a, v); }
    void MemoryWrite64(u32 a, u64 v) override { Write(a, v); }
    void InterpreterFallback(u32, size_t) override { bad = true; jit->HaltExecution(); }
    void ExceptionRaised(u32, Dynarmic::A32::Exception) override { bad = true; jit->HaltExecution(); }
    void CallSVC(u32 swi) override {
        svcs++;
        if (swi == 0) {
            done = true;
            jit->HaltExecution();
        } else if (!program->patch_slots.empty() && std::getenv("NOPATCH") == nullptr) {
            // The program changes its own code: one "ADD r0, r0, #imm" gets a new immediate.
            const u32 at = program->patch_slots[svc_rng() % program->patch_slots.size()];
            Write<u32>(at, dp_imm(AL, 4, 0, 0, 0, svc_rng() & 0xff));
            jit->InvalidateCacheRange(at, 4);
        }
    }
    void AddTicks(u64 t) override {
        ticks += t;
        remaining -= static_cast<long long>(t);
    }
    u64 GetTicksRemaining() override { return remaining > 0 ? static_cast<u64>(remaining) : 0; }
};

struct Config {
    const char* name;
    int link_table, direct, compact, worker, alias, counters;
    bool deterministic;
};

static size_t small_cache = 0;
static bool grow_test = false;
extern "C" int hen_buffer_grow, hen_resort;
extern "C" unsigned long long hen_jit_now_ns(void);
static const struct Config* reference_config = nullptr;
static unsigned long long deferred_total = 0;
extern "C" int hen_tier_force;

static bool Run(const Program& p, u32 seed, const Config& c, Result& out, bool keep_slices) {
    hen_link_table = c.link_table;
    hen_direct_links = c.direct;
    hen_compact_links = c.compact;
    hen_translation_worker = c.worker;
    shim_alias_active = c.alias;
    hen_exit_counts = c.counters;
    hen_emu_ahead = c.worker ? static_cast<int>(seed % 3) * 2 : 4;  // (0: everything ahead is the worker's)

    Env env;
    std::memcpy(env.code.data(), p.code.data(), p.code.size() * 4);
    std::memcpy(env.data.data(), p.data.data(), p.data.size() * 4);
    env.table->fill(nullptr);
    for (u32 a = 0; a < CODE_SIZE; a += 0x1000) {
        (*env.table)[(CODE_BASE + a) >> 12] = &env.code[a];
    }
    for (u32 a = 0; a < DATA_SIZE; a += 0x1000) {
        (*env.table)[(DATA_BASE + a) >> 12] = &env.data[a];
    }
    env.program = &p;
    env.svc_rng.seed(seed ^ 0x5bd1e995);

    Dynarmic::A32::UserConfig conf;
    conf.callbacks = &env;
    conf.page_table = env.table.get();
    conf.define_unpredictable_behaviour = true;
    conf.code_cache_size = small_cache && &c != reference_config ? small_cache : 64 * 1024 * 1024;
    Dynarmic::A32::Jit jit{conf};
    env.jit = &jit;
    if (c.worker) {
        jit.HenSetTag(0);
    }
    jit.Regs().fill(0);
    for (u32 r = 0; r < 8; r++) {
        jit.Regs()[r] = seed * 2654435761u + r * 40503u;
    }
    jit.Regs()[8] = DATA_BASE;
    jit.Regs()[10] = p.loops;
    jit.Regs()[15] = CODE_BASE;
    jit.SetCpsr(0x10);

    std::mt19937 rng{seed ^ 0x9e3779b9};
    u64 slices = 0;
    while (!env.done && !env.bad) {
        env.remaining = rng() % 50 == 0 ? 5000 : 1 + rng() % 64;
        Dynarmic::HaltReason hr = jit.Run();
        // "Interpret this block" (the code buffer is full): this test has no interpreter,
        // so it answers as the interpreter does for a block it cannot run, and comes back.
        unsigned spins = 0;
        while (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2) && !env.done && !env.bad) {
            hen_tier_force = 1;
            deferred_total++;
            if (++spins > 100000) {
                std::printf("  %s: sent to the interpreter 100000 times in a row at pc %08x\n", c.name, jit.Regs()[15]);
                return false;
            }
            hr = jit.Run();
        }
        slices++;
        if (keep_slices) {
            out.slices.push_back({jit.Regs()[15], jit.Cpsr(), env.ticks, Hash(reinterpret_cast<const u8*>(jit.Regs().data()), 15 * 4)});
        }
        const u32 e = rng();
        if (c.worker && e % 5 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(30));  // (lets the worker get ahead)
        }
        jit.HenUpgradeLinks(~u64(0), 1 + (e >> 8) % 64);
        if (grow_test && c.worker) {
            jit.HenResortTick(hen_jit_now_ns());
        }
        static const bool quiet = std::getenv("NOINV") != nullptr;
        if (quiet && &c != reference_config) {
        } else if (e % 50 == 0) {
            const u32 at = CODE_BASE + (rng() % static_cast<u32>(p.code.size())) * 4;
            jit.InvalidateCacheRange(at, 4 + rng() % 256);
        } else if (e % 3001 == 1) {
            jit.ClearCache();
        }
        if (slices > 40'000'000) {
            std::printf("  %s: no end after %llu slices\n", c.name, static_cast<unsigned long long>(slices));
            return false;
        }
    }
    if (env.bad) {
        std::printf("  %s: the program left its memory or raised an exception (pc %08x)\n", c.name, jit.Regs()[15]);
        return false;
    }
    out.regs = jit.Regs();
    out.cpsr = jit.Cpsr();
    out.ticks = env.ticks;
    out.data_hash = Hash(env.data.data(), env.data.size());
    out.code_hash = Hash(env.code.data(), env.code.size());
    out.svcs = env.svcs;
    return true;
}

#ifndef NO_MAIN
int main(int argc, char** argv) {
    const u32 programs = argc > 1 ? static_cast<u32>(std::atoi(argv[1])) : 40;
    const u32 first_seed = argc > 2 ? static_cast<u32>(std::atoi(argv[2])) : 1;
    const u32 worker_repeats = argc > 3 ? static_cast<u32>(std::atoi(argv[3])) : 3;
    // Slices must stop at the same places within a group. With direct links, a branch written
    // straight to a block that is invalidated later goes through the dispatcher from then on
    // (HenTombstone), where the time slice is tested; with slots or upstream's rewriting it is
    // linked again when the block is recompiled. So the two groups stop in different places.
    static const Config configs[] = {
        // name                               table direct compact worker alias counters deterministic
        {"table+direct (the reference)", 1, 1, 0, 0, 1, 0, true},
        {"compact, stubs after blocks", 1, 1, 1, 0, 0, 0, true},
        {"compact, stubs in runs", 1, 1, 1, 0, 1, 0, true},
        {"compact, exit counters", 1, 1, 1, 0, 1, 1, true},
        {"table, no direct links (second reference)", 1, 0, 0, 0, 1, 0, true},
        {"compact, no direct links", 1, 0, 1, 0, 1, 0, true},
        {"upstream links (no table)", 0, 0, 0, 0, 0, 0, true},
        {"table+direct, worker", 1, 1, 0, 3, 1, 0, false},
        {"compact, worker", 1, 1, 1, 3, 1, 0, false},
    };
    constexpr size_t second_reference = 4;
    reference_config = &configs[0];
    if (std::getenv("GROW")) {
        grow_test = true;
        hen_buffer_grow = hen_resort = 1;
    }
    if (const char* kib = std::getenv("CACHE_KIB")) {
        small_cache = static_cast<size_t>(std::atoi(kib)) << 10;
    }
    u32 failures = 0;
    u64 total_slices = 0, total_ticks = 0, thumb_slices = 0;
    unsigned long long compact_sum[6]{}, runs_per_config[16]{};
    for (u32 n = 0; n < programs; n++) {
        const u32 seed = first_seed + n;
        const u32 segments = n % 5 == 4 ? 4000 + seed % 3000 : 100 + (seed * 7919u) % 1500;
        const Program p = Generate(seed, segments);
        Result ref, ref2;
        if (!Run(p, seed, configs[0], ref, true)) {
            failures++;
            continue;
        }
        if (std::getenv("VERBOSE")) {
            std::printf("seed %u: %u segments, %zu words, loops %u: %zu slices, %llu ticks, %u svcs, r10 %d, pc %08x\n", seed, segments, p.code.size(), p.loops,
                        ref.slices.size(), static_cast<unsigned long long>(ref.ticks), ref.svcs, static_cast<int>(ref.regs[10]), ref.regs[15]);
        }
        for (const Slice& sl : ref.slices) {
            thumb_slices += (sl.cpsr >> 5) & 1;
        }
        total_slices += ref.slices.size();
        total_ticks += ref.ticks;
        for (size_t ci = 1; ci < std::size(configs); ci++) {
            const Config& c = configs[ci];
            const u32 repeats = c.deterministic ? 1 : worker_repeats;
            for (u32 r = 0; r < repeats; r++) {
                for (auto& v : shim_compact) {
                    v = 0;
                }
                Result got;
                if (!Run(p, seed, c, got, c.deterministic)) {
                    failures++;
                    continue;
                }
                if (ci == second_reference) {
                    ref2 = got;
                }
                const Result& ref_slices = ci >= second_reference ? ref2 : ref;
                runs_per_config[ci]++;
                if (c.compact) {
                    for (int k = 0; k < 6; k++) {
                        compact_sum[k] += shim_compact[k];
                    }
                }
                if (!got.SameEnd(ref)) {
                    failures++;
                    std::printf("seed %u (%u segments) %s: ends differently: pc %08x/%08x ticks %llu/%llu svcs %u/%u data %08x/%08x code %08x/%08x cpsr %08x/%08x\n",
                                seed, segments, c.name, got.regs[15], ref.regs[15], static_cast<unsigned long long>(got.ticks),
                                static_cast<unsigned long long>(ref.ticks), got.svcs, ref.svcs, got.data_hash, ref.data_hash, got.code_hash, ref.code_hash, got.cpsr, ref.cpsr);
                } else if (c.deterministic && small_cache == 0 && got.slices != ref_slices.slices) {
                    failures++;
                    size_t at = 0;
                    while (at < got.slices.size() && at < ref_slices.slices.size() && got.slices[at] == ref_slices.slices[at]) {
                        at++;
                    }
                    std::printf("seed %u (%u segments) %s: same end, but slice %zu of %zu/%zu stops elsewhere\n", seed, segments, c.name, at, got.slices.size(), ref_slices.slices.size());
                }
            }
        }
        if (n % 10 == 9 || n + 1 == programs) {
            std::printf("%u programs, %llu slices and %llu M ticks in the reference runs, %u failures\n", n + 1,
                        static_cast<unsigned long long>(total_slices), static_cast<unsigned long long>(total_ticks / 1000000), failures);
            std::fflush(stdout);
        }
    }
    std::printf("compact runs in all: %llu sites direct when written, %llu through a stub, %llu of those made direct since, %llu stubs, "
                "%llu upgrades left alone (block invalidated); %llu cache clears, %llu publishes with %.2f ranges each\nsmall cache %zu KiB; lookups answered \"interpret\": %llu\n",
                compact_sum[0], compact_sum[1], compact_sum[2], compact_sum[3], compact_sum[5], shim_clears.load(), shim_publishes.load(),
                shim_publishes ? static_cast<double>(shim_ranges) / shim_publishes : 0.0, small_cache >> 10, deferred_total);
    std::printf("blocks compiled by the worker's thread: %llu; reference slices that ended in Thumb code: %llu\n", shim_worker_blocks.load(), static_cast<unsigned long long>(thumb_slices));
    std::printf(failures ? "FAILED: %u\n" : "ok (%u failures)\n", failures);
    return failures != 0;
}
#endif
