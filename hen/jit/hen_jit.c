// Code memory for the JITs (dynarmic and the shader JIT, through oaknut's
// CodeBlock) on Switch 2 homebrew (kaiHEN).
//
// The HEN maps heap memory as code (__nx_jit_map_code, in the devkit's
// kaihen-jit.o: link it) and unmaps it again; mapped memory can't be
// written, and its source pages can't be touched. Measured:
//   - at most 16 mappings at a time for the process (the 17th: 0x416);
//   - a mapping can only be unmapped whole (a part: 0xd401);
//   - map ~0.02 ms + 0.055 ms/MiB, unmap ~0.02 ms + 0.016 ms/MiB.
// libounce's Jit API maps one buffer per Jit and moves all of it between
// writable and executable, flushing the caches for all of it: 18.6 ms per
// transition for dynarmic's 128 MiB buffer, twice per compiled block.
//
// Here each buffer has three areas:
//   - the write area: ordinary memory the JIT writes code into (wptr);
//   - source memory of the same size, allocated when the buffer first gets code;
//   - an execute address range (xptr), where pieces of the source are mapped.
// The JIT says what it changed (hen_jit_invalidate, which every JIT calls for
// cache flushing anyway). hen_jit_executable() then unmaps the pieces with
// changes, copies the changed bytes from the write area to the source, maps
// the pieces again and flushes the caches for those bytes. hen_jit_writable()
// costs nothing.
//
// The pieces. dynarmic's code only grows at its end now (block
// links go through a table: hen_link_table, address_space.cpp), so only the
// end is ever updated, and an update costs what unmapping and mapping its
// piece costs. The mapped part [0, end) is therefore cut like a binary number:
// the last 64 KiB unit is a piece of its own, and the units before it are
// pieces of 1, 2, 4... units, one per set bit of their count. Updating the
// end remaps 64 KiB (~0.05 ms; a 4 MiB piece takes 0.45 ms),
// pieces merge as the end moves on (each byte is remapped about once per
// doubling), and 128 MiB needs at most 12 mappings. A change in old code
// still works: its piece, whatever its size, is remapped.
//
// The 16 mappings are shared by all buffers, and Azahar creates a buffer per
// emulated core and page table (8 for a typical game) of which one or two do
// nearly all the work. So:
//   - a buffer takes no mapping before it has code, and its first change
//     (dynarmic's prelude, written when the JIT is created) is held back
//     until its second, the first compiled block: a JIT that never runs
//     never takes a mapping;
//   - when the mappings run short, a buffer doubles its unit, which merges
//     its pieces (down to one piece for the whole buffer).
//
// While a piece is being updated, no code in it can run: fine for JITs driven
// from one thread (the emulated CPUs), not for code another thread may be
// running at that moment.
#include <switch.h>

#include <malloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The devkit's kaihen-jit.o.
Result __nx_jit_map_code(void* dst, void* src, u64 size);
Result __nx_jit_unmap_code(void* dst, void* src, u64 size);

// A newer HEN can keep a writable view of mapped code and copy
// bytes into it on request (__nx_jit_map_alias and kaihenJitPublishRanges), and
// allows 512 mappings. With that, a buffer is mapped
// once, whole, and an update is one request carrying the changed bytes: nothing is
// unmapped, so the pieces above aren't needed. Used where it is offered
// (j->alias); otherwise the pieces.
Result __nx_jit_map_alias(void* dst, u64 size) __attribute__((weak));
typedef struct {
	u64 offset;
	u64 size;
} OuncehenJitRange;
// (kaihenJitPublishRanges in a newer devkit, ouncehenJitPublishRanges in an older one: the
// same call. Both weak; whichever the devkit has is used. Neither: the pieces.)
Result kaihenJitPublishRanges(Jit* code, const OuncehenJitRange* ranges, size_t count) __attribute__((weak));
Result ouncehenJitPublishRanges(Jit* code, const OuncehenJitRange* ranges, size_t count) __attribute__((weak));
static Result jit_publish_ranges(Jit* code, const OuncehenJitRange* ranges, size_t count) {
	return kaihenJitPublishRanges != NULL ? kaihenJitPublishRanges(code, ranges, count)
	                                      : ouncehenJitPublishRanges(code, ranges, count);
}
int hen_jit_alias = 1;  // --no-jit-alias: the pieces even where the alias is offered (for comparing)
// The publish request sent from here, from two buffers kept for the whole run, each
// on a page boundary and whole pages long, so no page of theirs holds anything else: see
// publish_ranges below. The library's call allocates and frees two heap blocks per request.
// 0: --no-publish-aligned, the library's call, to compare.
int hen_publish_aligned = 1;
// How many blocks the compile worker may put in one publish request while the
// program is not waiting for any of them (dynarmic's address_space.cpp, HenWorkerCompileBatch:
// the rule is there). 0: --no-publish-batch, eight at most, to compare.
int hen_publish_batch = 32;
// Publishes: bytes, ranges, ns in the request, and how many took <40 us, <100 us, <1 ms, longer.
static _Atomic unsigned long long s_pub[7];
// Since the start, like the above: requests sent from here, requests through the library, and
// of those the ones too large for the buffer here; the worker's batches, their blocks, the
// batches of more than eight blocks, and the batches the program's interpreting ended.
static _Atomic unsigned long long s_pubPath[3];
// The JIT's statistics from here down to hen_jit_tier_stats are counted for the development
// builds' reports. A release build keeps what the session report and the boost read: the
// count of compiled blocks (s_boostBlocks) and the time in the interpreter
// (s_boostDemandBlocks).
#if HEN_DEV
void hen_jit_publish_stats(unsigned long long* out) {
	for (int i = 0; i < 7; i++) {
		out[i] = atomic_load(&s_pub[i]);
	}
}
static _Atomic unsigned long long s_batch[4];
void hen_jit_note_batch(unsigned blocks, int cut) {
	atomic_fetch_add_explicit(&s_batch[0], 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_batch[1], blocks, memory_order_relaxed);
	if (blocks > 8) {
		atomic_fetch_add_explicit(&s_batch[2], 1, memory_order_relaxed);
	}
	if (cut) {
		atomic_fetch_add_explicit(&s_batch[3], 1, memory_order_relaxed);
	}
}
void hen_jit_publish_path_stats(unsigned long long* out) {
	for (int i = 0; i < 3; i++) {
		out[i] = atomic_load(&s_pubPath[i]);
	}
	for (int i = 0; i < 4; i++) {
		out[3 + i] = atomic_load(&s_batch[i]);
	}
}
#endif
static int s_aliasState;  // 0 not tried, 1 works, -1 refused

#define MAX_MAPPINGS 256  // (a newer HEN allows 512; an older one 16)
#define UNIT ((size_t) 64 << 10)
#define SMALL ((size_t) 4 << 20)  // a buffer below this is one piece (the shader JIT's)
#define PAGE ((size_t) 0x1000)
#define BLOCK_2M ((size_t) 2 << 20)
#define MAX_RANGES 512

struct piece {
	size_t begin, size;
};

struct hen_jit {
	size_t size;        // page aligned (a multiple of UNIT unless small)
	size_t usable;      // how much of it the JIT may fill: size, or less (see try_alias)
	size_t unit;        // UNIT, doubled when mappings run short; the whole size if small
	bool small;
	u8* write;          // the write area
	u8* source;         // what is mapped, once the buffer has code
	u8* sourceBlock;    // the heap block source lies in (what free() gets)
	u8* exec;           // the execute address range
	size_t align;       // of exec and source: PAGE, or BLOCK_2M (hen_code_align_2m)
	VirtmemReservation* reservation;
	struct piece mapped[MAX_MAPPINGS];
	unsigned mappedCount;
	bool alias;         // mapped whole, updated through the HEN's writable view
	Jit lib;            // (what kaihenJitPublishRanges reads: sizes and the three addresses)
	bool writing;       // between hen_jit_writable and hen_jit_executable
	unsigned sessions;  // completed write sessions
	struct {
		size_t begin, end;
	} dirty[MAX_RANGES];
	size_t dirtyCount;
	size_t highWater;   // end of the code (furthest byte changed since the last rewind)
	size_t peak;        // furthest byte ever changed
	unsigned id;
	bool writeFull;     // write is j->size long; else WRITE_FIRST (hen_jit_write_full)
};

// A buffer's write area is this long until its first block is compiled (dynarmic calls
// hen_jit_write_full at the start of AddressSpace::Emit). A JIT is made for every core a
// process is set to run on, and many never compile a block: 6 of the 18 buffers of a Mario
// Kart 7 session with the online modules on ("used up to 3 KiB"), which with a whole write
// area would hold 128 MiB each, 160 MiB as the allocator counts (below).
// What is written before the first block is the JIT's prelude, about 3 KiB.
#define WRITE_FIRST ((size_t) 64 << 10)
int hen_jit_lazy_write = 1;  // --no-jit-lazy-write: the whole write area at creation
int hen_code_cache_mib = 124; // --jit-buffer-mib (core/arm/dynarmic/arm_dynarmic.cpp, MakeJit)
// (Both off by default: Smash ran less smoothly with them.)
// --jit-buffer-grow: a buffer under 124 MiB that is seven tenths full is re-sorted into one 32 MiB
// larger (dynarmic address_space.cpp, HenResortTick). --jit-buffer-sized: a JIT's first buffer is
// sized from its process's code (arm_dynarmic.cpp, MakeJit: 12 times the segments + 8 MiB, in the
// steps 28, 60, 92, 124 MiB, which with their header fill whole 32 MiB units); turns growing on.
int hen_buffer_grow = 0;   // (--jit-buffer-sized turns both on,
int hen_buffer_sized = 0;  //  --jit-buffer-grow growth alone)
void sw_fatal_record(const char* text) __attribute__((weak));  // citra_switch/switch_platform.c
// (heapGetTotalSize, heapGetFreeSize: libounce, through switch.h)

#define MAX_LIVE 64
static struct hen_jit* s_live[MAX_LIVE];  // buffers created and not destroyed
static Mutex s_lock;
static unsigned s_mappings;  // of MAX_MAPPINGS
static _Atomic unsigned long long s_syncs, s_syncNs, s_maps, s_clears;
static _Atomic unsigned long long s_reach[5];  // updates reaching back <=64K, <=256K, <=1M, <=4M, more
#if HEN_DEV
// Since the last call.
void hen_jit_reach_stats(unsigned long long* out) {
	for (int i = 0; i < 5; i++) {
		out[i] = atomic_exchange(&s_reach[i], 0);
	}
}
#endif

// Compile statistics from the JIT (dynarmic's address_space.cpp): time making IR, time emitting
// code, blocks, and calls (one call compiles a block and those it links to).
#if HEN_DEV
static _Atomic unsigned long long s_irNs, s_emitNs, s_blocks, s_compiles;
#endif
// What went on during a gap between two frames shown (src/common/hen_gap.h; development builds).
// Running totals per kind, and the longest file call and service call since they were last read.
#if HEN_DEV
#define HEN_GAP_KINDS_C 17
static _Atomic unsigned long long s_gapNs[HEN_GAP_KINDS_C], s_gapLongestNs[2];
static char s_gapLongestWhat[2][64];
void hen_gap_add(int kind, unsigned long long ns) {
	if (kind >= 0 && kind < HEN_GAP_KINDS_C) {
		atomic_fetch_add_explicit(&s_gapNs[kind], ns, memory_order_relaxed);
	}
}
void hen_gap_longest(int which, unsigned long long ns, const char* what, unsigned detail) {
	if (which < 0 || which > 1 || ns <= atomic_load_explicit(&s_gapLongestNs[which], memory_order_relaxed)) {
		return;
	}
	// (two threads here at once can mix the two texts: a line of the log, nothing more)
	atomic_store_explicit(&s_gapLongestNs[which], ns, memory_order_relaxed);
	if (which == 1) {
		snprintf(s_gapLongestWhat[which], sizeof(s_gapLongestWhat[which]), "%.48s %08x", what, detail);
	} else {
		snprintf(s_gapLongestWhat[which], sizeof(s_gapLongestWhat[which]), "%.60s", what);
	}
}
void hen_gap_read(unsigned long long* totals, unsigned long long* longest_ns, char (*what)[64]) {
	for (int i = 0; i < HEN_GAP_KINDS_C; i++) {
		totals[i] = atomic_load_explicit(&s_gapNs[i], memory_order_relaxed);
	}
	for (int i = 0; i < 2; i++) {
		longest_ns[i] = atomic_exchange(&s_gapLongestNs[i], 0);
		memcpy(what[i], s_gapLongestWhat[i], 64);
		what[i][63] = 0;
	}
}
unsigned long long hen_gap_total(int kind) {
	return kind >= 0 && kind < HEN_GAP_KINDS_C ? atomic_load_explicit(&s_gapNs[kind], memory_order_relaxed) : 0;
}
#define GAP_ADD(kind, ns) hen_gap_add(kind, ns)
#else
#define GAP_ADD(kind, ns) ((void) 0)
#endif
unsigned long long hen_jit_now_ns(void) { return armTicksToNs(armGetSystemTick()); }
static _Atomic unsigned long long s_boostBlocks; // (never reset: blocks compiled, for the loading boost)
// Time in the interpreter (ns, never reset): the title is running code that is not compiled
// yet. Blocks compiled is the wrong measure of that: a warm Smash compiles its 634457 listed
// blocks ahead for the whole route, and counting those held the clock up for 95 of 100 s.
static _Atomic unsigned long long s_boostDemandBlocks;
// (the times are the development builds': the others pass 0)
void hen_jit_note_compile(unsigned long long irNs, unsigned long long emitNs, unsigned blocks) {
#if HEN_DEV
	atomic_fetch_add(&s_irNs, irNs);
	atomic_fetch_add(&s_emitNs, emitNs);
	GAP_ADD(0 /* HEN_GAP_JIT_COMPILE */, irNs + emitNs);
	atomic_fetch_add(&s_blocks, blocks);
	atomic_fetch_add(&s_compiles, 1);
#else
	(void) irNs;
	(void) emitNs;
#endif
	atomic_fetch_add_explicit(&s_boostBlocks, blocks, memory_order_relaxed);
}
// Translation against emission for the blocks compiled ahead (HenEmitAhead), timed inside it.
#if HEN_DEV
static _Atomic unsigned long long s_splitIrNs, s_splitEmitNs, s_splitBlocks;
#endif
void hen_jit_note_split(unsigned long long translateNs, unsigned long long emitNs) {
#if HEN_DEV
	atomic_fetch_add_explicit(&s_splitIrNs, translateNs, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_splitEmitNs, emitNs, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_splitBlocks, 1, memory_order_relaxed);
#else
	(void) translateNs;
	(void) emitNs;
#endif
	atomic_fetch_add_explicit(&s_boostBlocks, 1, memory_order_relaxed);
}
#if HEN_DEV
void hen_jit_split_stats(unsigned long long* out) {
	out[0] = atomic_exchange(&s_splitIrNs, 0);
	out[1] = atomic_exchange(&s_splitEmitNs, 0);
	out[2] = atomic_exchange(&s_splitBlocks, 0);
}
// The parts of emitting a block (dynarmic's AddressSpace::Emit): generating code, the block
// tables, linking, the frontier, making it executable, registering its range.
static _Atomic unsigned long long s_emitParts[6], s_emitCount;
void hen_jit_note_emit_parts(unsigned long long a, unsigned long long b, unsigned long long c, unsigned long long d,
                             unsigned long long e, unsigned long long f) {
	const unsigned long long v[6] = {a, b, c, d, e, f};
	for (int i = 0; i < 6; i++) {
		atomic_fetch_add_explicit(&s_emitParts[i], v[i], memory_order_relaxed);
	}
	atomic_fetch_add_explicit(&s_emitCount, 1, memory_order_relaxed);
}
void hen_jit_emit_parts_stats(unsigned long long* out) {
	for (int i = 0; i < 6; i++) {
		out[i] = atomic_exchange(&s_emitParts[i], 0);
	}
	out[6] = atomic_exchange(&s_emitCount, 0);
}
// Lookups that took the compile lock on the emulation thread, by kind (see address_space.cpp).
static _Atomic unsigned long long s_miss[4], s_missLockNs, s_missLockLong, s_missTotalNs;
void hen_jit_note_miss(int kind, unsigned long long lockNs, unsigned long long totalNs) {
	atomic_fetch_add(&s_miss[kind & 3], 1);
	atomic_fetch_add(&s_missLockNs, lockNs);
	if (lockNs > 1000000) {
		atomic_fetch_add(&s_missLockLong, 1);
	}
	atomic_fetch_add(&s_missTotalNs, totalNs);
}
// Every time a thread other than the worker took the compile lock (misses, invalidations,
// list changes): count, ns waited, longest.
static _Atomic unsigned long long s_locks, s_lockNs, s_lockLongest;
void hen_jit_note_lock(unsigned long long ns) {
	atomic_fetch_add_explicit(&s_locks, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_lockNs, ns, memory_order_relaxed);
	GAP_ADD(1 /* HEN_GAP_JIT_LOCK */, ns);
	if (ns > atomic_load_explicit(&s_lockLongest, memory_order_relaxed)) {
		atomic_store_explicit(&s_lockLongest, ns, memory_order_relaxed);
	}
}
void hen_jit_lock_stats(unsigned long long* out) {
	out[0] = atomic_exchange(&s_locks, 0);
	out[1] = atomic_exchange(&s_lockNs, 0);
	out[2] = atomic_exchange(&s_lockLongest, 0);
}
// Since the last call: the four kinds, ns waiting for the lock, waits over 1 ms, ns in misses.
void hen_jit_miss_stats(unsigned long long* out) {
	for (int i = 0; i < 4; i++) {
		out[i] = atomic_exchange(&s_miss[i], 0);
	}
	out[4] = atomic_exchange(&s_missLockNs, 0);
	out[5] = atomic_exchange(&s_missLockLong, 0);
	out[6] = atomic_exchange(&s_missTotalNs, 0);
}
#endif
// Since the last call on this thread: wall time, and the time the thread had a core.
unsigned hen_jit_thread_times(double* wall_ms, double* cpu_ms) {
	static u64 lastCpu, lastWall;
	u64 cpu = 0;
	Result rc = svcGetInfo(&cpu, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX);
	const u64 wall = armGetSystemTick();
	*wall_ms = armTicksToNs(wall - lastWall) / 1e6;
	*cpu_ms = R_SUCCEEDED(rc) ? armTicksToNs(cpu - lastCpu) / 1e6 : 0;
	lastCpu = cpu;
	lastWall = wall;
	return rc;
}
#if HEN_DEV
// Since the last call: IR ns, emit ns, blocks, calls.
void hen_jit_compile_stats(unsigned long long* out) {
	out[0] = atomic_exchange(&s_irNs, 0);
	out[1] = atomic_exchange(&s_emitNs, 0);
	out[2] = atomic_exchange(&s_blocks, 0);
	out[3] = atomic_exchange(&s_compiles, 0);
}
#endif

// The dispatcher's block lookups that left generated code (dynarmic's a32_address_space.cpp), and
// the time spent running generated code (Azahar's arm_dynarmic.cpp).
int hen_fast_dispatch = 1;
// Block links through a table instead of rewritten instructions (dynarmic's address_space.cpp).
int hen_link_table = 1; // --no-link-table
int hen_direct_links = 1; // --no-direct-links (dynarmic address_space.cpp, HenWriteLinkSites)
// Link sites of one instruction, with the slot form and the unlinked exit in a stub per target
// away from the blocks (dynarmic's emit_arm64_a32.cpp and address_space.cpp, HenLinkStub).
int hen_compact_links = 1; // --no-compact-links
// 0 compact sites written as a branch to the target, 1 as a branch to the target's stub,
// 2 of those made direct since, 3 stubs written (40 bytes each), 4 bytes of all blocks
// compiled (without the stubs), 5 sites left alone because their block had been invalidated.
#if HEN_DEV
static _Atomic unsigned long long s_compact[6];
void hen_jit_note_compact(int kind, unsigned long long count) { atomic_fetch_add_explicit(&s_compact[kind], count, memory_order_relaxed); }
void hen_jit_compact_stats(unsigned long long* out) {
	for (int i = 0; i < 6; i++) {
		out[i] = atomic_load(&s_compact[i]);
	}
}
static _Atomic unsigned long long s_linkKinds[3];
void hen_jit_note_link_upgrades(unsigned long long count) { atomic_fetch_add_explicit(&s_linkKinds[2], count, memory_order_relaxed); }
void hen_jit_note_link(int kind) { atomic_fetch_add_explicit(&s_linkKinds[kind], 1, memory_order_relaxed); }
void hen_jit_link_stats(unsigned long long* out) {
	out[0] = atomic_load(&s_linkKinds[0]);
	out[1] = atomic_load(&s_linkKinds[1]);
	out[2] = atomic_load(&s_linkKinds[2]);
}
// The emulation thread's link upgrades (dynarmic address_space.cpp, HenUpgradeLinks) never wait
// for the compile mutex: a frame that finds it taken goes without its upgrades. (A wait there
// can be a 59 ms gap between two frames, the worker on a core all through it.)
// The most frames in a row that went without (since the last read), and such frames in all.
static _Atomic unsigned s_linkSkipRun, s_linkSkipRunMax;
static _Atomic unsigned long long s_linkSkips;
void hen_jit_note_link_skip(int skipped) {
	if (!skipped) {
		atomic_store_explicit(&s_linkSkipRun, 0, memory_order_relaxed);
		return;
	}
	const unsigned run = atomic_fetch_add_explicit(&s_linkSkipRun, 1, memory_order_relaxed) + 1;
	atomic_fetch_add_explicit(&s_linkSkips, 1, memory_order_relaxed);
	if (run > atomic_load_explicit(&s_linkSkipRunMax, memory_order_relaxed)) {
		atomic_store_explicit(&s_linkSkipRunMax, run, memory_order_relaxed);
	}
}
// out[0]: frames without upgrades in all; out[1]: the longest run of them since the last call.
void hen_jit_link_skip_stats(unsigned long long* out) {
	out[0] = atomic_load(&s_linkSkips);
	out[1] = atomic_exchange(&s_linkSkipRunMax, 0);
}
#endif
// 3: a second thread compiles ahead of the program (dynarmic's address_space.cpp). It needs code
// that stays mapped while it is updated (try_alias); where that is not offered, the
// setting falls back to 0. 0: no worker (--translation-worker 0). No other value is offered.
int hen_translation_worker = 3;
#if HEN_DEV
static _Atomic unsigned long long s_translated, s_translationsUsed, s_translationsStale;
void hen_jit_note_translation(unsigned made, unsigned used, unsigned stale) {
	atomic_fetch_add_explicit(&s_translated, made, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_translationsUsed, used, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_translationsStale, stale, memory_order_relaxed);
}
#endif
// The compile worker (--translation-worker 3; dynarmic's address_space.cpp, "Compiling on a
// second thread"): how many branches past a block the program asked for it compiles, and how
// many blocks past the wanted one the emulation thread still compiles itself at a miss.
int hen_worker_depth_limit = 4;  // --jit-worker-depth
int hen_emu_ahead = 4;           // --jit-ahead
// Interpreter first (dynarmic's AddressSpace::HenGetOrDefer, Azahar's ARM_Dynarmic::Run): how
// many times a block that isn't compiled yet is interpreted before the emulation thread
// compiles it itself. 0: never interpret.
// The higher the better as far as measured: along the recorded routes, cold, Kid
// Icarus gave up 1202 ms of lag in its first 25 s at 16, 970 at 32, 816 at 64, 771 at 128 and
// 713 to 728 at 250; Pokemon Omega Ruby 348 at 64 and 309 at 250 in its first 30 s. The later
// the emulation thread gives up on the worker and compiles a block itself, the fewer it compiles
// (5088 at 16, 727 at 250), and that outweighs the extra interpreting.
int hen_tier = 1000;             // --jit-tier
int hen_tier_force = 0;
// A block still interpreted after this many visits goes back to the top of the compile worker's
// list (dynarmic HenGetOrDefer). 4, 16 and 64 measure alike, all a little
// better than off: Kid Icarus cold, first 25 s, lag 566 -> 527 ms; Smash, first 55 s, lag 469 ->
// 445 ms and 1855 -> 1629 ms in gaps between frames; Pokemon OR unchanged. --jit-bump 0: off.
int hen_bump = 16;
// kind 0: a block left to the interpreter; 1: compiled on the emulation thread after hen_tier
// visits; 2: compiled at once because the interpreter refused it.
#if HEN_DEV
static _Atomic unsigned long long s_tier[3], s_tierInstrs, s_tierNs;
void hen_jit_note_tier(int kind) { atomic_fetch_add_explicit(&s_tier[kind], 1, memory_order_relaxed); }
#endif
// The compile worker publishes a block the program asked for without filling the batch up,
// and is woken only when asleep (dynarmic's address_space.cpp; --no-jit-publish-early).
int hen_publish_early = 1;
#if HEN_DEV
static _Atomic unsigned long long s_earlyPublish, s_wakeSkipped;
void hen_jit_note_early_publish(void) { atomic_fetch_add_explicit(&s_earlyPublish, 1, memory_order_relaxed); }
void hen_jit_note_wake_skipped(void) { atomic_fetch_add_explicit(&s_wakeSkipped, 1, memory_order_relaxed); }
void hen_jit_early_stats(unsigned long long* out) {
	out[0] = atomic_exchange(&s_earlyPublish, 0);
	out[1] = atomic_exchange(&s_wakeSkipped, 0);
}
#endif
void hen_jit_note_interpreted(unsigned instrs, unsigned long long ns) {
#if HEN_DEV
	atomic_fetch_add_explicit(&s_tierInstrs, instrs, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_tierNs, ns, memory_order_relaxed);
	GAP_ADD(2 /* HEN_GAP_INTERPRETER */, ns);
#else
	(void) instrs;
#endif
	atomic_fetch_add_explicit(&s_boostDemandBlocks, ns, memory_order_relaxed);
}
// In all, ns in the interpreter (core_timing.cpp: the automatic New 3DS clock stays down while
// code is new).
unsigned long long hen_jit_interpreted_ns(void) {
	return atomic_load_explicit(&s_boostDemandBlocks, memory_order_relaxed);
}
#if HEN_DEV
// Since the last call: the three kinds, instructions interpreted, ns in the interpreter.
void hen_jit_tier_stats(unsigned long long* out) {
	for (int i = 0; i < 3; i++) {
		out[i] = atomic_exchange(&s_tier[i], 0);
	}
	out[3] = atomic_exchange(&s_tierInstrs, 0);
	out[4] = atomic_exchange(&s_tierNs, 0);
}
#endif
int hen_jit_alias_active(void);
// The thread that creates a worker (the emulation thread), just before: which core it is on.
static int s_emuCore = -1;
static Handle s_emuHandle;
Handle hen_worker_handle; // (the first compile worker, for the black box below)
void hen_jit_worker_creating(void) {
	s_emuCore = (int) svcGetCurrentProcessorNumber();
	s_emuHandle = threadGetCurHandle();
}
// For the line about a gap between two frames: the time the emulation thread and the compile
// worker have had a core, in all, ns (0 if not readable).
#if HEN_DEV
void hen_gap_thread_cpu(unsigned long long* out) {
	u64 ticks = 0;
	out[0] = s_emuHandle != 0 && R_SUCCEEDED(svcGetInfo(&ticks, InfoType_ThreadTickCount, s_emuHandle, UINT64_MAX))
	             ? armTicksToNs(ticks) : 0;
	out[1] = hen_worker_handle != 0 && R_SUCCEEDED(svcGetInfo(&ticks, InfoType_ThreadTickCount, hen_worker_handle, UINT64_MAX))
	             ? armTicksToNs(ticks) : 0;
}
#endif
// The worker's thread, at its start: on another core than the emulation thread (threads of
// one priority on one core don't take turns here), and below it in priority if allowed.
void hen_thread_name(const char* name); // hen_thread.c
void hen_jit_worker_started(void) {
	if (hen_worker_handle == 0) {
		hen_worker_handle = threadGetCurHandle();
	}
	hen_thread_name("jit-worker");
	{
		s32 preferred = -1;
		u64 mask = 0;
		const Result got = svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE);
		const int here = (int) svcGetCurrentProcessorNumber();
		int chosen = -1;
		Result rc = 0;
		if (R_SUCCEEDED(got) && here == s_emuCore) {
			for (int core = 5; core >= 0 && chosen < 0; core--) {
				if (core != s_emuCore && (mask >> core & 1)) {
					rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, mask);
					if (R_SUCCEEDED(rc)) {
						chosen = core;
					}
				}
			}
		}
		static bool told;
		if (!told) {
			told = true;
			printf("hen_jit: worker on core %d (emulation thread on %d), allowed 0x%llx (rc 0x%x), moved to %d (rc 0x%x)\n", here,
			       s_emuCore, (unsigned long long) mask, got, chosen, rc);
		}
	}
	s32 priority = 0;
	if (R_SUCCEEDED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)) && priority < 62) {
		const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, priority + 1);
		static bool told;
		if (!told) {
			told = true;
			printf("hen_jit: translation worker priority %d -> %d (rc 0x%x)\n", priority, priority + 1, rc);
		}
	}
}
// The thread that destroys the JITs of ended processes (Azahar's
// core/arm/dynarmic/arm_dynarmic.cpp, HenTeardownMain). 0: --no-jit-teardown-thread, they are
// destroyed on the emulation thread, to compare.
int hen_jit_teardown_thread = 1;
// Starting the same title again within a run.
// hen_launch_cache: what a launch read is kept for the run (the title's content path in
// core/hle/service/am/am.cpp, the code image and the icon, logo and banner in
// core/loader/ncch.cpp, the optional files that were not there in common/file_util.cpp).
// 0: --no-launch-cache, every launch reads everything again.
int hen_launch_cache = 1;
// hen_jit_park: the JITs of an ended process started that way are kept, with the process's
// page table, and handed to the next process that is the same program at the same addresses
// (core/arm/dynarmic/arm_dynarmic.cpp, "Parked JITs"). 0: --no-jit-park, they are destroyed
// when the process ends.
int hen_jit_park = 1;
static int s_teardownCreatorCore = -1;
// The thread that starts it (the emulation thread), just before: which core it is on.
void hen_jit_teardown_creating(void) { s_teardownCreatorCore = (int) svcGetCurrentProcessorNumber(); }
// The teardown thread, at its start. Not on the emulation thread's core (threads of one
// priority on one core don't take turns here), and two steps below it in priority where
// allowed: one below the compile workers, so that a worker with blocks to compile takes the
// core from it at once and only the teardown waits.
void hen_jit_teardown_started(void) {
	s32 preferred = -1;
	u64 mask = 0;
	const Result got = svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE);
	const int here = (int) svcGetCurrentProcessorNumber();
	int chosen = -1;
	Result rc = 0;
	if (R_SUCCEEDED(got) && here == s_teardownCreatorCore) {
		for (int core = 5; core >= 0 && chosen < 0; core--) {
			if (core != s_teardownCreatorCore && (mask >> core & 1)) {
				rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, mask);
				if (R_SUCCEEDED(rc)) {
					chosen = core;
				}
			}
		}
	}
	s32 priority = 0, lowered = 0;
	Result prc = svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	if (R_SUCCEEDED(prc)) {
		// (62 at most, as for the workers)
		lowered = priority + 2 <= 62 ? priority + 2 : priority < 62 ? 62 : priority;
		if (lowered != priority) {
			prc = svcSetThreadPriority(CUR_THREAD_HANDLE, lowered);
		}
	}
#if HEN_DEV
	printf("hen_jit: teardown thread on core %d (started from core %d), allowed 0x%llx (rc 0x%x), moved to %d (rc 0x%x); priority %d -> %d (rc 0x%x)\n",
	       here, s_teardownCreatorCore, (unsigned long long) mask, got, chosen, rc, priority, lowered, prc);
#endif
}
#if HEN_DEV
// Since the last call: made by the worker, used, found stale.
void hen_jit_translation_stats(unsigned long long* out) {
	out[0] = atomic_exchange(&s_translated, 0);
	out[1] = atomic_exchange(&s_translationsUsed, 0);
	out[2] = atomic_exchange(&s_translationsStale, 0);
}
#endif
// Homebrew (.3dsx) has no title ID (it reads as 0): the frontend sets this, from the file's
// name and size, as the name of its shader caches and block list. Never a real title ID.
unsigned long long hen_homebrew_cache_id = 0;
// For hen_thread.c at exit: a thread still running (already paused there). Its start routine
// and call chain, as offsets for addr2line.
extern char _start[];
void hen_thread_stuck_report(Handle thread, void* start_routine) {
	ThreadContext ctx;
	if (R_FAILED(svcGetThreadContext3(&ctx, thread))) {
		printf("hen: exit: a thread still running (start +%lx): no context\n", (unsigned long) ((uintptr_t) start_routine - (uintptr_t) _start));
		return;
	}
	char line[500];
	int at = snprintf(line, sizeof(line), "hen: exit: a thread still running (start +%lx) was at +%lx/%lx", (unsigned long) ((uintptr_t) start_routine - (uintptr_t) _start),
	                  (unsigned long) (ctx.pc.x - (uintptr_t) _start), (unsigned long) (ctx.lr - (uintptr_t) _start));
	uintptr_t fp = ctx.fp;
	for (int i = 0; i < 14 && (size_t) at < sizeof(line) - 20; i++) {
		MemoryInfo info;
		u32 page_info;
		if (fp == 0 || (fp & 15) != 0 || R_FAILED(svcQueryMemory(&info, &page_info, fp)) || !(info.perm & Perm_R) || fp + 16 > info.addr + info.size) {
			break;
		}
		at += snprintf(line + at, sizeof(line) - at, "/%lx", (unsigned long) (((const uintptr_t*) fp)[1] - (uintptr_t) _start));
		const uintptr_t next = ((const uintptr_t*) fp)[0];
		if (next <= fp) {
			break;
		}
		fp = next;
	}
	puts(line);
}

// The calling thread's call chain, as offsets for addr2line (from the frame records).
extern char _start[];
void hen_backtrace(const char* what) {
	char line[400];
	int at = snprintf(line, sizeof(line), "hen: backtrace: %s:", what);
	const uintptr_t* fp = (const uintptr_t*) __builtin_frame_address(0);
	for (int i = 0; i < 18 && fp != NULL && ((uintptr_t) fp & 15) == 0 && (size_t) at < sizeof(line) - 20; i++) {
		const uintptr_t ret = fp[1];
		const uintptr_t* next = (const uintptr_t*) fp[0];
		if (ret - (uintptr_t) _start >= ((uintptr_t) 256 << 20)) {
			at += snprintf(line + at, sizeof(line) - at, " ?");
		} else {
			at += snprintf(line + at, sizeof(line) - at, " %lx", (unsigned long) (ret - (uintptr_t) _start));
		}
		if (next <= fp || (uintptr_t) next - (uintptr_t) fp > (8u << 20)) {
			break;
		}
		fp = next;
	}
	puts(line);
}
unsigned long long hen_core_ns[4], hen_core_ticks[4], hen_core_runs[4];  // core/core.cpp, ExecuteCore; reported and reset by main.cpp
#if HEN_DEV
int hen_dump_spirv = 0;  // --dump-spirv (vk_shader_util.cpp)
#endif
// ---- Loading boost ---------------------------------------------------------------------
// The system's CPU boost mode (appletSetCpuBoostMode, what games ask for on their loading
// screens: about 1.1 -> 1.7 GHz here, measured with the .3dsx benchmark, with the GPU
// throttled) while the program is loading or warming up, and never in steady play:
//   - few frames presented while the emulation thread hardly sleeps (a loading screen), or
//   - a lot of new program code being compiled, or
//   - a graphics pipeline that took real time to build just now.
// It speeds up every host thread, so the compile worker and the driver's shader compiles
// gain as well. Off two seconds after none of these holds, in the title list and at exit
// (the system also drops it when the application closes). hen_boost_tick runs once a
// vertical blank on the emulation thread. --no-load-boost.
// ON (--no-load-boost). Measured
// cold, hands-off, 60 s, off against on. Time given up by the limiter: Pokemon OR 638 -> 303
// ms, Ocarina 114 -> 61, Kid Icarus 1918 -> 1139 (first interval 67% -> 79%), Mario Kart 7
// 609 -> 331; audio held 623 -> 614, 0 -> 0, 562 -> 290, 619 -> 452 ms; draws left out the
// same.
// Fragment shader configurations made canonical before hashing (pica_fs_config.cpp,
// FSConfig::Canonicalize): --no-fs-canon: 0. Counts: configurations as the registers gave
// them, and fragment shaders actually built.
int hen_fs_canon = 1;
// The generic fragment shader (glsl_fs_shader_gen.cpp, GenerateGenericFragmentShader): a draw
// shaded on the CPU whose stand-in pipeline is not ready is drawn with it instead of being
// left out. 0: --no-generic-fs. 2: --generic-all, every such draw (for comparing pictures).
int hen_generic_fs = 1;
#if HEN_DEV
unsigned long long hen_generic_stats[3];
#endif
unsigned long long hen_generic_unsupported;
int hen_stale_profile = 0; // --stale-profile (vk_pipeline_cache.cpp)
// A shader cache file made by another build of the shader generator or for another profile is
// regenerated by the shader workers while the title runs, not before it starts
// (vk_shader_disk_cache.cpp, HenRegen). 1: --cache-regen-sync, before it starts as upstream, to
// compare.
int hen_cache_regen_sync = 0;
int hen_generic_seed = 1; // (--no-generic-seed: its pipeline is started by the first draw that needs it)
// First-time graphics work off the GPU thread. Each has its own switch so that one run can tell
// them apart.
// --no-sw-standin: a software draw that is not a stand-in for a pending pipeline (six vertices or
// fewer, or never offered to the hardware path) has its pipeline compiled on the GPU thread
// (vk_rasterizer.cpp, Draw).
// Off that thread only while the GPU has time to spare (hen_gpu_headroom) or the draw covers little.
int hen_sw_standin = 1;
// drawn with the generic shader meanwhile; waited (generic not possible); waited (no GPU time
// to spare); drawn with it because the draw covers little
#if HEN_DEV
unsigned long long hen_sw_standin_stats[4];
#endif
// --sw-standin-area PERCENT: without GPU time to spare, a draw whose bounding box covers up to
// this share of the viewport still takes the generic shader (a quarter of a target a frame in
// all). 0: no such exception. A sixteenth: sixteen such draws cost one whole-screen pass.
int hen_sw_standin_area = 6;
// The present wait's running mean (vk_present_window.cpp, GetRenderFrame) and its verdict.
unsigned long long hen_present_wait_mean_ns;
int hen_gpu_headroom = 1;
#if HEN_DEV
unsigned long long hen_gpu_headroom_flips, hen_gpu_headroom_frames_without;
#endif
// --no-etc1-workers: ETC1 textures are decoded by the GPU thread alone (texture_codec.h).
int hen_etc1_workers = 1;
// --no-vma-small-blocks: image memory in the allocator's default blocks (256 MiB, begun at 32),
// which the driver chunks, instead of 8 MiB ones (vk_instance.cpp, CreateAllocator).
int hen_vma_small_blocks = 1;
#if HEN_DEV
unsigned long long hen_image_memory_stats[8];
// Since the last report (src/common/hen_frame_work.h names the slots): surfaces and images the
// renderer made, by who asked and by size; and the commands it recorded. Plain counters.
unsigned long long hen_surface_stats[32];
unsigned long long hen_frame_work[32];
// The same two as running totals that no report empties: the gap line's "work" takes differences.
unsigned long long hen_surface_stats_total[32];
unsigned long long hen_frame_work_total[32];
#endif
// --recycle-surfaces turns it on, --no-recycle-surfaces off. OFF by default: with it Xenoblade, large
// top at 4x, has 52 gaps over 40 ms along its route against 17 without, about 40 frames slipping
// one vertical blank ("waited for the GPU thread 7 ms"). The pool: a surface the title has
// overwritten is kept, with its images, in a pool for some frames, and handed back when a
// surface of the same size, format and factor is asked for, in place of destroying both images
// at the next frame and making them again (rasterizer_cache.h, HenPoolCollect and HenPoolTake).
// --recycle-cap-mib N: the most image memory the pool holds. --recycle-age N: frames a surface
// stays in it without being asked for.
int hen_recycle_surfaces = 0;
unsigned hen_recycle_cap_mib = 64;
unsigned hen_recycle_age_frames = 30;
// --compile-workers N: pipeline and shader workers each (4; vk_pipeline_cache.cpp). 1 with
// --nir-timing gives one compile at a time, so that a report is one shader's.
int hen_compile_workers = 4;
// --vs-jit-async (OFF: the interpreter it uses meanwhile is not bit-identical to the JIT): a PICA
// vertex program is compiled for the CPU by a shader worker (shader/shader_jit.cpp).
// --vs-jit-small N: draws of up to N vertices are interpreted meanwhile.
int hen_vs_jit_async = 0;
int hen_vs_jit_small = 512;
// jobs queued, draws interpreted, their vertices, draws that waited, compiles on the GPU thread, ns
#if HEN_DEV
unsigned long long hen_vs_jit_stats[6];
#endif
// Pipelines compiled by the driver on the renderer's own thread: count, ns, longest ns
// (vk_graphics_pipeline.cpp; emptied by the frontend's report).
unsigned long long hen_here_builds[3];
// ETC1 decodes split over the workers: count, tiles (texture_codec.h).
unsigned long long hen_etc1_split[2];
// The generic fragment shader's part of a pipeline, kept for reuse (vk_graphics_pipeline.cpp).
#if HEN_DEV
unsigned long long hen_part_cache_hits;
#endif
#if HEN_DEV
unsigned long long hen_fs_configs_raw;
#endif
unsigned long long hen_fs_configs_built;
int hen_load_boost = 1;
static _Atomic unsigned long long s_boostFrames, s_boostWorkNs, s_boostWorkSum;
static _Atomic unsigned long long s_resortBusyLast; // (see hen_resort_busy_last)
static struct { u64 ns; unsigned long long frames, blocks, demand, work, present_ns, present_frames; long long slept; } s_boostRing[128];
// Showing a frame (renderer_vulkan.cpp): how long it waited for the display side, and how many.
static _Atomic unsigned long long s_boostPresentNs, s_boostPresentFrames;
void hen_boost_note_present(unsigned long long ns) {
	atomic_fetch_add_explicit(&s_boostPresentNs, ns, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_boostPresentFrames, 1, memory_order_relaxed);
}
static unsigned s_boostAt;
static bool s_boostOn;
static _Atomic u64 s_boostLastWanted;
static u64 s_boostOnSince;
static u64 s_boostRaisedAt; // (when it last went on: s_boostOnSince starts again at every report)
static unsigned long long s_boostOnNs, s_boostSwitches, s_boostWhy[5];
void hen_boost_note_frame(void) { atomic_fetch_add_explicit(&s_boostFrames, 1, memory_order_relaxed); }
// A pipeline build that took real time (ns of it), from whichever thread built it.
void hen_boost_note_work(unsigned long long ns) {
	atomic_store_explicit(&s_boostWorkNs, armTicksToNs(armGetSystemTick()), memory_order_relaxed);
	atomic_fetch_add_explicit(&s_boostWorkSum, ns, memory_order_relaxed);
}
// Docked: the CPU's clock set to the rate the system's own boost mode uses, through the clock
// controller (switch_platform.c, sw_cpu_clock_quiet), with the GPU left as it is. Measured
// docked: normal CPU 998.4 MHz and GPU 1007.25 MHz; the system's boost mode CPU
// 1523.2 MHz and GPU 306 MHz; the service offers the CPU up to 1702.4 MHz. Five titles, cold,
// first 55 s, clock held against the system's boost in bursts: lag given up 1451 -> 617 ms,
// audio held 1457 -> 861 ms, time in gaps between frames 6346 -> 4072 ms.
// Docked, if the service refuses: the system's boost mode.
// hen_load_boost: 0 off, 1 this (default).
#define HEN_BOOST_CPU_HZ 1523200000u
int sw_cpu_clock_quiet(unsigned hz); // 1 if set
void sw_cpu_clock_end(void);
void sw_cpu_clock_forget(void);
void sw_cpu_clock_keep(void);
static bool s_boostByClock, s_boostDocked;
static unsigned long long s_boostSwitchNsMax;
int hen_boost_linger_ms = 0;    // --boost-linger MS: how long after the last reason it stays on
                                // (0: 10 s)
int hen_n3ds_mode = 0;          // core_timing.cpp: the title runs in the New 3DS fast CPU mode
// An applet or a system module started by the running title leaves the fast mode
// as the application set it (Azahar's core/hle/service/apt/ns.cpp, LaunchTitle: the reasons are
// there). Otherwise the first keyboard a New 3DS title opens turns the mode off for the rest
// of the run, and with it the clock this file holds for such a title (hen_n3ds_mode above).
// 0: --no-n3ds-mode-keep, every title started sets the mode from its own header, to compare.
int hen_n3ds_mode_keep = 1;
// Handheld: as docked. The boost is the CPU's clock raised
// through the clock controller, for the same reasons and for as long as docked (hen_boost_tick:
// loading, new code, pipeline builds, a busy emulation thread; ten seconds after the last
// reason it goes back to the rate found), and held for a title in the New 3DS fast mode. The
// system's boost mode is never asked for (so never cancelled either): the GPU keeps its rate.
// If the clock is refused there is no boost at all for the rest of the run: nothing falls back
// to the system's mode.
// A rate set through the clock controller lasts until the system applies its own configuration
// again, so once a second in handheld the rate is read and set again if it is no longer ours
// (sw_cpu_clock_keep, switch_platform.c).
// With the clock raised the same load sleeps more, so "busy" (under a tenth of the time asleep
// at the normal rate) is a higher line while raised: 1 - 0.9 * normal / raised. Docked, 998.4
// to 1523.2 MHz: 41%. Handheld, 1100.8 to 1523.2 MHz: 35%.
#define BOOST_BUSY_RAISED() (s_boostDocked ? 0.41 : 0.35)
// --room-plain-io, a test switch for multiplayer rooms (src/citra_switch/switch_room_io.c): the
// room's datagrams go through sendto and recvfrom, one buffer each, instead of the sendmsg and
// recvmsg the network library calls. Unmeasured; for a room that connects but carries nothing.
int hen_room_plain_io = 0;
// What the boost's calls returned, for the session report (hen_boost_calls).
static struct {
	unsigned clock_on, clock_refused, system_on, system_off;
	unsigned rc_on, rc_off;     // the last results of the system's calls
	unsigned failed_rc;         // the result that switched the boost off for good
	unsigned failed;            // 0 no, 1 turning the system's mode on, 2 ending it
} s_boostCalls;
static unsigned s_boostWhyLast; // (the reason last counted: for the line of a handheld rate change)
unsigned sw_cpu_clock_now(void);
// --n3ds-clock-high (off by default): a second rate for a title in the
// New 3DS fast mode. Such a title holds HEN_BOOST_CPU_HZ; while it is short of CPU the
// clock goes to HEN_BOOST_HIGH_HZ, and back after the boost's own linger without that signal.
// "Short of CPU" is the boost's "busy" measure taken at the rate the clock is at: the frame
// limiter slept under a tenth of the last second at HEN_BOOST_CPU_HZ; at the high rate the same
// load sleeps more, so the line there is 1 - 0.9 * rate / high rate (19.5%), the formula of
// BOOST_BUSY_RAISED. (The automatic emulated clock, core_timing.cpp HenClockControl, lowers
// its factor when a window's work is over 95% of its time and raises it under 90%: a title it
// is holding back sleeps about a tenth or less, one at its ceiling with time to spare more.)
// Only where the boost is the CPU's clock (docked, and handheld by default: one path), only for
// a title in the fast mode. The rate goes through the same check as the boost's (inside the
// range the clock controller offers, read each time: sw_cpu_clock_step); if it is not offered,
// the request fails or the rate does not read back, the title stays at HEN_BOOST_CPU_HZ and the
// high rate is not asked for again in this run.
#define HEN_BOOST_HIGH_HZ 1702400000u
int hen_n3ds_clock_high = 0;
int sw_cpu_clock_step(unsigned hz, unsigned* out);
static bool s_boostHigh, s_boostHighRefused;
static u64 s_boostHighWanted; // (when the title was last seen short of CPU)
// (for the report) vertical blanks at the high rate, switches up and down, blanks short of CPU
static unsigned long long s_boostHighTicks, s_boostHighSwitches, s_boostHighShort;
static void boost_tier(bool high, u64 now) {
	if (high == s_boostHigh) {
		return;
	}
	static bool told;
	unsigned r[3] = {0, 0, 0};
	const u64 t0 = armGetSystemTick();
	const int ok = sw_cpu_clock_step(high ? HEN_BOOST_HIGH_HZ : HEN_BOOST_CPU_HZ, r);
	const unsigned long long took = armTicksToNs(armGetSystemTick() - t0);
	if (took > s_boostSwitchNsMax) {
		s_boostSwitchNsMax = took;
	}
	if (high && !ok) {
		// Not offered, refused, or not what reads back: the boost's rate again (asked for even
		// if the request failed: what the clock is at now is not known), and no more tries.
		s_boostHighRefused = true;
		unsigned back[3] = {0, 0, 0};
		const int back_ok = sw_cpu_clock_step(HEN_BOOST_CPU_HZ, back);
		printf("hen: boost: high tier: %u Hz %s (rc 0x%x, reads back %u): the title stays at %u Hz "
		       "(set again: %s, rc 0x%x, reads back %u); not asked for again in this run\n",
		       HEN_BOOST_HIGH_HZ,
		       r[0] == 1 ? "is outside what the clock controller offers" : r[0] == 2 ? "was not asked for" : "was refused",
		       r[1], r[2], HEN_BOOST_CPU_HZ, back_ok ? "yes" : "no", back[1], back[2]);
		return;
	}
#if HEN_DEV
	const bool say = true; // (every switch: at most one pair a linger)
#else
	const bool say = !told || !ok;
#endif
	if (say) {
		told = true;
		if (high) {
			printf("hen: boost: high tier: %u -> %u Hz asked (the title is short of CPU), rc 0x%x; reads back %u\n",
			       HEN_BOOST_CPU_HZ, HEN_BOOST_HIGH_HZ, r[1], r[2]);
		} else {
			printf("hen: boost: high tier: back to %u Hz (not short of CPU for %llu ms), rc 0x%x; reads back %u\n",
			       HEN_BOOST_CPU_HZ, (unsigned long long) ((now - s_boostHighWanted) / 1000000), r[1], r[2]);
		}
	}
	s_boostHigh = high;
	s_boostHighSwitches++;
}
static void boost_set(bool on, u64 now) {
	if (on == s_boostOn) {
		return;
	}
#if HEN_DEV
	const bool was_by_clock = s_boostByClock;
#endif
	const u64 t0 = armGetSystemTick();
	// (--n3ds-clock-high: every change of the boost starts from its own rate; turning it off
	// puts the rate found back whatever the clock was at, sw_cpu_clock_end)
	if (s_boostHigh) {
		s_boostHigh = false;
		s_boostHighSwitches++;
	}
	if (on) {
		const bool clock_only = !s_boostDocked;
		s_boostByClock = sw_cpu_clock_quiet(HEN_BOOST_CPU_HZ);
		if (s_boostByClock) s_boostCalls.clock_on++; else s_boostCalls.clock_refused++;
		if (clock_only && !s_boostByClock) {
			printf("hen: boost: the clock controller refused the CPU rate in handheld: no boost "
			       "for the rest of this run (the system's boost mode is not used)\n");
			hen_load_boost = 0;
			return;
		}
		if (!s_boostByClock) {
			const Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
			s_boostCalls.system_on++;
			s_boostCalls.rc_on = rc;
			if (R_FAILED(rc)) {
				s_boostCalls.failed = 1;
				s_boostCalls.failed_rc = rc;
				hen_load_boost = 0; // (not available: stop asking)
				return;
			}
		}
	} else if (s_boostByClock) {
		sw_cpu_clock_end();
	} else {
		// Ending the system's boost mode. For an Application on Switch 2,
		// appletSetCpuBoostMode(Normal) keeps the request active and
		// appletCancelCpuBoostMode() clears it.
		static bool logged;
		const Result rc = appletCancelCpuBoostMode();
		if (!logged) {
			logged = true;
			printf("hen: boost: ended by appletCancelCpuBoostMode, rc 0x%x\n", rc);
		}
		s_boostCalls.system_off++;
		s_boostCalls.rc_off = rc;
		if (R_FAILED(rc)) {
			s_boostCalls.failed = 2;
			s_boostCalls.failed_rc = rc;
			hen_load_boost = 0;
			return;
		}
	}
	const unsigned long long took = armTicksToNs(armGetSystemTick() - t0);
	if (took > s_boostSwitchNsMax) {
		s_boostSwitchNsMax = took;
	}
	if (on) {
		s_boostOnSince = now;
		s_boostRaisedAt = now;
	} else {
		s_boostOnNs += now - s_boostOnSince;
	}
#if HEN_DEV
	// Handheld, by the clock: every change of the rate, with the reason and what it reads now.
	if (!s_boostDocked && (on ? s_boostByClock : was_by_clock)) {
		static const char* const why[5] = {"loading", "new code", "pipeline builds", "a busy emulation thread",
		                                   "held (New 3DS mode, or for the whole run)"};
		if (on) {
			printf("hen: boost: handheld: CPU clock raised (%s); reads %u Hz\n", why[s_boostWhyLast % 5],
			       sw_cpu_clock_now());
		} else {
			printf("hen: boost: handheld: CPU clock back (nothing has asked for it for %llu ms; it was raised for "
			       "%llu ms); reads %u Hz\n",
			       (unsigned long long) ((now - atomic_load_explicit(&s_boostLastWanted, memory_order_relaxed)) / 1000000),
			       (unsigned long long) ((now - s_boostRaisedAt) / 1000000), sw_cpu_clock_now());
		}
	}
#endif
	s_boostOn = on;
	s_boostSwitches++;
}
void hen_boost_off(void) { boost_set(false, armTicksToNs(armGetSystemTick())); }
// out[0..3]: times the clock was raised, the clock route was refused, the system's mode was
// turned on, and ended; [4], [5]: the last results of turning the system's mode on and ending
// it; [6]: 0, or 1/2 if a failed call (on/ending) switched the boost off; [7]: that call's
// result; [8]: the setting now (hen_load_boost); [9]: docked, as the boost last saw it.
void hen_boost_calls(unsigned* out) {
	out[0] = s_boostCalls.clock_on;
	out[1] = s_boostCalls.clock_refused;
	out[2] = s_boostCalls.system_on;
	out[3] = s_boostCalls.system_off;
	out[4] = s_boostCalls.rc_on;
	out[5] = s_boostCalls.rc_off;
	out[6] = s_boostCalls.failed;
	out[7] = s_boostCalls.failed_rc;
	out[8] = (unsigned) hen_load_boost;
	out[9] = s_boostDocked;
}
static unsigned long long boost_blocks(void);
// Where the time of showing a frame goes (renderer_vulkan, per report interval): [0] frames
// taken, [1] ns waiting for a free frame, [2] ns waiting for that frame's last showing to be
// done by the GPU, [3] frames shown, [4] ns getting the display's next image, [5] ns handing
// the frame's commands to the GPU, [6] ns handing the image to the display, [7] ns drawing
// the screens into the frame (the emulation thread's side).
#if HEN_DEV
unsigned long long hen_swap_ns[8];
#endif
// --present-cpu-acquire (off by default): the present thread waits on the
// CPU for the display to let go of the image it acquired, and submits its copy without the
// image_acquired semaphore, so that nothing in the GPU's queue waits for the display
// (video_core/renderer_vulkan/vk_present_window.cpp, vk_swapchain.cpp).
int hen_present_cpu_acquire = 0;
// Those waits: count, ns in all, longest ns (emptied at every report); [3]: how many of
// them began right after the present before, ahead of their frame.
#if HEN_DEV
unsigned long long hen_present_acquire_wait[4];
#endif
int hen_assert_test_frame; // (--assert-test N, development builds: video_core/gpu.cpp)
#if HEN_DEV
unsigned long long hen_vblank_count; // (vertical blanks of the program so far: --screenshot-frame)
#endif
#if HEN_DEV
void hen_core_sample_emu(int at_vblank); // hen_thread.c (which host core this thread is on)
#endif
void hen_thread_name(const char* name);  // hen_thread.c
void hen_boost_tick(long long limiter_slept_total_us) {
	const u64 now = armTicksToNs(armGetSystemTick());
#if HEN_DEV
	hen_vblank_count++;
	hen_core_sample_emu(1);
#endif
	const unsigned at = s_boostAt++ % 128;
	s_boostRing[at].ns = now;
	s_boostRing[at].frames = atomic_load_explicit(&s_boostFrames, memory_order_relaxed);
	s_boostRing[at].blocks = boost_blocks();
	s_boostRing[at].demand = atomic_load_explicit(&s_boostDemandBlocks, memory_order_relaxed);
	s_boostRing[at].slept = limiter_slept_total_us;
	s_boostRing[at].work = atomic_load_explicit(&s_boostWorkSum, memory_order_relaxed);
	s_boostRing[at].present_ns = atomic_load_explicit(&s_boostPresentNs, memory_order_relaxed);
	s_boostRing[at].present_frames = atomic_load_explicit(&s_boostPresentFrames, memory_order_relaxed);
	// The oldest entry of the last second (at least half a second of history before judging).
	unsigned old = at;
	for (unsigned back = 1; back < 128 && back < s_boostAt; back++) {
		const unsigned i = (at + 128 - back) % 128;
		if (now - s_boostRing[i].ns > 1000000000ull) {
			break;
		}
		old = i;
	}
	const u64 span = now - s_boostRing[old].ns;
	bool short_of_cpu = false; // (--n3ds-clock-high)
	if (span >= 500000000ull) {
		const double seconds = span / 1e9;
		const double fps = (s_boostRing[at].frames - s_boostRing[old].frames) / seconds;
		const double blocks = (s_boostRing[at].blocks - s_boostRing[old].blocks) / seconds;
		const double slept = (s_boostRing[at].slept - s_boostRing[old].slept) / 1e6 / seconds;
		const bool loading = fps < 15.0 && slept < 0.2;
		const bool compiling = blocks > 1500.0;
		// (The boost goes by bursts: over 30 ms of pipeline compiles in the second. In a first
		// fight a few pipelines are compiled every second throughout, and a rule that
		// goes by any build over 3 ms keeps the boost on for the whole fight.)
		const bool any_pipeline = now - atomic_load_explicit(&s_boostWorkNs, memory_order_relaxed) < 500000000ull;
		const bool pipelines = (s_boostRing[at].work - s_boostRing[old].work) / seconds > 30e6;
		// What asks for the boost. The boost is the CPU's clock raised by itself
		// (boost_set), which costs the GPU nothing, so any pipeline build in the last half
		// second counts too, not only bursts.
		// A New 3DS title in its fast mode holds the clock for as long as it runs: it needs the
		// CPU all the time, not while something compiles.
		const bool n3ds = hen_n3ds_mode;
		// By the clock there are two more reasons (with the bursts alone the clock
		// did no better than the system's mode, 143 against 147 ms of lag in Pokemon OR; what
		// the held clock won came from being raised already when a hitch arrives):
		// new code: over 5 ms of the second spent in the interpreter (code not compiled yet);
		// busy: the emulation thread sleeps under a tenth of the time. With the clock raised the
		// same load sleeps more (1523.2 / 998.4 MHz: 90% busy becomes 59%), so while raised
		// the line is 41%: it stays up for as long as the normal clock would be short.
		const bool new_code = (s_boostRing[at].demand - s_boostRing[old].demand) / seconds > 5e6;
		// (the background re-sort waits while code is new, whatever the boost's rules are)
		if (compiling || (s_boostRing[at].demand - s_boostRing[old].demand) / seconds > 5e6) {
			atomic_store_explicit(&s_resortBusyLast, now, memory_order_relaxed);
		}
		const bool busy = slept < (s_boostOn && s_boostByClock ? BOOST_BUSY_RAISED() : 0.10);
		if (loading || compiling || pipelines || any_pipeline || n3ds || new_code || busy) {
			atomic_store_explicit(&s_boostLastWanted, now, memory_order_relaxed);
			s_boostWhyLast = loading ? 0 : compiling || new_code ? 1 : pipelines || any_pipeline ? 2 : busy ? 3 : 4;
			s_boostWhy[s_boostWhyLast]++;
		}
		// (--n3ds-clock-high: "busy" at the rate the clock is at now; see HEN_BOOST_HIGH_HZ)
		short_of_cpu = slept < (s_boostHigh ? 1.0 - 0.9 * HEN_BOOST_CPU_HZ / HEN_BOOST_HIGH_HZ : 0.10);
	}
	const u64 last = atomic_load_explicit(&s_boostLastWanted, memory_order_relaxed);
	// (docked or not: asked once a second, it is a call to the system)
	if (s_boostAt % 60 == 1) {
		const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
		if (docked != s_boostDocked) {
			sw_cpu_clock_forget(); // (the system resets the clocks when the mode changes)
			boost_set(false, now);
			s_boostDocked = docked;
		}
		// A start is a reason by itself: the first seconds of any title are loads and code
		// seen for the first time, and the signals above need half a second of history.
		if (s_boostAt == 1 && hen_load_boost == 1) {
			atomic_store_explicit(&s_boostLastWanted, now, memory_order_relaxed);
		}
		// Handheld, the clock raised by us: still ours? (the system applies its own rates
		// again at times; docked this is not looked at)
		if (!s_boostDocked && s_boostOn && s_boostByClock) {
			sw_cpu_clock_keep();
		}
	}
	if (hen_load_boost) {
		const u64 linger_ms = hen_boost_linger_ms > 0 ? (u64) hen_boost_linger_ms : 10000;
		const u64 wanted = atomic_load_explicit(&s_boostLastWanted, memory_order_relaxed);
		(void) last;
		boost_set(wanted != 0 && now - wanted < linger_ms * 1000000ull, now);
		// --n3ds-clock-high: the second rate, while the boost is the CPU's clock and the title is
		// in the New 3DS fast mode. Up as soon as the title is short of CPU, down after the
		// boost's linger without it, at once if the title leaves the fast mode.
		if (hen_n3ds_clock_high && s_boostOn && s_boostByClock) {
			const bool eligible = hen_n3ds_mode && !s_boostHighRefused;
			if (eligible && short_of_cpu) {
				s_boostHighWanted = now;
				s_boostHighShort++;
			}
			boost_tier(eligible && s_boostHighWanted != 0 && now - s_boostHighWanted < linger_ms * 1000000ull, now);
			if (s_boostHigh) {
				s_boostHighTicks++;
			}
		}
	}
}
// For the report: milliseconds on since the last call, switches, and ticks by reason.
void hen_boost_stats(unsigned long long* out) {
	const u64 now = armTicksToNs(armGetSystemTick());
	if (s_boostOn) {
		s_boostOnNs += now - s_boostOnSince;
		s_boostOnSince = now;
	}
	out[0] = s_boostOnNs / 1000000;
	out[1] = s_boostSwitches;
	out[2] = s_boostWhy[0];
	out[3] = s_boostWhy[1];
	out[4] = s_boostWhy[2];
	out[5] = s_boostSwitchNsMax / 1000;
	out[6] = s_boostByClock;
	out[7] = s_boostWhy[3];
	out[8] = s_boostWhy[4];
	// (--n3ds-clock-high) [9]: 0 flag off, 1 on, 2 on and the high rate was refused; [10]:
	// vertical blanks at the high rate; [11]: switches between the two rates; [12]: vertical
	// blanks on which the title was short of CPU.
	out[9] = hen_n3ds_clock_high ? (s_boostHighRefused ? 2 : 1) : 0;
	out[10] = s_boostHighTicks;
	out[11] = s_boostHighSwitches;
	out[12] = s_boostHighShort;
	s_boostHighTicks = s_boostHighSwitches = s_boostHighShort = 0;
	s_boostSwitchNsMax = 0;
	s_boostOnNs = s_boostSwitches = 0;
	s_boostWhy[0] = s_boostWhy[1] = s_boostWhy[2] = s_boostWhy[3] = s_boostWhy[4] = 0;
}

int hen_live_fpsr = 1; // --no-live-fpsr: 0 (dynarmic, backend/arm64/fpsr_manager.cpp)
int hen_lazy_resched = 1; // --no-lazy-resched: 0 (svc.cpp, SVC::CallSVC)
#if HEN_DEV
unsigned long long hen_resched_skipped = 0;
#endif
// A New 3DS title that asks for the 804 MHz clock gets its CPU work charged less emulated
// time (src/core/core_timing.cpp). 2 (default): automatic, a third of the time at best and
// more when the host has none to spare. 1 (--n3ds-clock): a third, fixed. 0 (--n3ds-clock-off):
// as at 268 MHz, upstream's.
int hen_n3ds_clock = 2;
unsigned long long hen_n3ds_clock_ticks = 0;
// --gpu-thread: PICA command lists, fills and transfers on a thread of their own, their
// interrupts signalled when the work is done (src/video_core/hen_gpu_thread.cpp).
// ON (--no-gpu-thread turns it off). Cold, along the recorded routes, first 55 s of five
// titles: emulation-thread stalls of 20 ms or more 42 -> 12 (1597 -> 528 ms), lag given up
// 2065 -> 1906 ms, its core time down 13 to 30% a title.
int hen_gpu_thread = 1;
// --gpu-thread-sync-writes: 0. A program's write to memory the renderer has cached waits for
// the GPU thread instead of queueing the invalidation behind its work (for comparing).
int hen_gpu_defer_writes = 1;
int hen_depth_lag = 1;  // --sync-depth-reads: 0 (vk_texture_runtime.cpp, Surface::Download)
// Draws are shaded on the CPU while their pipeline is compiled (vk_rasterizer.cpp,
// AccelerateDrawBatchInternal).
int hen_cpu_shade_share = 33;  // --cpu-shade-share P: percent of wall time such draws may take when there is none to spare
int hen_max_lag_ms = 60; // --max-lag-ms
// What a vertex shaded on the CPU costs, as measured by PicaCore (a running mean; starts at
// the shader JIT's single-thread figure).
#if HEN_DEV
unsigned long long hen_cpu_shade_ns_per_vertex = 500;
long long hen_limiter_given_up_us;  // core/perf_stats.cpp; reported and reset by main.cpp
#endif
long long hen_limiter_slept_us;  // core/perf_stats.cpp; reset by main.cpp
// Stand-in pipelines (for draws shaded on the CPU): [0..2] built on the emulation thread: count,
// ns in total, longest ns; [3..5] the same for those the shader workers built; [6] already built.
// Frames shown (renderer_vulkan.cpp): count, gaps over 40 / 70 / 120 ms, the longest gap (us),
// and the time in gaps over 40 ms (us). Reported and reset by main.cpp.
unsigned long long hen_present_gaps[6];
#if HEN_DEV
unsigned long long hen_standin_stats[7];
#endif
// The same builds by part: vertex input interface, the stand-in's vertex side, fragment shader,
// output interface, link (ns); [5] parts the driver had to compile (not repeats).
#if HEN_DEV
unsigned long long hen_standin_part_ns[6];
#endif
__thread int hen_standin_build; // set around a stand-in's Build() (vk_rasterizer / vk_graphics_pipeline)
#if HEN_DEV
unsigned long long hen_fast_lane_skips;  // vk_rasterizer.cpp; reported and reset by main.cpp
#endif
long long hen_cpu_shade_debt_ns;  // (the emulation thread's; core/perf_stats.cpp pays it off with the frame limiter's sleeps)
int hen_shader_jit = 1;       // the PICA shader JIT (--no-shader-jit: the interpreter, about 3 times slower a vertex)
int hen_inline_exclusive = 1; // --no-inline-exclusive (dynarmic's emit_arm64_memory.cpp)
// Generated code looks reads up in a table of their own, in which a page the renderer has
// cached keeps its memory while nothing drawn is waiting to be written back to it
// (src/core/memory.h, PageTable::Pointers): such reads no longer leave generated code (1 to
// 2 million per 5 s in Xenoblade Chronicles 3D, about 100 ns each). --no-read-table: 0, one
// table for reads and writes as upstream.
int hen_read_table = 1;
// ARM state: a conditional branch ends the block before it with both ways out instead of being
// a block of its own (dynarmic's a32_branch.cpp, arm_B): a fifth fewer block entries in hot
// code. --no-fold-cond-branch: 0, upstream's blocks.
int hen_fold_cond_branch = 1;
// Code buffers of 4 MiB and more: the execute range and the heap memory mapped there both
// start on a 2 MiB boundary and are a multiple of 2 MiB long, so the kernel can map them
// with 2 MiB blocks if it does that (hen_jit_create, sync). --no-code-align-2m: 0, pages.
int hen_code_align_2m = 1;
int hen_depth_scale_blit = 1; // --no-depth-scale-blit (vk_texture_runtime.cpp, Surface::BlitScale)
// Early depth: in Z-buffer mode with a depth range inside 0..1 the
// viewport's depth range carries the PICA depth scale and offset and the fragment shader does
// not write depth, so the hardware may test depth before shading (vk_rasterizer.cpp, Draw; the
// two fragment shader generators). --no-early-depth: 0, every shader writes depth.
int hen_early_depth = 1;
// The generic fragment shader's draws too: a draw that qualifies takes the variant of
// that shader without the depth write and keeps the PICA depth range in the viewport
// (vk_rasterizer.cpp, HenDrawGeneric). --no-generic-early-depth: 0, every generic draw
// with the depth write under the range 0..1. Set to 0 by the renderer if the driver refuses
// the variant's pipeline.
int hen_generic_early_depth = 1;
// The generic fragment shader's draws are shaded once per square of this many pixels a side
// (1: every pixel; 2 or 4 where the device has VK_KHR_fragment_shading_rate). --generic-shading-rate N.
int hen_generic_shading_rate = 2;
// The generic fragment shader's draws by frame, since the last report (vk_rasterizer.cpp):
// [0] frames that had any; the most in one frame [1] in all, [2] with the depth write, [3]
// without it; draws [4] with the depth write, [5] without it.
#if HEN_DEV
unsigned long long hen_generic_frame_stats[6];
#endif
// Since the last report: draws that kept the shader's depth write, draws that could go without
// it (and did, with the flag), of those the ones whose shader has no discard either.
#if HEN_DEV
unsigned long long hen_early_depth_stats[3];
#endif
int hen_pipeline_libraries = 1; // --whole-pipelines turns it off (vk_graphics_pipeline.cpp)
unsigned hen_cia_chunk_bytes = 0; // --cia-chunk-kib (0: the default, 16 MiB; am.cpp)
unsigned long long hen_cia_read_ns = 0, hen_cia_write_ns = 0, hen_cia_chunks = 0;
#if HEN_DEV
// How generated code leaves a block, counted (--no-exit-counts: 0, to measure the counting).
// Generated code adds to hen_exit_count itself, in the dispatcher's entry code (dynarmic's
// a32_address_space.cpp, EmitPrelude): plain adds, the emulated cores run on one thread.
// A direct branch to a linked block is not counted (it would cost that path instructions).
//   0 a return that found its target in the return stack buffer (PopRSBHint)
//   1 a return that went to the dispatcher        2 an indirect branch (FastDispatchHint)
//   3 any other exit to the dispatcher            4 a direct branch whose target was not linked
//   5 the dispatcher found a halt request         6 the dispatcher found the time slice used up
//   7 the dispatcher's table missed: the lookup in C++
int hen_exit_counts = 0; // (about 1% while on: --exit-counts)
unsigned long long hen_exit_count[8];
static _Atomic unsigned long long s_blockTicks, s_blocksEmitted, s_blocksFolded;
// A block was compiled, of this many ticks (its length, for estimating blocks from ticks);
// folded: it ends in a conditional branch that would have been a block of its own
// (hen_fold_cond_branch).
void hen_jit_note_block_ticks(unsigned ticks, int folded) {
	atomic_fetch_add_explicit(&s_blockTicks, ticks, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_blocksEmitted, 1, memory_order_relaxed);
	if (folded) {
		atomic_fetch_add_explicit(&s_blocksFolded, 1, memory_order_relaxed);
	}
}
// All blocks compiled so far: their ticks, their number, those with a folded branch.
void hen_jit_block_stats(unsigned long long* out) {
	out[0] = atomic_load(&s_blockTicks);
	out[1] = atomic_load(&s_blocksEmitted);
	out[2] = atomic_load(&s_blocksFolded);
}
// out[0..7]: hen_exit_count since the last call; out[8], out[9]: ticks and number of all
// blocks compiled so far.
void hen_jit_exit_stats(unsigned long long* out) {
	for (int i = 0; i < 8; i++) {
		out[i] = hen_exit_count[i];
		hen_exit_count[i] = 0;
	}
	out[8] = atomic_load(&s_blockTicks);
	out[9] = atomic_load(&s_blocksEmitted);
}
int hen_jit_trace = 0; // --jit-trace
int hen_pipe_log = 0; // --pipe-log (vk_graphics_pipeline.cpp)
#endif
// The emulation thread's counters from here to hen_emu_stats are the development builds': the
// places that count compile them for those builds alone.
#if HEN_DEV
static _Atomic unsigned long long s_lookups;
void hen_jit_note_lookup(void) { atomic_fetch_add_explicit(&s_lookups, 1, memory_order_relaxed); }
unsigned long long hen_emu_ns[12], hen_emu_count[12];  // ("hen: emu:": below)
static _Atomic unsigned long long s_runNs, s_runs;
void hen_jit_note_run(unsigned long long ns) {
	atomic_fetch_add_explicit(&s_runNs, ns, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_runs, 1, memory_order_relaxed);
	hen_emu_ns[7] += ns;
	hen_emu_count[7]++;
}
// Since the last call: lookups, run ns, runs.
void hen_jit_run_stats(unsigned long long* out) {
	out[0] = atomic_exchange(&s_lookups, 0);
	out[1] = atomic_exchange(&s_runNs, 0);
	out[2] = atomic_exchange(&s_runs, 0);
}

// ---- Where the emulation thread's time goes, for the "hen: emu:" line ----
//
// The clock is read at these points only with hen_emu_stats_on (set while a title runs); the
// counts are plain adds. All of it is written by the emulation thread
// and read by it at the 5 s report, so nothing is atomic (hen_dispatch_count[4] and the layout
// figures can be written by another thread under the compile lock: read as they are).
//   0 passes of the run loop (core/core.cpp, System::RunLoop), ns inside it
//   1 system calls of the program (arm_dynarmic.cpp, CallSVC): everything they do, services too
//   2 the dispatcher's lookups in C++ (dynarmic, a32_address_space.cpp): its table missed
//   3 commands handed to the GPU thread (hen_gpu_thread.cpp, Queue), without a wait for room
//   4 switches between emulated threads (kernel/thread.cpp, SwitchContext): count only
//   5, 6 memory reads and writes that left generated code: count only
//   7 slices of the emulated CPU (arm_dynarmic.cpp, Run: what "hen: jit run:" counts)
//   8 waits of the emulation thread for the GPU thread, every kind (hen_gpu_thread.cpp, NoteWait)
int hen_emu_stats_on;
// ("hen: emu svc:") System calls by number and the time in them (arm_dynarmic.cpp, CallSVC),
// and what the waiting calls did (kernel/svc.cpp has the list); reported and emptied by
// Kernel::HenSvcReport.
unsigned long long hen_svc_seen[256], hen_svc_seen_ns[256];
unsigned long long hen_wait_count[16];
// The dispatcher's table (dynarmic, address_space.h: 65536 entries, one block each, the newest
// wins) at a lookup in C++: 0 the entry was empty, 1 it held another block (which is now out
// of the table), 2 it held this block already, 3 nothing was stored (the block is not compiled:
// to the interpreter), 4 the whole table was emptied (blocks were invalidated).
unsigned long long hen_dispatch_count[5];
// Every block placed in a code buffer, in the order placed: how many, a figure that depends
// only on which blocks (a sum), and one that depends on their order and addresses as well.
// Two runs with the same three have the same code at the same addresses.
static unsigned long long s_placed, s_placedSet, s_placedOrder;
static unsigned long long place_mix(unsigned long long x) {
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdull;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ull;
	return x ^ x >> 33;
}
// Blocks and their bytes, for "hen: layout:".
static _Atomic unsigned long long s_layoutBlocks, s_layoutBytes;
void hen_jit_note_block_place(unsigned long long descriptor, unsigned long long entry, unsigned long long bytes) {
	s_placed++;
	s_placedSet += place_mix(descriptor);
	s_placedOrder = place_mix(s_placedOrder ^ place_mix(descriptor) ^ entry * 0x9e3779b97f4a7c15ull);
	atomic_fetch_add_explicit(&s_layoutBlocks, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_layoutBytes, bytes, memory_order_relaxed);
}
// ns[12], count[12], dispatch[5]: since the last call; layout[3]: of the whole session.
void hen_emu_stats(unsigned long long* ns, unsigned long long* count, unsigned long long* dispatch,
                   unsigned long long* layout) {
	for (int i = 0; i < 12; i++) {
		ns[i] = hen_emu_ns[i];
		count[i] = hen_emu_count[i];
		hen_emu_ns[i] = hen_emu_count[i] = 0;
	}
	for (int i = 0; i < 5; i++) {
		dispatch[i] = hen_dispatch_count[i];
		hen_dispatch_count[i] = 0;
	}
	layout[0] = s_placed;
	layout[1] = s_placedSet;
	layout[2] = s_placedOrder;
}
#endif

// A JIT threw away all its code because its buffer was full (it recompiles from scratch).
void hen_jit_note_cache_clear(void) { atomic_fetch_add(&s_clears, 1); }

// ---- The background re-sort: "hen: resort:" ----
//
// While a title runs, each JIT's compile worker builds a second copy of all its compiled blocks,
// dense and in the order of the program's addresses, in a spare code buffer, and the emulation
// thread changes over to it between two slices (dynarmic's address_space.cpp, "Re-sorting the
// code in the background"; the method and the numbers: hen_resort.h). On by default.
// --no-jit-resort: 0, the code stays where it was compiled.
int hen_resort = 1;
// --jit-resort-at SECONDS: one re-sort of every JIT that has code that long after the title's
// start, whatever the rules say, and no other (a test's trigger). -1: the rules.
#if HEN_DEV
int hen_resort_at_ms = -1;
#endif
// 1 by default: the buffer a re-sort leaves is unmapped and freed after the change-over, so a
// re-sorted JIT holds one buffer between re-sorts, not two; the next re-sort maps a fresh one
// (3.5 to 12 ms on the worker). --jit-resort-keep: 0, kept as the next re-sort's buffer (128 MiB
// of write area and as much mapped source stay allocated for each JIT that has re-sorted).
int hen_resort_release = 1;
// --jit-resort-poison (development builds): the buffer a re-sort leaves is filled with an
// instruction that faults, so that anything still leading into the old code is found at once.
#if HEN_DEV
int hen_resort_poison = 0;
#endif
// --jit-clear-at SECONDS (a test's trigger): every JIT that has code clears its cache at its
// first slice that long after the title's start, once. With --jit-resort-poison (development
// builds) the old code is overwritten with an instruction that faults. -1: never.
#if HEN_DEV
int hen_jit_clear_at_ms = -1;
#endif
// --jit-resort-chunk-kib N: the most bytes of a copy in one publish request (the lock that
// every code update of every JIT takes is held for one request).
int hen_resort_chunk = 256 << 10;
// 0 blocks in sorted order, 1 their bytes, 2 blocks compiled since (in compile order), 3 their
// bytes: what exists now, over all JITs. 4 re-sorts done, 5 abandoned, 6 failed checks, 7 the
// longest pause of the emulation thread at a change-over (us): of the session.
#if HEN_DEV
static _Atomic long long s_resort[8];
void hen_resort_note(int kind, long long delta) { atomic_fetch_add_explicit(&s_resort[kind & 7], delta, memory_order_relaxed); }
void hen_resort_note_pause(unsigned long long us) {
	if ((long long) us > atomic_load_explicit(&s_resort[7], memory_order_relaxed)) {
		atomic_store_explicit(&s_resort[7], (long long) us, memory_order_relaxed);
	}
}
void hen_resort_stats(long long* out) {
	for (int i = 0; i < 8; i++) {
		out[i] = atomic_load_explicit(&s_resort[i], memory_order_relaxed);
	}
}
#endif
// One copy is made at a time, over all JITs: 1 if the caller may begin.
static atomic_flag s_resortClaimed = ATOMIC_FLAG_INIT;
int hen_resort_claim(void) { return !atomic_flag_test_and_set(&s_resortClaimed); }
void hen_resort_unclaim(void) { atomic_flag_clear(&s_resortClaimed); }
static _Atomic unsigned s_resortNumber;
unsigned hen_resort_number(void) { return atomic_fetch_add(&s_resortNumber, 1) + 1; }
// When the title started (the block list is opened then: core/arm/dynarmic/hen_block_list.cpp).
static _Atomic unsigned long long s_resortOrigin;
void hen_resort_title_start(void) { atomic_store(&s_resortOrigin, armTicksToNs(armGetSystemTick())); }
unsigned long long hen_resort_origin(void) {
	unsigned long long origin = atomic_load(&s_resortOrigin);
	if (origin == 0) {
		origin = armTicksToNs(armGetSystemTick());
		atomic_store(&s_resortOrigin, origin);
	}
	return origin;
}
// When the title was last seen compiling a lot or interpreting code that is not compiled yet
// (hen_boost_tick's two measures, without the boost's own conditions): no re-sort begins then.
unsigned long long hen_resort_busy_last(void) { return atomic_load_explicit(&s_resortBusyLast, memory_order_relaxed); }

#if HEN_DEV
// Which pages of generated code were in use in a report's interval, from a sample of it: the
// block entries the dispatcher looked up in C++ (dynarmic, a32_address_space.cpp: only the
// transfers its table missed; with hen_emu_stats_on; the emulation thread's).
// A set of 4 KiB page numbers; the 2 MiB count is made from it at the report.
#define TOUCH_SLOTS 8192
struct touch_set {
	u32 page[TOUCH_SLOTS];  // page number + 1; 0: free
	unsigned count;
	unsigned long long notes;
};
static struct touch_set s_touch;
static void touch_add(struct touch_set* set, u64 address) {
	set->notes++;
	const u32 key = (u32) (address >> 12) + 1;
	for (unsigned i = 0, at = (key * 2654435761u) >> 19; i < TOUCH_SLOTS; i++, at = (at + 1) % TOUCH_SLOTS) {
		if (set->page[at] == key) {
			return;
		}
		if (set->page[at] == 0) {
			if (set->count < TOUCH_SLOTS * 3 / 4) {  // (full: no more pages are told apart)
				set->page[at] = key;
				set->count++;
			}
			return;
		}
	}
}
// out[0]: notes, out[1]: 4 KiB pages, out[2]: 2 MiB pages; emptied.
static void touch_read(struct touch_set* set, unsigned long long* out) {
	static u32 big[TOUCH_SLOTS];  // (the emulation thread's report only)
	unsigned bigCount = 0;
	for (unsigned i = 0; i < TOUCH_SLOTS; i++) {
		if (set->page[i] == 0) {
			continue;
		}
		const u32 block = (set->page[i] - 1) >> 9;
		unsigned k = 0;
		while (k < bigCount && big[k] != block) {
			k++;
		}
		if (k == bigCount) {
			big[bigCount++] = block;
		}
	}
	out[0] = set->notes;
	out[1] = set->count;
	out[2] = bigCount;
	memset(set, 0, sizeof(*set));
}
void hen_layout_touch(const void* entry) {
	if (hen_emu_stats_on) {
		touch_add(&s_touch, (u64) (uintptr_t) entry);
	}
}
// For "hen: layout:", from the emulation thread's report. Of the session: out[0] blocks,
// [1] their bytes. Since the last call: [2] dispatcher lookups noted, [3] 4 KiB pages and
// [4] 2 MiB pages their block entries were in.
void hen_layout_stats(unsigned long long* out) {
	out[0] = atomic_load_explicit(&s_layoutBlocks, memory_order_relaxed);
	out[1] = atomic_load_explicit(&s_layoutBytes, memory_order_relaxed);
	touch_read(&s_touch, out + 2);
}
#endif

// The sampling profiler and the black box below: development builds only (HEN_DEV).
#if HEN_DEV
// --profile: where the emulation thread is, sampled every 2 ms by pausing it and reading its
// registers. Host addresses inside this program are counted by 64-byte piece (offsets from
// _start, for addr2line on the .elf); samples in generated code by the emulated program's
// last block address (the A32 state's r15, which is as old as the block's start). Printed
// with the 5-second statistics.
int hen_profile = 0;
extern char _start[];
extern const unsigned hen_a32_regs_offset; // dynarmic's a32_address_space.cpp
#define PROF_SLOTS 8192
static struct { u64 key; u32 count; } s_profHost[PROF_SLOTS], s_profGuest[PROF_SLOTS], s_profCode[PROF_SLOTS];
static _Atomic unsigned s_profSamples, s_profJit, s_profOther, s_profFailed, s_profPrelude;
static struct { uintptr_t start, size; } s_profRanges[16];
static unsigned s_profRangeCount;
static Thread s_profThread;
static Handle s_profTarget;
static volatile bool s_profStop, s_profStarted;
static Mutex s_profLock;
// What kind of generated instruction a sample landed on (and the one before it: the one a
// stalled core is usually still waiting for). Works however widely the samples are spread
// over blocks.
enum { PK_LINK, PK_PAGETABLE, PK_GUESTMEM, PK_STATE, PK_STACK, PK_NZCV, PK_FPSYS, PK_BR, PK_B, PK_BCOND, PK_FP, PK_ATOMIC, PK_MOVIMM, PK_OTHER, PK_COUNT };
static const char* const s_profKindName[PK_COUNT] = { "link-slot", "page-table", "guest-memory", "state", "stack", "nzcv", "fpsr/fpcr", "br/blr/ret", "b/bl", "b.cond/cbz", "float", "atomic", "mov-imm", "other" };
static _Atomic unsigned s_profKindAt[PK_COUNT], s_profKindBefore[PK_COUNT];
static int prof_kind(u32 w) {
	if (w == 0xf9400210 || w == 0xf9400231) return PK_LINK;                       // ldr x16, [x16] / ldr x17, [x17]
	if ((w & 0xffe0ffe0) == 0xf8607b00) return PK_PAGETABLE;                      // ldr x, [x24, x, lsl #3]
	if ((w & 0x3b200c00) == 0x38200800) return PK_GUESTMEM;                       // load/store, register offset
	if ((w & 0x3b000000) == 0x39000000 || (w & 0x3a000000) == 0x28000000 || (w & 0x3b200000) == 0x38000000) { // unsigned offset, pair, unscaled
		const unsigned rn = (w >> 5) & 31;
		return rn == 28 ? PK_STATE : rn == 31 ? PK_STACK : PK_GUESTMEM;
	}
	if ((w & 0xffdfffe0) == 0xd51b4200) return PK_NZCV;                           // msr/mrs nzcv
	if ((w & 0xffdfff00) == 0xd51b4400) return PK_FPSYS;                          // msr/mrs fpcr, fpsr
	if ((w & 0xfe000000) == 0xd6000000) return PK_BR;
	if ((w & 0x7c000000) == 0x14000000) return PK_B;
	if ((w & 0xff000000) == 0x54000000 || (w & 0x7e000000) == 0x34000000 || (w & 0x7e000000) == 0x36000000) return PK_BCOND;
	if ((w & 0x0e000000) == 0x0e000000) return PK_FP;                             // FP and SIMD data processing
	if ((w & 0x3f000000) == 0x08000000) return PK_ATOMIC;                         // exclusive, acquire/release
	if ((w & 0x1f800000) == 0x12800000) return PK_MOVIMM;                         // movz/movn/movk
	return PK_OTHER;
}

static void prof_add(void* table_, u64 key) {
	struct { u64 key; u32 count; }* table = table_;
	for (unsigned i = 0, at = (unsigned) (key * 0x9E3779B97F4A7C15ull >> 51); i < 64; i++, at = (at + 1) % PROF_SLOTS) {
		if (table[at].count == 0 || table[at].key == key) {
			table[at].key = key;
			table[at].count++;
			return;
		}
	}
}
// --no-blackbox turns this off. Crashes leave no report on this runtime (the exception handler
// is never called), so the log carries a line a second saying where the
// emulation thread and the compile worker were at their last few samples (25 a second each):
// "+offset" in this program (addr2line), "J:address" in generated code with the emulated
// program's last block address, "?" elsewhere. After a crash, the last line is the evidence.
int hen_blackbox = 1;
extern Handle hen_worker_handle;
static bool prof_sample(Handle thread, char* out, size_t size) {
	if (thread == 0 || R_FAILED(svcSetThreadActivity(thread, ThreadActivity_Paused))) {
		return false;
	}
	ThreadContext ctx;
	const Result rc = svcGetThreadContext3(&ctx, thread);
	// (while it is still paused: up to eight callers from its frame records, so that a
	// sample inside a lock says who wanted the lock)
	uintptr_t callers[8] = {0};
	if (R_SUCCEEDED(rc)) {
		uintptr_t fp = ctx.fp;
		for (unsigned i = 0; i < 8; i++) {
			MemoryInfo info;
			u32 page_info;
			if (fp == 0 || (fp & 15) != 0 || R_FAILED(svcQueryMemory(&info, &page_info, fp)) || !(info.perm & Perm_R) ||
			    fp + 16 > info.addr + info.size) {
				break;
			}
			callers[i] = ((const uintptr_t*) fp)[1];
			const uintptr_t next = ((const uintptr_t*) fp)[0];
			if (next <= fp) {
				break;
			}
			fp = next;
		}
	}
	svcSetThreadActivity(thread, ThreadActivity_Runnable);
	if (R_FAILED(rc)) {
		return false;
	}
	const uintptr_t pc = ctx.pc.x;
	if (pc - (uintptr_t) _start < ((uintptr_t) 256 << 20)) {
		int at = snprintf(out, size, "+%lx/%lx", (unsigned long) (pc - (uintptr_t) _start), (unsigned long) (ctx.lr - (uintptr_t) _start));
		for (unsigned i = 0; i < 8 && callers[i] != 0 && at > 0 && (size_t) at < size; i++) {
			at += snprintf(out + at, size - at, "/%lx", (unsigned long) (callers[i] - (uintptr_t) _start));
		}
		return true;
	}
	for (unsigned i = 0; i < s_profRangeCount; i++) {
		if (pc - s_profRanges[i].start < s_profRanges[i].size) {
			const uintptr_t state = ctx.cpu_gprs[28].x;
			MemoryInfo info;
			u32 page_info;
			u32 guest = 0;
			if (state != 0 && (state & 3) == 0 && R_SUCCEEDED(svcQueryMemory(&info, &page_info, state)) && (info.perm & Perm_R) &&
			    info.type == MemType_Heap && state + 256 < info.addr + info.size) {
				guest = ((const u32*) (state + hen_a32_regs_offset))[15];
			}
			snprintf(out, size, "J%u.%lx:%08x", i, (unsigned long) (pc - s_profRanges[i].start), guest);
			return true;
		}
	}
	snprintf(out, size, "?/%lx", (unsigned long) (ctx.lr - (uintptr_t) _start));
	return true;
}
// The emulation thread's last samples, one every 10 ms, for hen_blackbox_dump.
enum { BB_RING = 64, BB_KEEP = 6 };
static char s_bbEmu[BB_RING][136];
static char s_bbExtra[BB_RING][136];  // (the renderer's command thread: hen_blackbox_watch)
static Handle s_bbExtraHandle;
static u64 s_bbTick[BB_RING];
// Called by a thread that the black box should sample next to the emulation thread (the
// Vulkan scheduler's worker: the emulation thread waits for it at every frame).
void hen_blackbox_watch(void) { s_bbExtraHandle = threadGetCurHandle(); }
// And the Vulkan scheduler's worker, which records and submits what the renderer's
// thread queued: a surface download waits for it before it waits for the GPU.
static char s_bbSched[BB_RING][136];
static Handle s_bbSchedHandle;
void hen_blackbox_watch_scheduler(void) { s_bbSchedHandle = threadGetCurHandle(); }
void hen_blackbox_unwatch_scheduler(void) { s_bbSchedHandle = 0; }
static _Atomic unsigned s_bbCount;
// After something slow on the emulation thread (called from it): where it was during the
// last ms milliseconds, one sample every 10 ms, as offsets for addr2line.
void hen_blackbox_dump(unsigned ms) {
	if (!s_profStarted || hen_profile) {
		return;
	}
	const unsigned count = atomic_load(&s_bbCount);
	const u64 now = armGetSystemTick();
	// (short stalls, as in a scene that is slow every frame: one dump a second at most)
	static u64 last;
	if (ms < 80 && armTicksToNs(now - last) < 1000000000ull) {
		return;
	}
	last = now;
	static char line[BB_RING * 140];
	for (int which = 0; which < 3; which++) {
		if (which == 2 && (s_bbSchedHandle == 0 || s_bbSchedHandle == s_bbExtraHandle)) {
			break;
		}
		int at = snprintf(line, sizeof(line), which == 0 ? "hen: bb: the last %u ms:" : which == 1 ? "hen: bb: the renderer's thread in those %u ms:" : "hen: bb: the scheduler's worker in those %u ms:", ms);
		for (unsigned k = BB_RING; k > 0; k--) {
			if (count < k) {
				continue;
			}
			const unsigned i = (count - k) % BB_RING;
			if (armTicksToNs(now - s_bbTick[i]) <= (u64) ms * 1000000 && at > 0 && (size_t) at < sizeof(line) - 140) {
				at += snprintf(line + at, sizeof(line) - at, " %s", which == 0 ? s_bbEmu[i] : which == 1 ? s_bbExtra[i] : s_bbSched[i]);
			}
		}
		puts(line);
		if (s_bbExtraHandle == 0) {
			break;
		}
	}
}
static void blackbox_main(void* arg) {
	(void) arg;
	static char worker[BB_KEEP][136];
	unsigned n = 0;
	while (!s_profStop) {
		svcSleepThread(10000000);
		const unsigned count = atomic_load(&s_bbCount);
		prof_sample(s_profTarget, s_bbEmu[count % BB_RING], sizeof(s_bbEmu[0]));
		if (s_bbExtraHandle == 0 || !prof_sample(s_bbExtraHandle, s_bbExtra[count % BB_RING], sizeof(s_bbExtra[0]))) {
			strcpy(s_bbExtra[count % BB_RING], "-");
		}
		const Handle sched = s_bbSchedHandle;
		if (sched == 0 || sched == s_bbExtraHandle || !prof_sample(sched, s_bbSched[count % BB_RING], sizeof(s_bbSched[0]))) {
			strcpy(s_bbSched[count % BB_RING], "-");
		}
		s_bbTick[count % BB_RING] = armGetSystemTick();
		atomic_store(&s_bbCount, count + 1);
		n++;
		if (n % 4 == 0 && !prof_sample(hen_worker_handle, worker[(n / 4) % BB_KEEP], sizeof(worker[0]))) {
			worker[(n / 4) % BB_KEEP][0] = 0;
		}
		if (n % 100 == 0) {
			static char line[2000];
			int at = snprintf(line, sizeof(line), "hen: bb: E");
			for (unsigned k = BB_KEEP; k > 0; k--) {
				at += snprintf(line + at, sizeof(line) - at, " %s", s_bbEmu[(count + 1 + BB_RING - k * 4) % BB_RING]);
			}
			at += snprintf(line + at, sizeof(line) - at, " W");
			for (unsigned i = 1; i <= BB_KEEP; i++) {
				const char* w = worker[(n / 4 + i) % BB_KEEP];
				at += snprintf(line + at, sizeof(line) - at, " %s", w[0] ? w : "-");
			}
			puts(line);
		}
	}
}
static void prof_main(void* arg) {
	(void) arg;
	if (!hen_profile) {
		blackbox_main(arg);
		return;
	}
	while (!s_profStop) {
		svcSleepThread(2000000);
		if (R_FAILED(svcSetThreadActivity(s_profTarget, ThreadActivity_Paused))) {
			s_profFailed++;
			continue;
		}
		ThreadContext ctx;
		const Result rc = svcGetThreadContext3(&ctx, s_profTarget);
		svcSetThreadActivity(s_profTarget, ThreadActivity_Runnable);
		if (R_FAILED(rc)) {
			s_profFailed++;
			continue;
		}
		const uintptr_t pc = ctx.pc.x;
		mutexLock(&s_profLock);
		s_profSamples++;
		if (pc - (uintptr_t) _start < ((uintptr_t) 256 << 20)) {
			prof_add(s_profHost, (pc - (uintptr_t) _start) >> 6);
		} else {
			bool jit = false;
			for (unsigned i = 0; i < s_profRangeCount; i++) {
				jit = jit || pc - s_profRanges[i].start < s_profRanges[i].size;
				// (the first page of a code buffer is the translator's own entry, exit and block
				// lookup code: time there is time between blocks, not in them)
				if (pc - s_profRanges[i].start < 0x1000) {
					s_profPrelude++;
				}
				if (pc - s_profRanges[i].start < s_profRanges[i].size) {
					prof_add(s_profCode, pc >> 6); // (which generated code: dumped by the report)
					if (pc - s_profRanges[i].start >= 8 && (pc & 3) == 0) {
						s_profKindAt[prof_kind(*(const u32*) pc)]++;
						s_profKindBefore[prof_kind(*(const u32*) (pc - 4))]++;
					}
				}
			}
			if (jit) {
				s_profJit++;
				// (x28 is the state pointer only once the entry code has set it: check that the
				// address is readable memory before following it. Not doing so is the likely
				// cause of a crash 100 s into a profiled run.)
				const uintptr_t state = ctx.cpu_gprs[28].x;
				MemoryInfo info;
				u32 page_info;
				if (state != 0 && (state & 3) == 0 && R_SUCCEEDED(svcQueryMemory(&info, &page_info, state)) &&
				    (info.perm & Perm_R) && info.type == MemType_Heap && state + 256 < info.addr + info.size) {
					const u32* regs = (const u32*) (state + hen_a32_regs_offset);
					prof_add(s_profGuest, regs[15]);
				}
			} else {
				s_profOther++;
				// (system code: a service call or the kernel's wait. By who called it.)
				if (ctx.lr - (uintptr_t) _start < ((uintptr_t) 256 << 20)) {
					prof_add(s_profHost, ((ctx.lr - (uintptr_t) _start) >> 6) | (1ull << 40));
				}
			}
		}
		mutexUnlock(&s_profLock);
	}
}
// From the emulation thread, once it runs the program.
// --profile-pipelines (hen_profile 2): the profile is of the first pipeline worker that
// compiles something, not of the emulation thread (what the driver's compiler spends its time on).
void hen_jit_profile_retarget(void) {
	static bool done;
	if (hen_profile == 2 && !done && s_profTarget != 0) {
		done = true;
		s_profTarget = threadGetCurHandle();
		printf("hen: profile: now of a pipeline worker\n");
	}
}

void hen_jit_profile_start(void) {
	if ((!hen_profile && !hen_blackbox) || s_profStarted) {
		return;
	}
	s_profTarget = threadGetCurHandle();
	s32 priority = 59;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	const int here = (int) svcGetCurrentProcessorNumber();
	const Result rc = threadCreate(&s_profThread, prof_main, NULL, NULL, 0x8000, priority - 1, here == 5 ? 4 : 5);
	if (R_SUCCEEDED(rc) && R_SUCCEEDED(threadStart(&s_profThread))) {
		s_profStarted = true;
	}
	printf("hen: %s: sampling thread %s (rc 0x%x)\n", hen_profile ? "profile" : "black box", s_profStarted ? "started" : "not started", rc);
}
void hen_jit_profile_stop(void) {
	if (s_profStarted) {
		s_profStop = true;
		threadWaitForExit(&s_profThread);
		threadClose(&s_profThread);
		s_profStarted = false;
	}
}
static int prof_cmp(const void* a, const void* b) {
	return (int) ((const u32*) b)[2] - (int) ((const u32*) a)[2];
}
unsigned hen_prof_top_guest[8];  // the last report's most sampled block addresses (main.cpp prints their code)
void hen_jit_profile_report(void) {
	if (!s_profStarted || !hen_profile) {
		return;
	}
	static struct { u64 key; u32 count; } host[PROF_SLOTS], guest[PROF_SLOTS];
	mutexLock(&s_profLock);
	memcpy(host, s_profHost, sizeof(host));
	memcpy(guest, s_profGuest, sizeof(guest));
	memset(s_profHost, 0, sizeof(s_profHost));
	memset(s_profGuest, 0, sizeof(s_profGuest));
	const unsigned samples = atomic_exchange(&s_profSamples, 0), jit = atomic_exchange(&s_profJit, 0);
	const unsigned other = atomic_exchange(&s_profOther, 0), failed = atomic_exchange(&s_profFailed, 0);
	mutexUnlock(&s_profLock);
	qsort(host, PROF_SLOTS, sizeof(host[0]), prof_cmp);
	qsort(guest, PROF_SLOTS, sizeof(guest[0]), prof_cmp);
	printf("hen: profile: %u samples: %u in generated code (%u of them in its entry and lookup code), %u in system code, %u in this program; %u failed\n", samples, jit,
	       atomic_exchange(&s_profPrelude, 0), other, samples - jit - other, failed);
	printf("hen: profile: host");
	for (int i = 0; i < 28 && host[i].count != 0; i++) {
		printf(" %s%llx:%u", (host[i].key >> 40) ? "via+" : "+", (unsigned long long) ((host[i].key & 0xffffffffffull) << 6), host[i].count);
	}
	printf("\nhen: profile: guest");
	for (int i = 0; i < 40 && guest[i].count != 0; i++) {
		printf(" %08llx:%u", (unsigned long long) guest[i].key, guest[i].count);
	}
	for (int i = 0; i < 8; i++) {
		hen_prof_top_guest[i] = guest[i].count != 0 ? (unsigned) guest[i].key : 0;
	}
	if (jit >= 50) {
		printf("\nhen: profile: kinds (at/before):");
		for (int k = 0; k < PK_COUNT; k++) {
			printf(" %s %u/%u", s_profKindName[k], atomic_exchange(&s_profKindAt[k], 0), atomic_exchange(&s_profKindBefore[k], 0));
		}
	}
	// The generated code sampled most often, as instruction words (for a disassembler on the
	// build machine): what the translator makes of the program's hottest code.
	{
		static struct { u64 key; u32 count; } code[PROF_SLOTS];
		mutexLock(&s_profLock);
		memcpy(code, s_profCode, sizeof(code));
		memset(s_profCode, 0, sizeof(s_profCode));
		mutexUnlock(&s_profLock);
		qsort(code, PROF_SLOTS, sizeof(code[0]), prof_cmp);
		for (int i = 0; i < 4 && code[i].count * 50 >= jit && code[i].count >= 20; i++) {
			const uintptr_t at = (uintptr_t) (code[i].key << 6);
			bool ok = false;
			for (unsigned r = 0; r < s_profRangeCount; r++) {
				ok = ok || (at - 96 >= s_profRanges[r].start && at + 160 - s_profRanges[r].start <= s_profRanges[r].size);
			}
			MemoryInfo info;
			u32 page_info;
			if (!ok || R_FAILED(svcQueryMemory(&info, &page_info, at - 96)) || !(info.perm & Perm_R) ||
			    at + 160 > info.addr + info.size) {
				continue;
			}
			printf("\nhen: profile: hostcode %u samples at %lx:", code[i].count, (unsigned long) at);
			for (const u32* w = (const u32*) (at - 96); w < (const u32*) (at + 160); w++) {
				printf(" %08x", *w);
			}
		}
	}
	printf("\n");
}
#endif // HEN_DEV

// ---- The address range of a buffer's code ---------------------------------
// Who keeps other things out of it. The range is chosen when the buffer is made (below): the
// library draws an address at random, asks the kernel whether everything from there to the end
// is unmapped at that moment, and notes the range in a list of its own (the "reservation").
// That list is only read by the library's own finders (thread stacks, shared and transfer
// memory, other code ranges). The kernel knows nothing of the range until something is mapped
// there, and what the kernel places by itself goes where the kernel likes: a thread-local
// page (memory type 0xc, made when a thread is created) can lie in the range by the time the
// buffer's first code is mapped (seen 21 MiB into Xenoblade's, about as many threads later as
// a title's start creates). The mapping of the whole buffer is refused for that one page.
//
// What can be done depends on whether code for the range exists yet:
//   - A re-sort's second buffer has none until it is mapped (hen_jit_resort_prepare): it gets
//     another range and the mapping is tried again (move_range).
//   - A JIT's first buffer has: the JIT is given the address when the buffer is made and writes
//     its prelude and its tables for it at once, long before the first mapping. Its range
//     cannot be exchanged here. The part below the occupied spot is mapped instead and the JIT
//     is told the buffer ends there (try_alias); the growth of a buffer and the re-sort then
//     move the code to a new range before that end is reached, as they do for a small buffer.
// Mapping every buffer when it is made would close the gap altogether (a mapped range is the
// kernel's own reservation), and is not done: a JIT is made for every core a process may run
// on, and those that never compile a block would each hold a heap block of their size (what
// WRITE_FIRST avoids for the write areas).

// Under the library's lock: a range for j->size bytes of code that is unmapped now, and its
// reservation. 0: found (j->exec, j->reservation); 1: no range; 2: no reservation.
static int choose_range(struct hen_jit* j) {
	// (virtmemFindCodeMemory's second argument is a guard size, it has no alignment: a
	// free range one alignment longer always holds an aligned one of the size wanted)
	u8* exec = virtmemFindCodeMemory(j->size + (j->align - PAGE), PAGE);
	if (exec != NULL) {
		exec = (u8*) (((uintptr_t) exec + j->align - 1) & ~(uintptr_t) (j->align - 1));
	} else if (j->align != PAGE) {
		printf("hen_jit: no address range for %zu MiB of code on a 2 MiB boundary: using pages\n", j->size >> 20);
		j->align = PAGE;
		exec = virtmemFindCodeMemory(j->size, PAGE);
	}
	if (exec == NULL) {
		return 1;
	}
	VirtmemReservation* const reservation = virtmemAddReservation(exec, j->size);
	if (reservation == NULL) {
		return 2;
	}
	j->exec = exec;
	j->reservation = reservation;
	return 0;
}

// Another range for a buffer that has no code yet and nothing mapped. The old reservation is
// kept until the new range is found, so the two cannot overlap. False: none was found, and the
// buffer is as it was.
static bool move_range(struct hen_jit* j) {
	u8* const old = j->exec;
	VirtmemReservation* const oldReservation = j->reservation;
	virtmemLock();
	const bool moved = choose_range(j) == 0;
	if (moved) {
		virtmemRemoveReservation(oldReservation);
	}
	virtmemUnlock();
#if HEN_DEV
	for (unsigned i = 0; moved && i < s_profRangeCount; i++) {
		if (s_profRanges[i].start == (uintptr_t) old) {
			s_profRanges[i].start = (uintptr_t) j->exec;  // (the profiler's list of code ranges)
		}
	}
#else
	(void) old;
#endif
	return moved;
}

struct hen_jit* hen_jit_create(size_t size) {
	static unsigned nextId;
	struct hen_jit* j = calloc(1, sizeof(*j));
	if (j == NULL) {
		return NULL;
	}
	j->small = size < SMALL;
	j->size = j->small ? (size + PAGE - 1) & ~(PAGE - 1) : (size + UNIT - 1) & ~(UNIT - 1);
	j->align = PAGE;
	if (hen_code_align_2m && !j->small) {
		j->align = BLOCK_2M;
		j->size = (j->size + BLOCK_2M - 1) & ~(BLOCK_2M - 1);
	}
	j->usable = j->size;
	j->unit = j->small ? j->size : UNIT;
	j->writing = true;
	j->writeFull = j->small || !hen_jit_lazy_write;
	j->write = malloc(j->writeFull ? j->size : WRITE_FIRST);
	int range = 1;
	if (j->write != NULL) {
		virtmemLock();
		range = choose_range(j);
		virtmemUnlock();
#if HEN_DEV
		if (range == 0 && s_profRangeCount < 16) {
			s_profRanges[s_profRangeCount].start = (uintptr_t) j->exec;
			s_profRanges[s_profRangeCount].size = j->size;
			s_profRangeCount++;
		}
#endif
	}
	if (range != 0) {
		printf("hen_jit: no %s for %zu KiB of code\n",
		       j->write == NULL ? "heap memory" : range == 1 ? "address range" : "address reservation", size >> 10);
		free(j->write);
		free(j);
		return NULL;
	}
	mutexLock(&s_lock);
	j->id = nextId++;
	for (unsigned i = 0; i < MAX_LIVE; i++) {
		if (s_live[i] == NULL) {
			s_live[i] = j;
			break;
		}
	}
	mutexUnlock(&s_lock);
	return j;
}

// Heap blocks the kernel refused as a code source although nothing we can read says why: kept
// allocated until the program exits, so that the next attempt gets other memory (freed, the
// allocator hands the same pages back).
#define MAX_REFUSED 16
static u8* s_refused[MAX_REFUSED];
static unsigned s_refusedCount;
static unsigned long long s_refusedTotal;  // (blocks ever set aside)
#if HEN_DEV
int hen_jit_refuse_test = 0;  // --jit-refuse-test MIB (refuse_test, below)
static struct {
	bool done;
	u8* at;
	u8* page;
} s_refuseTest[2];
#endif

// What the code buffers hold of the heap, for a line in the log: [0] buffers alive (of
// 1 MiB and more), [1] those with code mapped, [2] bytes of heap taken for them (write areas
// and the blocks code is mapped from), [3] bytes of code written at most (the peaks, summed),
// [4] heap blocks set aside because a mapping from them was refused.
void hen_jit_memory_stats(unsigned long long* out) {
	out[0] = out[1] = out[2] = out[3] = out[4] = 0;
	mutexLock(&s_lock);
	for (unsigned i = 0; i < MAX_LIVE; i++) {
		const struct hen_jit* j = s_live[i];
		if (j == NULL || j->size < ((size_t) 1 << 20)) {
			continue;
		}
		out[0]++;
		out[1] += j->mappedCount != 0;
		out[2] += (j->write != NULL ? (j->writeFull ? j->size : WRITE_FIRST) : 0) +
		          (j->sourceBlock != NULL ? j->size + (j->align != PAGE ? j->align : 0) : 0);
		out[3] += j->peak;
	}
	out[4] = s_refusedCount;
	mutexUnlock(&s_lock);
	out[5] = heapGetTotalSize();
	out[6] = heapGetFreeSize();
}

// How many more code-buffer blocks the allocator could give now: found by taking blocks
// of that size until one is refused and giving them all back. The allocator's free figure does
// not say this: a block over 32 MiB is only ever put in 32 MiB units that are wholly free,
// starting on a unit's boundary (the SDK's allocator is mimalloc),
// so free memory spread over units that each hold something small is of no use to it.
// Development builds, once per report; nothing is written to the blocks.
unsigned hen_jit_blocks_that_fit(size_t size) {
	void* held[80];
	unsigned count = 0;
	while (count < 80 && (held[count] = malloc(size)) != NULL) {
		count++;
	}
	for (unsigned i = 0; i < count; i++) {
		free(held[i]);
	}
	return count;
}

// No heap block for a code buffer: said in words, and left on the card (crash.txt, the
// session table's last line) before the program ends.
static void out_of_memory(const struct hen_jit* j, const char* what) {
	static char text[320];
	unsigned alive = 0, with_code = 0;
	for (unsigned i = 0; i < MAX_LIVE; i++) {
		if (s_live[i] != NULL && s_live[i]->size >= ((size_t) 1 << 20)) {
			alive++;
			with_code += s_live[i]->mappedCount != 0;
		}
	}
	snprintf(text, sizeof(text),
	         "out of memory for a process's code buffer: no heap block of %zu MiB for %s of buffer %u; %u code "
	         "buffers alive, %u with code; the allocator counts %llu MiB free of %llu MiB, but not in one piece",
	         j->size >> 20, what, j->id, alive, with_code, (unsigned long long) (heapGetFreeSize() >> 20),
	         (unsigned long long) (heapGetTotalSize() >> 20));
	printf("hen: %s: ending the program\n", text);
	fflush(stdout);
	if (sw_fatal_record != NULL) {
		sw_fatal_record(text);
	}
	abort();
}

// The whole write area, for a buffer made with the short one: 1 if it was made now (the
// caller's pointers into the old one are stale: hen_jit_wptr again), 0 if it was whole already,
// -1 if the heap has no block (fatal: the program ends with out_of_memory's lines). Only the
// thread that compiles into the buffer calls this, before its first block.
static int write_full(struct hen_jit* j, bool fatal) {
	if (j->writeFull) {
		return 0;
	}
	u8* const full = malloc(j->size);
	if (full == NULL) {
		if (fatal) {
			out_of_memory(j, "the write area");
		}
		return -1;
	}
	mutexLock(&s_lock);
	u8* const first = j->write;
	memcpy(full, first, WRITE_FIRST);
	j->write = full;
	if (j->alias) {
		j->lib.rw_addr = full;
	}
	j->writeFull = true;
	mutexUnlock(&s_lock);
	free(first);
	return 1;
}
int hen_jit_write_full(struct hen_jit* j) { return write_full(j, true); }
size_t hen_jit_write_first_size(void) { return WRITE_FIRST; }

// At exit: unmaps what is still mapped (a buffer nobody destroyed: dynarmic keeps one in a
// static object for its spin lock, whose destructor doesn't run here). The next program
// runs in this same process, and a mapping left behind keeps its heap pages locked for it.
// Returns how many mappings it released.
unsigned hen_jit_release_all(void) {
	unsigned released = 0;
	mutexLock(&s_lock);
	for (unsigned i = 0; i < MAX_LIVE; i++) {
		struct hen_jit* j = s_live[i];
		if (j == NULL) {
			continue;
		}
		for (unsigned m = 0; m < j->mappedCount; m++) {
			if (R_SUCCEEDED(__nx_jit_unmap_code(j->exec + j->mapped[m].begin, j->source + j->mapped[m].begin,
			                                    j->mapped[m].size))) {
				s_mappings--;
				released++;
			}
		}
		j->mappedCount = 0;
	}
	// (heap blocks set aside after a refused mapping: back to the allocator now)
	for (unsigned i = 0; i < s_refusedCount; i++) {
		free(s_refused[i]);
		s_refused[i] = NULL;
	}
	s_refusedCount = 0;
#if HEN_DEV
	// (--jit-refuse-test: its pages, or their heap stays locked for the next program)
	for (unsigned i = 0; i < 2; i++) {
		if (s_refuseTest[i].page != NULL && R_SUCCEEDED(__nx_jit_unmap_code(s_refuseTest[i].at, s_refuseTest[i].page, PAGE))) {
			free(s_refuseTest[i].page);
		}
		s_refuseTest[i].page = NULL;
	}
#endif
	mutexUnlock(&s_lock);
	return released;
}

// At exit, after everything above: how much of the heap still carries an attribute or a
// reference from a request or a device (the GPU service's hold on the driver's memory). The
// next program runs on the same heap, and such pages cannot be a code source there.
// out: bytes with an attribute, with an ipc reference, with a device reference; blocks.
void hen_jit_heap_left(unsigned long long* out) {
	out[0] = out[1] = out[2] = out[3] = 0;
	u64 heapStart = 0, heapSize = 0;
	if (R_FAILED(svcGetInfo(&heapStart, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
	    R_FAILED(svcGetInfo(&heapSize, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0))) {
		return;
	}
	u64 at = heapStart;
	const u64 end = heapStart + heapSize;
	for (unsigned blocks = 0; at < end && blocks < 200000; blocks++) {
		MemoryInfo info;
		u32 page;
		if (R_FAILED(svcQueryMemory(&info, &page, at)) || info.size == 0) {
			break;
		}
		const u64 stop = info.addr + info.size < end ? info.addr + info.size : end;
		if ((info.type & MemState_Type) == MemType_Heap &&
		    (info.attr != 0 || info.ipc_refcount != 0 || info.device_refcount != 0)) {
			out[0] += info.attr != 0 ? stop - at : 0;
			out[1] += info.ipc_refcount != 0 ? stop - at : 0;
			out[2] += info.device_refcount != 0 ? stop - at : 0;
			out[3]++;
		}
		at = stop;
	}
}

void hen_jit_destroy(struct hen_jit* j) {
	if (j == NULL) {
		return;
	}
	mutexLock(&s_lock);
	for (unsigned i = 0; i < MAX_LIVE; i++) {
		if (s_live[i] == j) {
			s_live[i] = NULL;
		}
	}
	const unsigned mapped = j->mappedCount;
	bool leaked = false;
	for (unsigned i = 0; i < j->mappedCount; i++) {
		const Result rc =
		    __nx_jit_unmap_code(j->exec + j->mapped[i].begin, j->source + j->mapped[i].begin, j->mapped[i].size);
		if (R_FAILED(rc)) {
			// Its source pages stay locked: they must not go back to the allocator.
			printf("hen_jit: buffer %u: unmapping %zu KiB at %zu KiB failed: 0x%x\n", j->id, j->mapped[i].size >> 10,
			       j->mapped[i].begin >> 10, rc);
			leaked = true;
			continue;
		}
		s_mappings--;
	}
	mutexUnlock(&s_lock);
	if (leaked) {
		j->sourceBlock = NULL;  // (left allocated)
	}
	if (j->size >= ((size_t) 1 << 20)) {
		printf("hen_jit: buffer %u (%zu MiB) closed: used up to %zu KiB, %u piece(s) mapped, unit %zu KiB\n", j->id,
		       j->size >> 20, j->peak >> 10, mapped, j->unit >> 10);
	}
	virtmemLock();
	virtmemRemoveReservation(j->reservation);
	virtmemUnlock();
	free(j->sourceBlock);
	free(j->write);
	free(j);
}

uint32_t* hen_jit_wptr(struct hen_jit* j) { return (uint32_t*) j->write; }

uint32_t* hen_jit_xptr(struct hen_jit* j) { return (uint32_t*) j->exec; }

// How many bytes from the start of the buffer the JIT may fill: all of it (a
// buffer merges its pieces when the mappings run short), or as far as its address range was
// free when its code was mapped (try_alias). dynarmic reads this whenever it asks for the room
// left (AddressSpace::GetRemainingSize), not once.
size_t hen_jit_usable_size(struct hen_jit* j) { return j->usable; }

// The JIT starts over at offset (it threw its code away): the code ends there
// again, and the pieces beyond are given back by the next update.
void hen_jit_rewind(struct hen_jit* j, size_t offset) {
	mutexLock(&s_lock);
	if (offset < j->highWater) {
		j->highWater = offset;
	}
	mutexUnlock(&s_lock);
}

static void fail(const struct hen_jit* j, const char* what, size_t at, Result rc) {
	printf("hen_jit: %s at %zu KiB of buffer %u failed: 0x%x (%u of %d mappings in use)\n", what, at >> 10, j->id, rc,
	       s_mappings, MAX_MAPPINGS);
	abort();
}

// The pieces for code that ends at j->highWater, with the given unit: the
// last unit alone, the units before it one piece per set bit of their count.
static unsigned pieces_for(const struct hen_jit* j, size_t unit, struct piece* out) {
	size_t end = (j->highWater + unit - 1) / unit * unit;
	if (end > j->size) {
		end = j->size;
	}
	if (end == 0) {
		return 0;
	}
	const size_t units = (end + unit - 1) / unit;  // (the last may be cut short by the buffer's end)
	unsigned count = 0;
	size_t at = 0;
	const size_t before = units - 1;
	for (int bit = 62; bit >= 0; bit--) {
		if (before >> bit & 1) {
			out[count].begin = at * unit;
			out[count].size = ((size_t) 1 << bit) * unit;
			at += (size_t) 1 << bit;
			if (++count == MAX_MAPPINGS) {
				return MAX_MAPPINGS + 1;  // (too many: the caller doubles the unit)
			}
		}
	}
	out[count].begin = at * unit;
	out[count].size = end - at * unit;
	return count + 1;
}

static bool is_dirty(const struct hen_jit* j, const struct piece* p) {
	for (size_t r = 0; r < j->dirtyCount; r++) {
		if (j->dirty[r].begin < p->begin + p->size && j->dirty[r].end > p->begin) {
			return true;
		}
	}
	return false;
}

// How much of a heap block can't be the source of a code mapping: pages that
// aren't plain read-write memory without attributes.
static size_t locked_bytes(const u8* block, size_t size) {
	size_t locked = 0;
	uintptr_t at = (uintptr_t) block;
	const uintptr_t end = at + size;
	while (at < end) {
		MemoryInfo info;
		u32 page;
		if (R_FAILED(svcQueryMemory(&info, &page, at)) || info.size == 0) {
			break;  // (no answer: assume it's fine, the mapping will tell)
		}
		const uintptr_t stop = info.addr + info.size < end ? (uintptr_t) (info.addr + info.size) : end;
		// (the two counts as well. Memory an in-flight request or a device, the GPU
		// service through the driver, still refers to cannot be a code source either, and the
		// attribute need not say so.)
		if (info.attr != 0 || (info.perm & Perm_Rw) != Perm_Rw || info.ipc_refcount != 0 || info.device_refcount != 0) {
			locked += stop - at;
		}
		at = stop;
	}
	return locked;
}

// A code mapping was refused (the kernel's 0xd401, "the
// memory is not in the state this call needs", which it gives for a source that is not plain
// heap and for a destination that is not free alike). What the kernel says about both ranges,
// so that the log names the cause: one line in every build, and in development builds every
// block that is not what the call needs. What it found is left in *out (if not NULL).
struct refusal {
	bool destFree;     // nothing is mapped in the destination range
	bool sourcePlain;  // the source is ordinary heap memory by everything the kernel shows
	size_t destAt;     // not free: where the first thing in it begins, from the buffer's start
	char dest[160];    // what the kernel says of that first thing ("free" if nothing)
};
static void report_refusal(const struct hen_jit* j, const u8* source, size_t begin, size_t size, Result rc,
                           struct refusal* out) {
	char dest[160] = "free", src[160] = "plain heap";
	bool destFree = true;
	size_t destAt = 0;
	unsigned destBlocks = 0, srcBlocks = 0;
	for (int which = 0; which < 2; which++) {
		uintptr_t at = (uintptr_t) (which == 0 ? j->exec : source) + begin;
		const uintptr_t end = at + size;
		while (at < end) {
			MemoryInfo info;
			u32 page;
			if (R_FAILED(svcQueryMemory(&info, &page, at)) || info.size == 0) {
				snprintf(which == 0 ? dest : src, sizeof(dest), "no answer from the kernel at %p", (void*) at);
				break;
			}
			const uintptr_t stop = info.addr + info.size < end ? (uintptr_t) (info.addr + info.size) : end;
			const bool bad = which == 0 ? (info.type & MemState_Type) != MemType_Unmapped
			                            : (info.attr != 0 || (info.perm & Perm_Rw) != Perm_Rw || info.ipc_refcount != 0 ||
			                               info.device_refcount != 0 || (info.type & MemState_Type) != MemType_Heap);
			if (bad) {
				unsigned* const count = which == 0 ? &destBlocks : &srcBlocks;
				if (*count == 0) {
					snprintf(which == 0 ? dest : src, sizeof(dest),
					         "%zu KiB at %p (+%zu KiB) are type 0x%x, permission %u, attribute 0x%x, ipc %u, device %u",
					         (size_t) (stop - at) >> 10, (void*) at,
					         (size_t) (at - (uintptr_t) (which == 0 ? j->exec : source)) >> 10,
					         (unsigned) (info.type & MemState_Type), (unsigned) info.perm, (unsigned) info.attr,
					         (unsigned) info.ipc_refcount, (unsigned) info.device_refcount);
				}
#if HEN_DEV
				else if (*count < 12) {
					printf("hen_jit: refused: %s also: %zu KiB at %p are type 0x%x, permission %u, attribute 0x%x, ipc %u, device %u\n",
					       which == 0 ? "destination" : "source", (size_t) (stop - at) >> 10, (void*) at,
					       (unsigned) (info.type & MemState_Type), (unsigned) info.perm, (unsigned) info.attr,
					       (unsigned) info.ipc_refcount, (unsigned) info.device_refcount);
				}
#endif
				if (which == 0 && destFree) {
					destFree = false;
					destAt = at - (uintptr_t) j->exec;
				}
				(*count)++;
			}
			at = stop;
		}
	}
	printf("hen_jit: refused: buffer %u, %zu KiB at %zu KiB, result 0x%x; destination %p: %s (%u block(s)); source %p: %s (%u block(s))\n",
	       j->id, size >> 10, begin >> 10, rc, (void*) (j->exec + begin), dest, destBlocks, (void*) (source + begin),
	       src, srcBlocks);
	if (out != NULL) {
		out->destFree = destFree;
		out->sourcePlain = srcBlocks == 0;
		out->destAt = destAt;
		memcpy(out->dest, dest, sizeof(dest));
	}
}

#if HEN_DEV
// Where the kernel has put the thread-local pages so far, next to where the library
// chooses code ranges: one line at the first buffer's mapping. A page of that type in a code
// range makes the kernel refuse the mapping; whether such pages lie anywhere in the
// address space or keep to a part of it that a code range could stay out of is not known,
// and this line is there to tell. The heap is stepped over (it has the most blocks and none
// of these).
static void thread_local_line(void) {
	static bool done;
	if (done) {
		return;
	}
	done = true;
	u64 space = 0, spaceSize = 0, heap = 0, heapSize = 0;
	if (R_FAILED(svcGetInfo(&space, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
	    R_FAILED(svcGetInfo(&spaceSize, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0))) {
		return;
	}
	svcGetInfo(&heap, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&heapSize, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
	const u64 t0 = armGetSystemTick();
	u64 at = space, lowest = 0, highest = 0;
	unsigned pages = 0, asked = 0;
	while (at < space + spaceSize && asked < 20000) {
		if (at >= heap && at < heap + heapSize) {
			at = heap + heapSize;
			continue;
		}
		MemoryInfo info;
		u32 page;
		if (R_FAILED(svcQueryMemory(&info, &page, at)) || info.size == 0 || info.addr + info.size <= at) {
			break;
		}
		asked++;
		if ((info.type & MemState_Type) == MemType_ThreadLocal) {
			if (pages == 0) {
				lowest = info.addr;
			}
			highest = info.addr + info.size;
			pages += (unsigned) (info.size >> 12);
		}
		at = info.addr + info.size;
	}
	printf("hen_jit: thread-local pages: %u so far, from %p to %p (%llu MiB apart); this thread's block is at %p; code ranges "
	       "are chosen from %p to %p outside the heap (%p to %p); the walk asked the kernel %u times in %.2f ms%s\n",
	       pages, (void*) lowest, (void*) highest, (unsigned long long) ((highest - lowest) >> 20), armGetTls(), (void*) space,
	       (void*) (space + spaceSize), (void*) heap, (void*) (heap + heapSize), asked,
	       armTicksToNs(armGetSystemTick() - t0) / 1e6, asked == 20000 ? " (cut short)" : "");
}

// (--jit-refuse-test MIB: a test's trigger) What otherwise happens by chance, made
// to happen: one page of heap is mapped as code inside a buffer's address range just before
// the buffer's own mapping, which the kernel then refuses. Once for the first JIT buffer that
// gets code (MIB into its range, so the buffer goes on with that much) and once for the first
// second buffer a re-sort or a growth makes (in its middle: it must move to another range).
// The two pages are unmapped at exit (hen_jit_release_all).
static void refuse_test(const struct hen_jit* j, int which, size_t offset) {
	if (hen_jit_refuse_test <= 0 || s_refuseTest[which].done) {
		return;
	}
	s_refuseTest[which].done = true;
	u8* const page = memalign(PAGE, PAGE);
	if (page == NULL) {
		return;
	}
	// (three pages past the offset asked for: the spot is not on any boundary, as it was not then)
	u8* const at = j->exec + (offset < j->size - 4 * PAGE ? offset : j->size / 2) + 3 * PAGE;
	const Result rc = __nx_jit_map_code(at, page, PAGE);
	printf("hen_jit: --jit-refuse-test: one page mapped at %p, %zu KiB into the address range of buffer %u (%s), before "
	       "its code is mapped (result 0x%x)\n",
	       (void*) at, (size_t) (at - j->exec) >> 10, j->id, which == 0 ? "a JIT's first" : "a re-sort's second", rc);
	if (R_FAILED(rc)) {
		free(page);
		return;
	}
	s_refuseTest[which].at = at;
	s_refuseTest[which].page = page;
}
#endif

// Under the lock: the heap memory the code is mapped from.
static void alloc_source(struct hen_jit* j) {
	// A program that ended without unmapping its code (a crash, a kill) leaves
	// those heap pages locked for whatever runs next in this
	// process, and a mapping from them is refused. Such a block is kept
	// allocated, so nothing else gets it either, and another is taken.
	for (int attempt = 0; attempt < 8 && j->source == NULL; attempt++) {
		// On a 2 MiB boundary (hen_code_align_2m): a block one alignment longer, and the
		// aligned part of it. Not memalign(2 MiB): the SDK's allocator has
		// a path of its own for alignments over 64 KiB, and that returned nothing for the
		// first 128 MiB buffer (the program ended at its first compiled block).
		// If the longer block can't be had either, the buffer is as without the switch.
		u8* raw = j->align != PAGE ? malloc(j->size + j->align) : NULL;
		u8* block = raw != NULL ? (u8*) (((uintptr_t) raw + j->align - 1) & ~(uintptr_t) (j->align - 1)) : NULL;
		if (raw == NULL) {
			if (j->align != PAGE) {
				printf("hen_jit: buffer %u: no heap block for %zu MiB on a 2 MiB boundary: using pages\n", j->id,
				       j->size >> 20);
				j->align = PAGE;
			}
			raw = block = memalign(PAGE, j->size);
		}
		if (block == NULL) {
			break;
		}
		const size_t locked = locked_bytes(block, j->size);
		if (locked == 0) {
			j->source = block;
			j->sourceBlock = raw;
			// (release builds too. Where the code is differs from run to run.)
			if (!j->small) {
				printf("hen_jit: buffer %u: %zu MiB of code at %p, from heap memory at %p (%s)\n", j->id, j->size >> 20,
				       (void*) j->exec, (void*) j->source,
				       (((uintptr_t) j->exec | (uintptr_t) j->source | j->size) & (BLOCK_2M - 1)) == 0
				           ? "both on 2 MiB boundaries"
				           : "not on 2 MiB boundaries");
			}
		} else {
			printf("hen_jit: buffer %u: %zu KiB of a %zu MiB heap block are still locked by an earlier program; taking another\n",
			       j->id, locked >> 10, j->size >> 20);
		}
	}
	if (j->source == NULL) {
		out_of_memory(j, "the code source");
	}
}

// Under the lock, for a buffer with nothing mapped yet: maps all of it and asks the HEN for
// a writable view. Leaves the buffer as it was if either is refused.
static void try_alias(struct hen_jit* j) {
	if (!hen_jit_alias || __nx_jit_map_alias == NULL ||
	    (kaihenJitPublishRanges == NULL && ouncehenJitPublishRanges == NULL)) {
		s_aliasState = -1;
		if (hen_translation_worker == 3) {
			hen_translation_worker = 0;
		}
		return;
	}
#if HEN_DEV
	refuse_test(j, 0, (size_t) hen_jit_refuse_test << 20);
#endif
	const u64 t0 = armGetSystemTick();
	size_t size = j->size;  // (what is mapped: less only below an occupied spot, see below)
	Result rc = __nx_jit_map_code(j->exec, j->source, j->size);
	// Refused: the log gets the kernel's view of both ranges, and while the
	// destination is free the fault can only be the source's, whatever the kernel shows of it:
	// the block is set aside, still allocated, and another part of the heap is tried. Up to
	// four more times: two on a 2 MiB boundary if the buffer has one, then on pages.
	for (int attempt = 0; R_FAILED(rc) && attempt < 4; attempt++) {
		struct refusal why;
		report_refusal(j, j->source, 0, j->size, rc, &why);
		if (!why.destFree) {
			// Something is in the address range (see "The address range of a
			// buffer's code" above) and the range cannot be exchanged: code for this address
			// exists already. The part below the first thing in it is mapped, on the buffer's
			// boundary, and the JIT is told that the buffer ends there. It then behaves as a
			// buffer of that size does: seven tenths full it grows into a new buffer at a new
			// range (HenResortTick; an ordinary re-sort moves it as well), and if neither comes
			// in time a full buffer is handled between two slices. Not if the room below
			// is under 4 MiB or the code written so far reaches within 2 MiB of it: then
			// the pieces, which end the program at that spot.
			const size_t below = why.destAt & ~((j->align != PAGE ? BLOCK_2M : UNIT) - 1);
			if (why.sourcePlain && below >= ((size_t) 4 << 20) && below >= j->highWater + BLOCK_2M) {
				rc = __nx_jit_map_code(j->exec, j->source, below);
				if (R_SUCCEEDED(rc)) {
					size = j->usable = below;
					printf("hen_jit: buffer %u: its address range %p to %p is occupied (%s): the %zu MiB below that are "
					       "mapped and the buffer ends there until its code moves to a new range (growth or re-sort)\n",
					       j->id, (void*) j->exec, (void*) (j->exec + j->size), why.dest, below >> 20);
				} else {
					report_refusal(j, j->source, 0, below, rc, NULL);
				}
			} else {
				printf("hen_jit: buffer %u: its address range %p to %p is occupied (%s) and too little lies below that "
				       "(%zu KiB; %zu KiB of code are written)\n",
				       j->id, (void*) j->exec, (void*) (j->exec + j->size), why.dest, why.destAt >> 10, j->highWater >> 10);
			}
			break;
		}
		if (j->align != PAGE && attempt >= 2) {
			printf("hen_jit: buffer %u: mapping from heap memory on a 2 MiB boundary was refused (0x%x): using pages\n", j->id, rc);
			j->align = PAGE;
		}
		if (s_refusedCount < MAX_REFUSED) {
			s_refused[s_refusedCount++] = j->sourceBlock;
			s_refusedTotal++;
			printf("hen_jit: buffer %u: the refused %zu MiB heap block at %p is set aside until the program exits; taking another\n",
			       j->id, j->size >> 20, (void*) j->source);
		} else {
			free(j->sourceBlock);  // (no room to keep more)
		}
		j->source = j->sourceBlock = NULL;
		alloc_source(j);
		rc = __nx_jit_map_code(j->exec, j->source, j->size);
		if (R_SUCCEEDED(rc)) {
			printf("hen_jit: buffer %u: mapped from the other heap block (attempt %d)\n", j->id, attempt + 2);
		}
	}
#if HEN_DEV
	thread_local_line();
#endif
	if (R_FAILED(rc)) {
		if (s_aliasState == 0) {
			printf("hen_jit: mapping a whole %zu MiB buffer was refused (0x%x): using pieces\n", j->size >> 20, rc);
			s_aliasState = -1;
		if (hen_translation_worker == 3) {
			hen_translation_worker = 0;
		}
		}
		return;
	}
	rc = __nx_jit_map_alias(j->exec, size);
	if (R_FAILED(rc)) {
		printf("hen_jit: the system gave no writable view of the code (0x%x): using pieces\n", rc);
		s_aliasState = -1;
		if (hen_translation_worker == 3) {
			hen_translation_worker = 0;
		}
		if (R_FAILED(__nx_jit_unmap_code(j->exec, j->source, size))) {
			fail(j, "unmapping", 0, rc);
		}
		j->usable = j->size;  // (pieces: each is mapped when the code reaches it)
		return;
	}
	if (s_aliasState == 0) {
		printf("hen_jit: code is updated through the system's writable view (mapping %zu MiB took %.2f ms)\n", size >> 20,
		       armTicksToNs(armGetSystemTick() - t0) / 1e6);
	}
	s_aliasState = 1;
	j->alias = true;
	j->lib.type = JitType_SetProcessMemoryPermission;
	j->lib.size = size;
	j->lib.src_addr = j->source;
	j->lib.rx_addr = j->exec;
	j->lib.rw_addr = j->write;
	j->mapped[0].begin = 0;
	j->mapped[0].size = size;
	j->mappedCount = 1;
	s_mappings++;
	atomic_fetch_add(&s_maps, 1);
}

int hen_jit_alias_active(void) { return s_aliasState == 1; }

// ---- The publish request, sent from here (hen_publish_aligned) ----------------------------
// The library's publish request, sent from two buffers kept for the whole run: the ranges
// (touching ones merged) and their bytes one after another. Then the instruction cache is
// invalidated for the ranges on the calling thread.
// What differs from the library's own call is where the two buffers are: it takes two heap
// blocks per call, and the two here are kept, each on a page boundary and a whole number of
// pages long, so that no page of theirs holds anything else.
// All of it runs under s_lock (sync), so one pair serves every buffer; an update too large
// for the pair goes through the library's call.
#define PUB_COMMAND 65015
#define PUB_PAYLOAD ((size_t) 1 << 20)  // (an update is 1.5 to 2 KiB on average; a larger one goes through the library)
#define PUB_RANGES_BYTES ((MAX_RANGES * sizeof(OuncehenJitRange) + PAGE - 1) & ~(PAGE - 1))
static OuncehenJitRange* s_pubRanges;
static u8* s_pubPayload;
static Service s_pubRo;  // (the session the program is handed: not ours to close)
static int s_pubOwn;     // 0 not set up yet, 1 in use, -1 not used (the switch, or not available)

static void publish_setup(void) {
	s_pubOwn = -1;
	// (only beside the library whose request this is: an older devkit's call may differ)
	if (!hen_publish_aligned || kaihenJitPublishRanges == NULL) {
		return;
	}
	const Handle session = smGetServiceOverride(smEncodeName("ldr:ro"));
	if (session == INVALID_HANDLE) {
		return;
	}
	s_pubRanges = memalign(PAGE, PUB_RANGES_BYTES);
	s_pubPayload = memalign(PAGE, PUB_PAYLOAD);
	if (s_pubRanges == NULL || s_pubPayload == NULL || ((uintptr_t) s_pubRanges & (PAGE - 1)) != 0 ||
	    ((uintptr_t) s_pubPayload & (PAGE - 1)) != 0) {
		printf("hen_jit: no page-aligned buffers for the publish request: the library's call is used\n");
		free(s_pubRanges);
		free(s_pubPayload);
		s_pubRanges = NULL;
		s_pubPayload = NULL;
		return;
	}
	serviceCreate(&s_pubRo, session);
	s_pubRo.own_handle = 0;
	s_pubOwn = 1;
#if HEN_DEV
	printf("hen_jit: publish requests are sent from here: ranges from %p (%zu KiB), bytes from %p (%zu KiB), both on page boundaries\n",
	       (void*) s_pubRanges, PUB_RANGES_BYTES >> 10, (void*) s_pubPayload, PUB_PAYLOAD >> 10);
#endif
}

// Under the lock. count ranges of j (at most MAX_RANGES, each inside the buffer and not empty:
// hen_jit_invalidate) from the write area to the execute range. Returns when the HEN has answered.
static Result publish_ranges(struct hen_jit* j, const OuncehenJitRange* ranges, size_t count) {
	if (s_pubOwn == 0) {
		publish_setup();
	}
	if (s_pubOwn != 1) {
		atomic_fetch_add_explicit(&s_pubPath[1], 1, memory_order_relaxed);
		return jit_publish_ranges(&j->lib, ranges, count);
	}
	size_t merged = 0, total = 0;
	for (size_t i = 0; i < count; i++) {
		if (merged != 0 && s_pubRanges[merged - 1].offset + s_pubRanges[merged - 1].size == ranges[i].offset) {
			s_pubRanges[merged - 1].size += ranges[i].size;
		} else {
			s_pubRanges[merged++] = ranges[i];
		}
		total += ranges[i].size;
	}
	if (total > PUB_PAYLOAD) {
		// (never cut in two: a range is published whole, in one request)
		atomic_fetch_add_explicit(&s_pubPath[1], 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&s_pubPath[2], 1, memory_order_relaxed);
		return jit_publish_ranges(&j->lib, ranges, count);
	}
	size_t at = 0;
	for (size_t i = 0; i < merged; i++) {
		memcpy(s_pubPayload + at, j->write + s_pubRanges[i].offset, s_pubRanges[i].size);
		at += s_pubRanges[i].size;
	}
	const struct {
		u64 pid_placeholder;
		u64 destination;
		u64 count;
	} in = {0, (u64) (uintptr_t) j->exec, merged};
	const Result rc = serviceDispatchIn(&s_pubRo, PUB_COMMAND, in,
	                                    .buffer_attrs = {
	                                        SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
	                                        SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
	                                    },
	                                    .buffers = {
	                                        {s_pubRanges, merged * sizeof(*s_pubRanges)},
	                                        {s_pubPayload, total},
	                                    },
	                                    .in_send_pid = true);
	if (R_FAILED(rc)) {
		// Refused from here: the library's call for this update and every later one. (Nothing
		// has been made visible yet, and publishing the same bytes again changes nothing.)
		printf("hen_jit: the publish request sent from here was refused (0x%x, %zu ranges, %zu bytes): the library's call is used from now on\n",
		       rc, merged, total);
		s_pubOwn = -1;
		atomic_fetch_add_explicit(&s_pubPath[1], 1, memory_order_relaxed);
		return jit_publish_ranges(&j->lib, ranges, count);
	}
	for (size_t i = 0; i < merged; i++) {
		armICacheInvalidate(j->exec + s_pubRanges[i].offset, s_pubRanges[i].size);
	}
	atomic_fetch_add_explicit(&s_pubPath[0], 1, memory_order_relaxed);
	return 0;
}

// Under the lock. Brings the execute range up to date with the write area
// for the dirty ranges.
static void sync(struct hen_jit* j) {
	if (j->dirtyCount == 0) {
		return;
	}
	const u64 t0 = armGetSystemTick();
	// How far back from the newest code this update reaches (a measurement).
	{
		size_t lowest = j->highWater;
		for (size_t r = 0; r < j->dirtyCount; r++) {
			if (j->dirty[r].begin < lowest) {
				lowest = j->dirty[r].begin;
			}
		}
		const size_t reach = j->highWater - lowest;
		const int bucket = reach <= (64u << 10) ? 0 : reach <= (256u << 10) ? 1 : reach <= (1u << 20) ? 2 : reach <= (4u << 20) ? 3 : 4;
		atomic_fetch_add(&s_reach[bucket], 1);
	}
	if (j->source == NULL) {
		alloc_source(j);
	}
	// (A small buffer, the shader JIT's one per shader, is written once: mapped the plain
	// way, without a writable view from the HEN.)
	if (j->mappedCount == 0 && !j->alias && s_aliasState >= 0 && !j->small) {
		try_alias(j);
	}
	if (j->alias) {
		static OuncehenJitRange ranges[MAX_RANGES];  // (under the lock)
		for (size_t r = 0; r < j->dirtyCount; r++) {
			ranges[r].offset = j->dirty[r].begin;
			ranges[r].size = j->dirty[r].end - j->dirty[r].begin;
		}
		// (Copies the bytes from the write area and invalidates the instruction cache for them.)
		u64 bytes = 0;
		for (size_t r = 0; r < j->dirtyCount; r++) {
			bytes += ranges[r].size;
		}
		const u64 p0 = armGetSystemTick();
		const Result rc = publish_ranges(j, ranges, j->dirtyCount);
		const u64 pns = armTicksToNs(armGetSystemTick() - p0);
		atomic_fetch_add(&s_pub[0], bytes);
		atomic_fetch_add(&s_pub[1], j->dirtyCount);
		atomic_fetch_add(&s_pub[2], pns);
		GAP_ADD(15 /* HEN_GAP_PUBLISH */, pns);
		GAP_ADD(16 /* HEN_GAP_PUBLISH_COUNT */, 1);
		atomic_fetch_add(&s_pub[pns < 40000 ? 3 : pns < 100000 ? 4 : pns < 1000000 ? 5 : 6], 1);
#if HEN_DEV
		if (pns > 10000000) {
			// (the HEN answers from its own process: this is how long it kept the caller waiting)
			printf("hen: slow: %llu ms in one publish of JIT code to the system\n", (unsigned long long) (pns / 1000000));
		}
#endif
		if (R_FAILED(rc)) {
			fail(j, "publishing", j->dirty[0].begin, rc);
		}
		j->dirtyCount = 0;
		atomic_fetch_add(&s_syncNs, armTicksToNs(armGetSystemTick() - t0));
		atomic_fetch_add(&s_syncs, 1);
		return;
	}
	// The pieces wanted, with a unit that fits the mappings left.
	struct piece want[MAX_MAPPINGS + 1];
	unsigned wantCount;
	for (;;) {
		wantCount = pieces_for(j, j->unit, want);
		if (wantCount <= MAX_MAPPINGS && s_mappings - j->mappedCount + wantCount <= MAX_MAPPINGS) {
			break;
		}
		if (j->unit >= j->size) {
			fail(j, "finding mappings", j->highWater, 0);
		}
		j->unit *= 2;
	}
	// Unmap the pieces that aren't wanted any more or have changes.
	unsigned kept = 0;
	for (unsigned i = 0; i < j->mappedCount; i++) {
		const struct piece p = j->mapped[i];
		bool wanted = false;
		for (unsigned w = 0; w < wantCount; w++) {
			wanted |= want[w].begin == p.begin && want[w].size == p.size;
		}
		if (wanted && !is_dirty(j, &p)) {
			j->mapped[kept++] = p;
			continue;
		}
		const Result rc = __nx_jit_unmap_code(j->exec + p.begin, j->source + p.begin, p.size);
		if (R_FAILED(rc)) {
			fail(j, "unmapping", p.begin, rc);
		}
		s_mappings--;
	}
	j->mappedCount = kept;
	for (size_t r = 0; r < j->dirtyCount; r++) {
		const size_t begin = j->dirty[r].begin, end = j->dirty[r].end;
		memcpy(j->source + begin, j->write + begin, end - begin);
		armDCacheFlush(j->source + begin, end - begin);
	}
	// Map the wanted pieces that aren't mapped.
	for (unsigned w = 0; w < wantCount; w++) {
		bool have = false;
		for (unsigned i = 0; i < j->mappedCount; i++) {
			have |= j->mapped[i].begin == want[w].begin && j->mapped[i].size == want[w].size;
		}
		if (have) {
			continue;
		}
		const Result rc = __nx_jit_map_code(j->exec + want[w].begin, j->source + want[w].begin, want[w].size);
		if (R_FAILED(rc)) {
			// (The code for this piece is already written for its address and about to be
			// run, and nothing tells the JIT that a buffer ends early, so the program cannot
			// carry on from here: it ends, with the kernel's view of both ranges in the log.)
			report_refusal(j, j->source, want[w].begin, want[w].size, rc, NULL);
			fail(j, "mapping", want[w].begin, rc);
		}
		j->mapped[j->mappedCount++] = want[w];
		s_mappings++;
		atomic_fetch_add(&s_maps, 1);
	}
	for (size_t r = 0; r < j->dirtyCount; r++) {
		armICacheInvalidate(j->exec + j->dirty[r].begin, j->dirty[r].end - j->dirty[r].begin);
	}
	j->dirtyCount = 0;
	atomic_fetch_add(&s_syncNs, armTicksToNs(armGetSystemTick() - t0));
	atomic_fetch_add(&s_syncs, 1);
}

// ---- The second buffer of a re-sort ---------------------------------------------------
// A buffer made by hen_jit_create that the JIT has not used: its source is allocated, the whole
// of it mapped as code and the HEN asked for its writable view, as try_alias does for a buffer's
// first code, but nothing here ends the program or changes how other buffers are handled when
// a step is refused: the re-sort just does not happen. Only where code is updated through that
// view (hen_jit_alias_active). The heap block and the kernel's walk of it are had without the
// lock every code update takes (the buffer is nobody else's yet); the two requests to the HEN
// are made under it, as every such request is (about 7 ms for 128 MiB, once per buffer: a code
// update of any JIT waits that long if it comes just then). Returns 1 if the buffer is mapped
// and can be published to.
void hen_jit_invalidate(struct hen_jit* j, uint32_t* xmem, size_t size);
void hen_jit_executable(struct hen_jit* j);
int hen_jit_resort_prepare(struct hen_jit* j, unsigned long long* map_ns) {
	*map_ns = 0;
	mutexLock(&s_lock);
	const bool have = j->alias;
	const bool can = s_aliasState == 1 && !j->small && j->mappedCount == 0 && __nx_jit_map_alias != NULL &&
	                 s_mappings < MAX_MAPPINGS;
	mutexUnlock(&s_lock);
	if (have) {
		return 1;
	}
	if (!can) {
		return 0;
	}
	// (a second buffer is written to at once, all over)
	if (write_full(j, false) < 0) {
		printf("hen_jit: buffer %u (a re-sort's): no heap block of %zu MiB for the write area\n", j->id, j->size >> 20);
		return 0;
	}
	size_t align = j->align;
	u8* raw = align != PAGE ? malloc(j->size + align) : NULL;
	u8* block = raw != NULL ? (u8*) (((uintptr_t) raw + align - 1) & ~(uintptr_t) (align - 1)) : NULL;
	if (raw == NULL) {
		align = PAGE;
		raw = block = memalign(PAGE, j->size);
	}
	if (block == NULL) {
		printf("hen_jit: buffer %u (a re-sort's): no heap block of %zu MiB for the code source\n", j->id, j->size >> 20);
		return 0;
	}
	const size_t locked = locked_bytes(block, j->size);
	if (locked != 0) {
		printf("hen_jit: buffer %u (a re-sort's): %zu KiB of the heap block are locked by an earlier program\n", j->id, locked >> 10);
		free(raw);
		return 0;
	}
	mutexLock(&s_lock);
#if HEN_DEV
	refuse_test(j, 1, j->size / 2);
#endif
	const u64 m0 = armGetSystemTick();
	Result rc = __nx_jit_map_code(j->exec, block, j->size);
	// Refused because something is in the address range while the heap block
	// is plain heap: nothing has been written for this range yet (the re-sort reads the address
	// after this call), so the buffer gets another range, unmapped by the kernel's answer when
	// it is chosen, and the same block is mapped there. Up to three other ranges.
	bool blockAtFault = true;  // (unless the kernel shows the fault in the address range alone)
	for (int attempt = 0; R_FAILED(rc) && attempt < 3; attempt++) {
		struct refusal why;
		report_refusal(j, block, 0, j->size, rc, &why);
		u8* const old = j->exec;
		blockAtFault = why.destFree || !why.sourcePlain;
		if (blockAtFault || !move_range(j)) {
			break;
		}
		printf("hen_jit: buffer %u (a re-sort's): address range %p to %p is occupied (%s): trying %p to %p (other range %d of 3)\n",
		       j->id, (void*) old, (void*) (old + j->size), why.dest, (void*) j->exec, (void*) (j->exec + j->size),
		       attempt + 1);
		rc = __nx_jit_map_code(j->exec, block, j->size);
	}
	if (R_FAILED(rc)) {
		// (set aside, as try_alias does: freed, the allocator hands the same pages out again)
		if (blockAtFault && s_refusedCount < MAX_REFUSED) {
			s_refused[s_refusedCount++] = raw;
			s_refusedTotal++;
			raw = NULL;
		}
		mutexUnlock(&s_lock);
		printf("hen_jit: buffer %u (a re-sort's): mapping %zu MiB was refused (0x%x)\n", j->id, j->size >> 20, rc);
		free(raw);
		return 0;
	}
	rc = __nx_jit_map_alias(j->exec, j->size);
	if (R_FAILED(rc)) {
		const Result back = __nx_jit_unmap_code(j->exec, block, j->size);
		mutexUnlock(&s_lock);
		printf("hen_jit: buffer %u (a re-sort's): the system gave no writable view (0x%x; unmapped again: 0x%x)\n", j->id, rc, back);
		if (R_SUCCEEDED(back)) {
			free(raw);
		}
		return 0;
	}
	*map_ns = armTicksToNs(armGetSystemTick() - m0);
	j->align = align;
	j->source = block;
	j->sourceBlock = raw;
	j->alias = true;
	j->lib.type = JitType_SetProcessMemoryPermission;
	j->lib.size = j->size;
	j->lib.src_addr = j->source;
	j->lib.rx_addr = j->exec;
	j->lib.rw_addr = j->write;
	j->mapped[0].begin = 0;
	j->mapped[0].size = j->size;
	j->mappedCount = 1;
	s_mappings++;
	atomic_fetch_add(&s_maps, 1);
	// (not a JIT's first session, whose update is held back: every change is published when reported)
	j->sessions = 2;
	j->writing = false;
	mutexUnlock(&s_lock);
	printf("hen_jit: buffer %u: %zu MiB of code at %p, from heap memory at %p (%s): a re-sort's second buffer, mapped in %.2f ms\n",
	       j->id, j->size >> 20, (void*) j->exec, (void*) j->source,
	       (((uintptr_t) j->exec | (uintptr_t) j->source | j->size) & (BLOCK_2M - 1)) == 0 ? "both on 2 MiB boundaries"
	                                                                                      : "not on 2 MiB boundaries",
	       *map_ns / 1e6);
	return 1;
}

// size bytes at begin of such a buffer's write area, to its execute range: in requests of at
// most hen_resort_chunk bytes, each sent and answered before the next (the lock is free between
// two, so another buffer's update waits for one request at most). ns: in all; longest_ns: one.
void hen_jit_resort_publish(struct hen_jit* j, size_t begin, size_t size, unsigned long long* ns, unsigned* requests,
                            unsigned long long* longest_ns) {
	*ns = *longest_ns = 0;
	*requests = 0;
	const size_t chunk = hen_resort_chunk >= 4096 ? ((size_t) hen_resort_chunk < PUB_PAYLOAD ? (size_t) hen_resort_chunk : PUB_PAYLOAD) : 4096;
	while (size != 0) {
		const size_t n = size < chunk ? size : chunk;
		const u64 t0 = armGetSystemTick();
		hen_jit_invalidate(j, (uint32_t*) (j->exec + begin), n);
		hen_jit_executable(j); // (published by the line above unless the buffer was left "being written": then here)
		const unsigned long long took = armTicksToNs(armGetSystemTick() - t0);
		*ns += took;
		if (took > *longest_ns) {
			*longest_ns = took;
		}
		++*requests;
		begin += n;
		size -= n;
	}
}

// Code mappings in use (0 once every JIT is destroyed: one left at exit keeps heap pages
// locked for the next program that runs in this process).
unsigned hen_jit_mappings_in_use(void) { return s_mappings; }

void hen_jit_writable(struct hen_jit* j) { j->writing = true; }

void hen_jit_executable(struct hen_jit* j) {
	j->writing = false;
	// A large buffer's first session is the JIT's prelude: applied with its
	// first compiled block (see the top of this file).
	if (j->sessions++ == 0 && !j->small) {
		return;
	}
	mutexLock(&s_lock);
	sync(j);
	mutexUnlock(&s_lock);
}

// The JIT changed size bytes of code at execute address xmem (through the
// write area). Applied by the next hen_jit_executable, or now if the buffer
// is executable already.
void hen_jit_invalidate(struct hen_jit* j, uint32_t* xmem, size_t size) {
	const size_t begin = (size_t) ((u8*) xmem - j->exec);
	if (size == 0 || begin >= j->usable) {
		return;
	}
	const size_t end = begin + size < j->usable ? begin + size : j->usable;
	mutexLock(&s_lock);
	if (end > j->highWater) {
		j->highWater = end;
	}
	if (end > j->peak) {
		j->peak = end;
	}
	if (j->dirtyCount == MAX_RANGES) {
		sync(j);  // out of slots: apply what's pending
	}
	// Code is mostly written in address order: extend the last range when this one touches it.
	if (j->dirtyCount > 0 && begin >= j->dirty[j->dirtyCount - 1].begin && begin <= j->dirty[j->dirtyCount - 1].end) {
		if (end > j->dirty[j->dirtyCount - 1].end) {
			j->dirty[j->dirtyCount - 1].end = end;
		}
	} else {
		j->dirty[j->dirtyCount].begin = begin;
		j->dirty[j->dirtyCount].end = end;
		j->dirtyCount++;
	}
	if (!j->writing && (j->sessions > 1 || j->small)) {
		sync(j);
	}
	mutexUnlock(&s_lock);
}

// For load statistics: syncs (compiles applied), their total time, cell maps, cache clears.
void hen_jit_stats(unsigned long long* syncs, unsigned long long* ns, unsigned long long* maps,
                   unsigned long long* clears) {
	*clears = atomic_load(&s_clears);
	*syncs = atomic_load(&s_syncs);
	*ns = atomic_load(&s_syncNs);
	*maps = atomic_load(&s_maps);
}

static unsigned long long boost_blocks(void) { return atomic_load_explicit(&s_boostBlocks, memory_order_relaxed); }
// Blocks compiled so far, as the loading boost counts them (every build: the session report).
unsigned long long hen_blocks_compiled(void) { return boost_blocks(); }

#if HEN_DEV
// (development builds) Every heap block the kernel shows with an attribute or a
// reference from a request or a device, with what we know lies there: at the start of the
// program (what was left by whatever ran before it) and at the end of its exit steps. The
// same four blocks, 170952 KiB, are held at exit after every title.
// Into out when given (the start: there is no log yet), else to the log.
int hen_thread_stack_owner(uintptr_t at, void* (**func)(void*));
extern char _start[];
void hen_jit_heap_walk(const char* label, char* out, size_t outSize) {
	const u64 t0 = armGetSystemTick();
	size_t used = 0;
	char line[320];
#define WALK_EMIT()                                                              \
	do {                                                                         \
		if (out == NULL) {                                                       \
			puts(line);                                                          \
		} else if (used + strlen(line) + 2 < outSize) {                          \
			used += (size_t) snprintf(out + used, outSize - used, "%s\n", line); \
		}                                                                        \
	} while (0)
	u64 heapStart = 0, heapSize = 0;
	if (R_FAILED(svcGetInfo(&heapStart, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
	    R_FAILED(svcGetInfo(&heapSize, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0))) {
		snprintf(line, sizeof(line), "hen: %s: no heap region from the kernel", label);
		WALK_EMIT();
		return;
	}
	unsigned long long held = 0, heapMapped = 0;
	unsigned blocks = 0, queried = 0;
	u64 at = heapStart;
	const u64 end = heapStart + heapSize;
	const Thread* const self = threadGetSelf();
	for (; at < end && queried < 200000; queried++) {
		MemoryInfo info;
		u32 page;
		if (R_FAILED(svcQueryMemory(&info, &page, at)) || info.size == 0) {
			break;
		}
		const u64 stop = info.addr + info.size < end ? info.addr + info.size : end;
		if ((info.type & MemState_Type) == MemType_Heap) {
			heapMapped += stop - at;
		}
		if ((info.type & MemState_Type) == MemType_Heap &&
		    (info.attr != 0 || info.ipc_refcount != 0 || info.device_refcount != 0)) {
			held += stop - at;
			blocks++;
			// Whose it is, as far as this program knows.
			char owner[120] = "nothing of ours known here (a service's transfer memory, the driver's, the system's)";
			void* (*func)(void*) = NULL;
			const int stack = hen_thread_stack_owner((uintptr_t) at, &func);
			if (stack != 0) {
				snprintf(owner, sizeof(owner), "the stack of a %s thread, start +0x%lx", stack == 1 ? "live" : "finished, unjoined",
				         (unsigned long) ((uintptr_t) func - (uintptr_t) _start));
			} else if (self != NULL && self->stack_mem != NULL && at >= (u64) (uintptr_t) self->stack_mem &&
			           at < (u64) (uintptr_t) self->stack_mem + self->stack_sz) {
				snprintf(owner, sizeof(owner), "this thread's own stack");
			} else if (s_pubRanges != NULL && at >= (u64) (uintptr_t) s_pubRanges && at < (u64) (uintptr_t) s_pubRanges + PUB_RANGES_BYTES) {
				snprintf(owner, sizeof(owner), "the JIT's publish ranges buffer");
			} else if (s_pubPayload != NULL && at >= (u64) (uintptr_t) s_pubPayload && at < (u64) (uintptr_t) s_pubPayload + PUB_PAYLOAD) {
				snprintf(owner, sizeof(owner), "the JIT's publish bytes buffer");
			} else {
				for (unsigned i = 0; i < MAX_LIVE; i++) {
					const struct hen_jit* const j = s_live[i];
					if (j == NULL) {
						continue;
					}
					if (j->source != NULL && at >= (u64) (uintptr_t) j->source && at < (u64) (uintptr_t) j->source + j->size) {
						snprintf(owner, sizeof(owner), "the code source of JIT buffer %u (%u piece(s) mapped)", j->id, j->mappedCount);
					} else if (j->write != NULL && at >= (u64) (uintptr_t) j->write && at < (u64) (uintptr_t) j->write + j->size) {
						snprintf(owner, sizeof(owner), "the write area of JIT buffer %u", j->id);
					}
				}
				for (unsigned i = 0; i < s_refusedCount; i++) {
					// (the block is at most 2 MiB longer than a buffer; sizes are not kept)
					if (s_refused[i] != NULL && at >= (u64) (uintptr_t) s_refused[i] &&
					    at < (u64) (uintptr_t) s_refused[i] + ((size_t) 130 << 20)) {
						snprintf(owner, sizeof(owner), "a heap block set aside after a refused mapping");
					}
				}
			}
			if (blocks <= 24) {
				snprintf(line, sizeof(line),
				         "hen: %s:   %p, %llu KiB: attribute 0x%x (%s%s%s%s), permission %u, state 0x%x, ipc %u, device %u: %s",
				         label, (void*) (uintptr_t) at, (unsigned long long) (stop - at) >> 10, (unsigned) info.attr,
				         info.attr & MemAttr_IsBorrowed ? "locked " : "", info.attr & MemAttr_IsIpcMapped ? "ipc " : "",
				         info.attr & MemAttr_IsDeviceMapped ? "device " : "", info.attr & MemAttr_IsUncached ? "uncached " : "",
				         (unsigned) info.perm, (unsigned) info.type, (unsigned) info.ipc_refcount,
				         (unsigned) info.device_refcount, owner);
				WALK_EMIT();
			}
		}
		at = stop;
	}
	snprintf(line, sizeof(line),
	         "hen: %s: %llu KiB in %u block(s) of %llu MiB of heap (at %p); the walk asked the kernel %u times in %.2f ms",
	         label, held >> 10, blocks, heapMapped >> 20, (void*) (uintptr_t) heapStart, queried,
	         armTicksToNs(armGetSystemTick() - t0) / 1e6);
	WALK_EMIT();
#undef WALK_EMIT
}
#endif
