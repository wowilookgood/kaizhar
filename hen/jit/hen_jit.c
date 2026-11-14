// Code memory for the JITs (dynarmic and the shader JIT, through oaknut's
// CodeBlock) on Switch 2 homebrew (kaiHEN).
//
// The HEN maps heap memory as code (__nx_jit_map_code, in the devkit's
// kaihen-jit.o: link it) and unmaps it again; mapped memory can't be
// written, and its source pages can't be touched. Measured 2026-11-09:
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
// The pieces (2026-11-10). dynarmic's code only grows at its end now (block
// links go through a table: hen_link_table, address_space.cpp), so only the
// end is ever updated, and an update costs what unmapping and mapping its
// piece costs. The mapped part [0, end) is therefore cut like a binary number:
// the last 64 KiB unit is a piece of its own, and the units before it are
// pieces of 1, 2, 4... units, one per set bit of their count. Updating the
// end remaps 64 KiB (~0.05 ms; the 4 MiB cells before this took 0.45 ms),
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

// Since kaiHEN 0.01 the HEN can keep a writable view of mapped code and copy
// bytes into it on request (__nx_jit_map_alias and kaihenJitPublishRanges), and
// allows 512 mappings. With that, a buffer is mapped
// once, whole, and an update is one request carrying the changed bytes: nothing is
// unmapped, so the pieces above aren't needed. Used when the loader offers it
// (j->alias); otherwise the pieces, as before.
Result __nx_jit_map_alias(void* dst, u64 size) __attribute__((weak));
typedef struct {
	u64 offset;
	u64 size;
} OuncehenJitRange;
// (kaihenJitPublishRanges from the devkit 20261113f on, ouncehenJitPublishRanges before: the
// same call. Both weak; whichever the devkit has is used. Neither: the pieces.)
Result kaihenJitPublishRanges(Jit* code, const OuncehenJitRange* ranges, size_t count) __attribute__((weak));
Result ouncehenJitPublishRanges(Jit* code, const OuncehenJitRange* ranges, size_t count) __attribute__((weak));
static Result jit_publish_ranges(Jit* code, const OuncehenJitRange* ranges, size_t count) {
	return kaihenJitPublishRanges != NULL ? kaihenJitPublishRanges(code, ranges, count)
	                                      : ouncehenJitPublishRanges(code, ranges, count);
}
int hen_jit_alias = 1;  // --no-jit-alias: the pieces even where the alias is offered (for comparing)
// Publishes: bytes, ranges, ns in the request, and how many took <40 us, <100 us, <1 ms, longer.
static _Atomic unsigned long long s_pub[7];
void hen_jit_publish_stats(unsigned long long* out) {
	for (int i = 0; i < 7; i++) {
		out[i] = atomic_load(&s_pub[i]);
	}
}
static int s_aliasState;  // 0 not tried, 1 works, -1 refused

#define MAX_MAPPINGS 256  // (kaiHEN 0.01 allows 512; it was 16 before)
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
};

#define MAX_LIVE 64
static struct hen_jit* s_live[MAX_LIVE];  // buffers created and not destroyed
static Mutex s_lock;
static unsigned s_mappings;  // of MAX_MAPPINGS
static _Atomic unsigned long long s_syncs, s_syncNs, s_maps, s_clears;
static _Atomic unsigned long long s_reach[5];  // updates reaching back <=64K, <=256K, <=1M, <=4M, more
// Since the last call.
void hen_jit_reach_stats(unsigned long long* out) {
	for (int i = 0; i < 5; i++) {
		out[i] = atomic_exchange(&s_reach[i], 0);
	}
}

// Compile statistics from the JIT (dynarmic's address_space.cpp): time making IR, time emitting
// code, blocks, and calls (one call compiles a block and those it links to).
static _Atomic unsigned long long s_irNs, s_emitNs, s_blocks, s_compiles;
unsigned long long hen_jit_now_ns(void) { return armTicksToNs(armGetSystemTick()); }
static _Atomic unsigned long long s_boostBlocks; // (never reset: blocks compiled, for the loading boost)
// Time in the interpreter (ns, never reset): the title is running code that is not compiled
// yet. Blocks compiled is the wrong measure of that: a warm Smash compiles its 634457 listed
// blocks ahead for the whole route, and counting those held the clock up for 95 of 100 s
// (2026-11-13).
static _Atomic unsigned long long s_boostDemandBlocks;
void hen_jit_note_compile(unsigned long long irNs, unsigned long long emitNs, unsigned blocks) {
	atomic_fetch_add(&s_irNs, irNs);
	atomic_fetch_add(&s_emitNs, emitNs);
	atomic_fetch_add(&s_blocks, blocks);
	atomic_fetch_add(&s_compiles, 1);
	atomic_fetch_add_explicit(&s_boostBlocks, blocks, memory_order_relaxed);
}
// Translation against emission for the blocks compiled ahead (HenEmitAhead), timed inside it.
static _Atomic unsigned long long s_splitIrNs, s_splitEmitNs, s_splitBlocks;
void hen_jit_note_split(unsigned long long translateNs, unsigned long long emitNs) {
	atomic_fetch_add_explicit(&s_splitIrNs, translateNs, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_splitEmitNs, emitNs, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_splitBlocks, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_boostBlocks, 1, memory_order_relaxed);
}
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
// Since the last call: IR ns, emit ns, blocks, calls.
void hen_jit_compile_stats(unsigned long long* out) {
	out[0] = atomic_exchange(&s_irNs, 0);
	out[1] = atomic_exchange(&s_emitNs, 0);
	out[2] = atomic_exchange(&s_blocks, 0);
	out[3] = atomic_exchange(&s_compiles, 0);
}

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
// 3: a second thread compiles ahead of the program (dynarmic's address_space.cpp). It needs code
// that stays mapped while it is updated (try_alias); where the loader doesn't offer that, the
// setting falls back to 0. 0: no worker (--translation-worker 0). 1 and 2 were earlier
// arrangements (the worker only translated) that the frontend no longer offers; dynarmic's
// address_space.cpp still has their paths.
int hen_translation_worker = 3;
static _Atomic unsigned long long s_translated, s_translationsUsed, s_translationsStale;
void hen_jit_note_translation(unsigned made, unsigned used, unsigned stale) {
	atomic_fetch_add_explicit(&s_translated, made, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_translationsUsed, used, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_translationsStale, stale, memory_order_relaxed);
}
// The compile worker (--translation-worker 3; dynarmic's address_space.cpp, "Compiling on a
// second thread"): how many branches past a block the program asked for it compiles, and how
// many blocks past the wanted one the emulation thread still compiles itself at a miss.
int hen_worker_depth_limit = 4;  // --jit-worker-depth
int hen_emu_ahead = 4;           // --jit-ahead
// Interpreter first (dynarmic's AddressSpace::HenGetOrDefer, Azahar's ARM_Dynarmic::Run): how
// many times a block that isn't compiled yet is interpreted before the emulation thread
// compiles it itself. 0: never interpret.
// 250 (the most the visit counter holds) since 2026-11-12: along the recorded routes, cold, Kid
// Icarus gave up 1202 ms of lag in its first 25 s at 16, 970 at 32, 816 at 64, 771 at 128 and
// 713 to 728 at 250; Pokemon Omega Ruby 348 at 64 and 309 at 250 in its first 30 s. The later
// the emulation thread gives up on the worker and compiles a block itself, the fewer it compiles
// (5088 at 16, 727 at 250), and that outweighs the extra interpreting.
int hen_tier = 1000;             // --jit-tier (250 until 2026-11-13: the counter was 8 bits)
int hen_tier_force = 0;
// A block interpreted right after an interpreted block, when it isn't compiled either, without
// going through the generated code's entry and exit and without copying the registers out and
// in between the two (ARM_Dynarmic::HenInterpretBlock, dynarmic's Jit::HenPeekDefer).
// --no-interp-chain: every block by itself, as before 2026-11-13.
int hen_interp_chain = 0;
static _Atomic unsigned long long s_tierChained;
void hen_jit_note_chained(unsigned blocks) { atomic_fetch_add_explicit(&s_tierChained, blocks, memory_order_relaxed); }
// Since the last call: of the blocks interpreted, those run that way.
unsigned long long hen_jit_chain_stats(void) { return atomic_exchange(&s_tierChained, 0); }
// A block still interpreted after this many visits goes back to the top of the compile worker's
// list (dynarmic HenGetOrDefer). 16 since 2026-11-12; 4, 16 and 64 measured alike, all a little
// better than off: Kid Icarus cold, first 25 s, lag 566 -> 527 ms; Smash, first 55 s, lag 469 ->
// 445 ms and 1855 -> 1629 ms in gaps between frames; Pokemon OR unchanged. --jit-bump 0: off.
int hen_bump = 16;
// kind 0: a block left to the interpreter; 1: compiled on the emulation thread after hen_tier
// visits; 2: compiled at once because the interpreter refused it.
static _Atomic unsigned long long s_tier[3], s_tierInstrs, s_tierNs;
void hen_jit_note_tier(int kind) { atomic_fetch_add_explicit(&s_tier[kind], 1, memory_order_relaxed); }
// The compile worker publishes a block the program asked for without filling the batch up,
// and is woken only when asleep (dynarmic's address_space.cpp; --no-jit-publish-early).
int hen_publish_early = 1;
static _Atomic unsigned long long s_earlyPublish, s_wakeSkipped;
void hen_jit_note_early_publish(void) { atomic_fetch_add_explicit(&s_earlyPublish, 1, memory_order_relaxed); }
void hen_jit_note_wake_skipped(void) { atomic_fetch_add_explicit(&s_wakeSkipped, 1, memory_order_relaxed); }
void hen_jit_early_stats(unsigned long long* out) {
	out[0] = atomic_exchange(&s_earlyPublish, 0);
	out[1] = atomic_exchange(&s_wakeSkipped, 0);
}
void hen_jit_note_interpreted(unsigned instrs, unsigned long long ns) {
	atomic_fetch_add_explicit(&s_tierInstrs, instrs, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_tierNs, ns, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_boostDemandBlocks, ns, memory_order_relaxed);
}
// In all, ns in the interpreter (core_timing.cpp: the automatic New 3DS clock stays down while
// code is new).
unsigned long long hen_jit_interpreted_ns(void) {
	return atomic_load_explicit(&s_boostDemandBlocks, memory_order_relaxed);
}
// Since the last call: the three kinds, instructions interpreted, ns in the interpreter.
void hen_jit_tier_stats(unsigned long long* out) {
	for (int i = 0; i < 3; i++) {
		out[i] = atomic_exchange(&s_tier[i], 0);
	}
	out[3] = atomic_exchange(&s_tierInstrs, 0);
	out[4] = atomic_exchange(&s_tierNs, 0);
}
int hen_jit_alias_active(void);
// The thread that creates a worker (the emulation thread), just before: which core it is on.
static int s_emuCore = -1;
void hen_jit_worker_creating(void) { s_emuCore = (int) svcGetCurrentProcessorNumber(); }
// The worker's thread, at its start: on another core than the emulation thread (threads of
// one priority on one core don't take turns here), and below it in priority if allowed.
Handle hen_worker_handle; // (the first compile worker, for the black box below)
void hen_jit_worker_started(void) {
	if (hen_worker_handle == 0) {
		hen_worker_handle = threadGetCurHandle();
	}
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
// Since the last call: made by the worker, used, found stale.
void hen_jit_translation_stats(unsigned long long* out) {
	out[0] = atomic_exchange(&s_translated, 0);
	out[1] = atomic_exchange(&s_translationsUsed, 0);
	out[2] = atomic_exchange(&s_translationsStale, 0);
}
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
// same. One run a cell, the same direction in all four.
// Fragment shader configurations made canonical before hashing (pica_fs_config.cpp,
// FSConfig::Canonicalize): --no-fs-canon: 0. Counts: configurations as the registers gave
// them, and fragment shaders actually built.
int hen_fs_canon = 1;
// The generic fragment shader (glsl_fs_shader_gen.cpp, GenerateGenericFragmentShader): a draw
// shaded on the CPU whose stand-in pipeline is not ready is drawn with it instead of being
// left out. 0: --no-generic-fs. 2: --generic-all, every such draw (for comparing pictures).
int hen_generic_fs = 1;
unsigned long long hen_generic_stats[3], hen_generic_unsupported;
int hen_stale_profile = 0; // --stale-profile (vk_pipeline_cache.cpp)
// A shader cache file made by another build of the shader generator or for another profile is
// regenerated by the shader workers while the title runs, not before it starts
// (vk_shader_disk_cache.cpp, HenRegen). 1: --cache-regen-sync, before it starts as upstream, to
// compare.
int hen_cache_regen_sync = 0;
int hen_generic_first = 1; // stand-ins never built on the renderer's thread (default since 2026-11-13; --no-generic-first)
int hen_generic_seed = 1; // (--no-generic-seed: its pipeline is started by the first draw that needs it)
// The generic fragment shader's part of a pipeline, kept for reuse (vk_graphics_pipeline.cpp).
unsigned long long hen_part_cache_hits;
unsigned long long hen_fs_configs_raw, hen_fs_configs_built;
int hen_load_boost = 1;
static _Atomic unsigned long long s_boostFrames, s_boostWorkNs, s_boostWorkSum;
static _Atomic u64 s_warmLast; // (the lenient signal: what the audio cushion goes by)
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
static unsigned long long s_boostOnNs, s_boostSwitches, s_boostWhy[5];
void hen_boost_note_frame(void) { atomic_fetch_add_explicit(&s_boostFrames, 1, memory_order_relaxed); }
// A pipeline build that took real time (ns of it), from whichever thread built it.
void hen_boost_note_work(unsigned long long ns) {
	atomic_store_explicit(&s_boostWorkNs, armTicksToNs(armGetSystemTick()), memory_order_relaxed);
	atomic_fetch_add_explicit(&s_boostWorkSum, ns, memory_order_relaxed);
}
// Docked: the CPU's clock set to the rate the system's own boost mode uses, through the clock
// controller (switch_platform.c, sw_cpu_clock_quiet), with the GPU left as it is. Measured
// 2026-11-12, docked: normal CPU 998.4 MHz and GPU 1007.25 MHz; the system's boost mode CPU
// 1523.2 MHz and GPU 306 MHz; the service offers the CPU up to 1702.4 MHz. Five titles, cold,
// first 55 s, clock held against the system's boost in bursts: lag given up 1451 -> 617 ms,
// audio held 1457 -> 861 ms, time in gaps between frames 6346 -> 4072 ms.
// Handheld, or if the service refuses: the system's boost mode as before.
// hen_load_boost: 0 off, 1 this (default), 3 the system's mode everywhere (--load-boost-system).
#define HEN_BOOST_CPU_HZ 1523200000u
int sw_cpu_clock_quiet(unsigned hz); // 1 if set
void sw_cpu_clock_end(void);
void sw_cpu_clock_forget(void);
static bool s_boostByClock, s_boostDocked;
static unsigned long long s_boostSwitchNsMax;
int hen_boost_lenient = 1;      // --boost-strict: 0
int hen_boost_linger_ms = 0;    // --boost-linger MS: how long after the last reason it stays on
                                // (0: 10 s by the CPU clock, 2 s by the system's boost mode)
int hen_boost_cancel = 1;       // --no-boost-cancel: 0 (the system's boost mode ended with
                                // appletSetCpuBoostMode(Normal), as before 2026-11-14)
int hen_n3ds_mode = 0;          // core_timing.cpp: the title runs in the New 3DS fast CPU mode
// A New 3DS title in handheld (owner, 2026-11-14: "new 3ds mode should work in handheld"; at
// the normal clock such a title was far too slow). 1: the CPU clock held as when docked (the
// system's own boost rate, GPU untouched). 2: the system's boost mode held instead (CPU the
// same, GPU at its lowest rate), also what 1 falls back to if the clock is refused. 0: nothing
// held in handheld, as before. --n3ds-handheld N. Heat and battery in handheld are unmeasured.
int hen_n3ds_handheld = 1;
// Handheld, any title: the system's boost mode held for as long as the title runs. Until
// 2026-11-14 that was what happened without being meant: the mode was ended with
// appletSetCpuBoostMode(Normal), which keeps the request, so after the first loading burst the
// CPU stayed at the boost rate. With the request really cancelled the handheld clock was too
// slow: Ocarina, warm, 60 s hands-off, 61 to 79% speed with 12186 ms of lag, against 100% and
// 54 ms held (one run each). 0: --no-handheld-hold, only while loading.
int hen_handheld_hold = 1;
static void boost_set(bool on, u64 now) {
	if (on == s_boostOn) {
		return;
	}
	const u64 t0 = armGetSystemTick();
	if (on) {
		s_boostByClock = hen_load_boost != 3 && (s_boostDocked || (hen_n3ds_mode && hen_n3ds_handheld == 1)) &&
		                 sw_cpu_clock_quiet(HEN_BOOST_CPU_HZ);
		if (!s_boostByClock && R_FAILED(appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad))) {
			hen_load_boost = 0; // (not available: stop asking)
			return;
		}
	} else if (s_boostByClock) {
		sw_cpu_clock_end();
	} else {
		// Ending the system's boost mode. libounce (services/applet.c:726-740): for an
		// Application on Switch 2, appletSetCpuBoostMode(Normal) (command 66, mode 0) keeps the
		// request active and appletCancelCpuBoostMode() (67) clears it. hen_boost_cancel 0
		// (--no-boost-cancel): the old call, to compare.
		static bool logged;
		const Result rc = hen_boost_cancel ? appletCancelCpuBoostMode()
		                                   : appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
		if (!logged) {
			logged = true;
			printf("hen: boost: ended by %s, rc 0x%x\n",
			       hen_boost_cancel ? "appletCancelCpuBoostMode" : "appletSetCpuBoostMode(Normal)", rc);
		}
		if (R_FAILED(rc)) {
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
	} else {
		s_boostOnNs += now - s_boostOnSince;
	}
	s_boostOn = on;
	s_boostSwitches++;
}
void hen_boost_off(void) { boost_set(false, armTicksToNs(armGetSystemTick())); }
static unsigned long long boost_blocks(void);
// Whether the program was loading or warming up within the last minute (the audio
// output keeps a deeper cushion then: audio_core/dsp_interface.cpp). From any thread.
int hen_boost_warming(void) {
	const u64 last = atomic_load_explicit(&s_warmLast, memory_order_relaxed);
	// (a minute: on a first launch the menus between two loads are quiet for longer than ten
	// seconds, and the cushion had shrunk again by the time the next load's long frames came)
	return last != 0 && armTicksToNs(armGetSystemTick()) - last < 60000000000ull;
}
unsigned long long hen_vblank_count; // (vertical blanks of the program so far: --screenshot-frame)
void hen_boost_tick(long long limiter_slept_total_us) {
	const u64 now = armTicksToNs(armGetSystemTick());
	hen_vblank_count++;
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
	if (span >= 500000000ull) {
		const double seconds = span / 1e9;
		const double fps = (s_boostRing[at].frames - s_boostRing[old].frames) / seconds;
		const double blocks = (s_boostRing[at].blocks - s_boostRing[old].blocks) / seconds;
		const double slept = (s_boostRing[at].slept - s_boostRing[old].slept) / 1e6 / seconds;
		const bool loading = fps < 15.0 && slept < 0.2;
		const bool compiling = blocks > 1500.0;
		// (The boost goes by bursts: over 30 ms of pipeline compiles in the second. In a first
		// fight a few pipelines are compiled every second throughout, and the first version,
		// which went by any build over 3 ms, kept the boost on for the whole fight. That
		// lenient signal is still what the audio cushion uses.)
		const bool any_pipeline = now - atomic_load_explicit(&s_boostWorkNs, memory_order_relaxed) < 500000000ull;
		const bool pipelines = (s_boostRing[at].work - s_boostRing[old].work) / seconds > 30e6;
		if (loading || compiling || any_pipeline) {
			atomic_store_explicit(&s_warmLast, now, memory_order_relaxed);
		}
		// What asks for the boost. Docked, the boost is the CPU's clock raised by itself
		// (boost_set), which costs the GPU nothing, so the lenient signal counts too: any
		// pipeline build in the last half second, not only bursts (hen_boost_lenient,
		// --boost-strict for the old rule). With the system's boost mode (handheld) the GPU
		// drops to its lowest rate (306 MHz against 1007), so there only the bursts count.
		// A New 3DS title in its fast mode holds the clock for as long as it runs: it needs the
		// CPU all the time, not while something compiles.
		const bool lenient = hen_boost_lenient && s_boostDocked && hen_load_boost != 3;
		const bool n3ds = hen_n3ds_mode && (s_boostDocked || hen_n3ds_handheld != 0) && hen_load_boost != 3;
		const bool handheld = !s_boostDocked && hen_handheld_hold && hen_load_boost != 3;
		// By the clock there are two more reasons (2026-11-12: with the bursts alone the clock
		// did no better than the system's mode, 143 against 147 ms of lag in Pokemon OR; what
		// the held clock won came from being raised already when a hitch arrives):
		// new code: over 5 ms of the second spent in the interpreter (code not compiled yet);
		// busy: the emulation thread sleeps under a tenth of the time. With the clock raised the
		// same load sleeps more (1523.2 / 998.4 MHz: 90% busy becomes 59%), so while raised
		// the line is 41%: it stays up for as long as the normal clock would be short.
		const bool new_code = lenient && (s_boostRing[at].demand - s_boostRing[old].demand) / seconds > 5e6;
		const bool busy = lenient && slept < (s_boostOn && s_boostByClock ? 0.41 : 0.10);
		if (loading || compiling || pipelines || (lenient && any_pipeline) || n3ds || handheld || new_code || busy) {
			atomic_store_explicit(&s_boostLastWanted, now, memory_order_relaxed);
			s_boostWhy[loading ? 0 : compiling || new_code ? 1 : pipelines || (lenient && any_pipeline) ? 2 : busy ? 3 : 4]++;
		}
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
		if (s_boostAt == 1 && docked && hen_boost_lenient && hen_load_boost == 1) {
			atomic_store_explicit(&s_boostLastWanted, now, memory_order_relaxed);
		}
	}
	if (hen_load_boost) {
		const bool clock = hen_boost_lenient && s_boostDocked && hen_load_boost != 3;
		const u64 linger_ms = hen_boost_linger_ms > 0 ? (u64) hen_boost_linger_ms : clock ? 10000 : 2000;
		const u64 wanted = atomic_load_explicit(&s_boostLastWanted, memory_order_relaxed);
		(void) last;
		boost_set(wanted != 0 && now - wanted < linger_ms * 1000000ull, now);
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
	s_boostSwitchNsMax = 0;
	s_boostOnNs = s_boostSwitches = 0;
	s_boostWhy[0] = s_boostWhy[1] = s_boostWhy[2] = s_boostWhy[3] = s_boostWhy[4] = 0;
}

int hen_live_fpsr = 1; // --no-live-fpsr: 0 (dynarmic, backend/arm64/fpsr_manager.cpp)
int hen_lazy_resched = 1; // --no-lazy-resched: 0 (svc.cpp, SVC::CallSVC)
unsigned long long hen_resched_skipped = 0;
// A New 3DS title that asks for the 804 MHz clock gets its CPU work charged less emulated
// time (src/core/core_timing.cpp). 2 (default): automatic, a third of the time at best and
// more when the host has none to spare. 1 (--n3ds-clock): a third, fixed. 0 (--n3ds-clock-off):
// as at 268 MHz, upstream's and the default before 2026-11-13.
int hen_n3ds_clock = 2;
unsigned long long hen_n3ds_clock_ticks = 0;
// --gpu-thread: PICA command lists, fills and transfers on a thread of their own, their
// interrupts signalled when the work is done (src/video_core/hen_gpu_thread.cpp). Off: not
// run on the console yet.
// ON since 2026-11-12 (--no-gpu-thread). Cold, along the recorded routes, first 55 s of five
// titles: emulation-thread stalls of 20 ms or more 42 -> 12 (1597 -> 528 ms), lag given up
// 2065 -> 1906 ms, its core time down 13 to 30% a title; homebrew, the CPU benchmark (identical)
// and a title started twice in one process were checked. Not yet judged by eye.
int hen_gpu_thread = 1;
// --gpu-thread-sync-writes: 0. A program's write to memory the renderer has cached waits for
// the GPU thread instead of queueing the invalidation behind its work (for comparing).
int hen_gpu_defer_writes = 1;
int hen_depth_lag = 1;  // --sync-depth-reads: 0 (vk_texture_runtime.cpp, Surface::Download)
// One compile of a vertex shader at a time (vk_graphics_pipeline.cpp, TryBuild). OFF since
// 2026-11-12: the driver of SDK 20261112 shares a compile that several pipelines ask for, and
// our list only added waiting (Pokemon Omega Ruby, cold route: 288 ms of lag given up without
// it against 417 to 431 with it; four other titles unchanged). --pipe-dedup turns it on.
int hen_pipe_dedup = 0;
unsigned long long hen_pipe_parked;  // pipelines that waited for a vertex shader another one was compiling
// Draws of up to this many vertices are shaded on the CPU while their pipeline is compiled
// (vk_rasterizer.cpp, AccelerateDrawBatchInternal). 0: never (upstream's wait or skip).
int hen_cpu_vertex_limit = 1000000;  // --cpu-vertex-limit N (a draw; 0: never)
int hen_cpu_shade_share = 33;  // --cpu-shade-share P: percent of wall time such draws may take when there is none to spare
int hen_max_lag_ms = 60; // --max-lag-ms
// What a vertex shaded on the CPU costs, as measured by PicaCore (a running mean; starts at
// the shader JIT's single-thread figure).
unsigned long long hen_cpu_shade_ns_per_vertex = 500;
long long hen_limiter_given_up_us, hen_limiter_slept_us;  // core/perf_stats.cpp; reported and reset by main.cpp
// Stand-in pipelines (for draws shaded on the CPU): [0..2] built on the emulation thread: count,
// ns in total, longest ns; [3..5] the same for those the shader workers built; [6] already built.
// Which stand-in pipelines are built on the rendering thread itself (vk_rasterizer.cpp);
// --standin-rule N. 2 (default): within the frame's measured spare time. 0: as 2, and none
// while the GPU thread is behind. Rule 0 against
// rule 2, cold, first 55 s: time in gaps between frames 1096 -> 901 ms (Mario Kart 7), 1757 ->
// 1825 (Smash), 2065 -> 1908 (Kid Icarus); audio held 18 to 29% less; but draws left out 167 ->
// 282, 54 -> 159, 26 -> 148. Fewer hitches for more missing objects: not settled.
int hen_standin_window = 2;
// Frames shown (renderer_vulkan.cpp): count, gaps over 40 / 70 / 120 ms, the longest gap (us),
// and the time in gaps over 40 ms (us). Reported and reset by main.cpp.
unsigned long long hen_present_gaps[6];
int hen_substitute = 0; // --substitute (vk_pipeline_cache.cpp, BindPipeline): off, for comparing by eye
unsigned long long hen_substituted;
unsigned long long hen_standin_stats[7];
// The same builds by part: vertex input interface, the stand-in's vertex side, fragment shader,
// output interface, link (ns); [5] parts the driver had to compile (not repeats).
unsigned long long hen_standin_part_ns[6];
__thread int hen_standin_build; // set around a stand-in's Build() (vk_rasterizer / vk_graphics_pipeline)
unsigned long long hen_fast_lane_skips;  // vk_rasterizer.cpp; reported and reset by main.cpp
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
// code (prof/NOTES.md, 2026-11-13). --no-fold-cond-branch: 0, upstream's blocks.
int hen_fold_cond_branch = 1;
// Code buffers of 4 MiB and more: the execute range and the heap memory mapped there both
// start on a 2 MiB boundary and are a multiple of 2 MiB long, so the kernel can map them
// with 2 MiB blocks if it does that (hen_jit_create, sync). --no-code-align-2m: 0, pages.
int hen_code_align_2m = 1;
int hen_depth_scale_blit = 1; // --no-depth-scale-blit (vk_texture_runtime.cpp, Surface::BlitScale)
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
int hen_exit_counts = 0; // (counted 2026-11-13; about 1% while on: --exit-counts)
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
static _Atomic unsigned long long s_lookups, s_runNs, s_runs;
void hen_jit_note_lookup(void) { atomic_fetch_add_explicit(&s_lookups, 1, memory_order_relaxed); }
void hen_jit_note_run(unsigned long long ns) {
	atomic_fetch_add_explicit(&s_runNs, ns, memory_order_relaxed);
	atomic_fetch_add_explicit(&s_runs, 1, memory_order_relaxed);
}
// Since the last call: lookups, run ns, runs.
void hen_jit_run_stats(unsigned long long* out) {
	out[0] = atomic_exchange(&s_lookups, 0);
	out[1] = atomic_exchange(&s_runNs, 0);
	out[2] = atomic_exchange(&s_runs, 0);
}

// A JIT threw away all its code because its buffer was full (it recompiles from scratch).
void hen_jit_note_cache_clear(void) { atomic_fetch_add(&s_clears, 1); }

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
// is never called, kaiHEN 0.01 included), so the log carries a line a second saying where the
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
	for (int which = 0; which < 2; which++) {
		int at = snprintf(line, sizeof(line), which == 0 ? "hen: bb: the last %u ms:" : "hen: bb: the renderer's thread in those %u ms:", ms);
		for (unsigned k = BB_RING; k > 0; k--) {
			if (count < k) {
				continue;
			}
			const unsigned i = (count - k) % BB_RING;
			if (armTicksToNs(now - s_bbTick[i]) <= (u64) ms * 1000000 && at > 0 && (size_t) at < sizeof(line) - 140) {
				at += snprintf(line + at, sizeof(line) - at, " %s", which == 0 ? s_bbEmu[i] : s_bbExtra[i]);
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
	j->unit = j->small ? j->size : UNIT;
	j->writing = true;
	j->write = malloc(j->size);
	if (j->write != NULL) {
		virtmemLock();
		// (virtmemFindCodeMemory's second argument is a guard size, it has no alignment: a
		// free range one alignment longer always holds an aligned one of the size wanted)
		j->exec = virtmemFindCodeMemory(j->size + (j->align - PAGE), PAGE);
		if (j->exec != NULL) {
			j->exec = (u8*) (((uintptr_t) j->exec + j->align - 1) & ~(uintptr_t) (j->align - 1));
		} else if (j->align != PAGE) {
			printf("hen_jit: no address range for %zu MiB of code on a 2 MiB boundary: using pages\n", j->size >> 20);
			j->align = PAGE;
			j->exec = virtmemFindCodeMemory(j->size, PAGE);
		}
#if HEN_DEV
		if (j->exec != NULL && s_profRangeCount < 16) {
			s_profRanges[s_profRangeCount].start = (uintptr_t) j->exec;
			s_profRanges[s_profRangeCount].size = j->size;
			s_profRangeCount++;
		}
#endif
		if (j->exec != NULL) {
			j->reservation = virtmemAddReservation(j->exec, j->size);
		}
		virtmemUnlock();
	}
	if (j->reservation == NULL) {
		printf("hen_jit: no memory or address space for %zu KiB of code\n", size >> 10);
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

// At exit: unmaps what is still mapped (a buffer nobody destroyed: dynarmic keeps one in a
// static object for its spin lock, whose destructor doesn't run here). The loader runs the
// next program in this process, and a mapping left behind keeps its heap pages locked for it.
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
	mutexUnlock(&s_lock);
	return released;
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
// buffer merges its pieces when the mappings run short).
size_t hen_jit_usable_size(struct hen_jit* j) { return j->size; }

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
		if (info.attr != 0 || (info.perm & Perm_Rw) != Perm_Rw) {
			locked += stop - at;
		}
		at = stop;
	}
	return locked;
}

// Under the lock: the heap memory the code is mapped from.
static void alloc_source(struct hen_jit* j) {
	// A program that ended without unmapping its code (a crash, a kill) leaves
	// those heap pages locked for whatever the loader runs next in this
	// process, and a mapping from them is refused. Such a block is kept
	// allocated, so nothing else gets it either, and another is taken.
	for (int attempt = 0; attempt < 8 && j->source == NULL; attempt++) {
		// On a 2 MiB boundary (hen_code_align_2m): a block one alignment longer, and the
		// aligned part of it. Not memalign(2 MiB): the allocator (libounce's mimalloc) has
		// a path of its own for alignments over 64 KiB, and that returned nothing for the
		// first 128 MiB buffer (2026-11-13: the program ended at its first compiled block).
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
#if HEN_DEV
			if (!j->small) {
				printf("hen_jit: buffer %u: %zu MiB of code at %p, from heap memory at %p (%s)\n", j->id, j->size >> 20,
				       (void*) j->exec, (void*) j->source,
				       (((uintptr_t) j->exec | (uintptr_t) j->source | j->size) & (BLOCK_2M - 1)) == 0
				           ? "both on 2 MiB boundaries"
				           : "not on 2 MiB boundaries");
			}
#endif
		} else {
			printf("hen_jit: buffer %u: %zu KiB of a %zu MiB heap block are still locked by an earlier program; taking another\n",
			       j->id, locked >> 10, j->size >> 20);
		}
	}
	if (j->source == NULL) {
		fail(j, "allocating the source", 0, 0);
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
	const u64 t0 = armGetSystemTick();
	Result rc = __nx_jit_map_code(j->exec, j->source, j->size);
	if (R_FAILED(rc) && j->align != PAGE) {
		// (hen_code_align_2m) Not from the aligned part of a longer block, then: a block of
		// its own on a page boundary, as without the switch.
		printf("hen_jit: buffer %u: mapping from heap memory on a 2 MiB boundary was refused (0x%x): using pages\n", j->id, rc);
		free(j->sourceBlock);
		j->source = j->sourceBlock = NULL;
		j->align = PAGE;
		alloc_source(j);
		rc = __nx_jit_map_code(j->exec, j->source, j->size);
	}
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
	rc = __nx_jit_map_alias(j->exec, j->size);
	if (R_FAILED(rc)) {
		printf("hen_jit: RO gave no writable view of the code (0x%x): using pieces\n", rc);
		s_aliasState = -1;
		if (hen_translation_worker == 3) {
			hen_translation_worker = 0;
		}
		if (R_FAILED(__nx_jit_unmap_code(j->exec, j->source, j->size))) {
			fail(j, "unmapping", 0, rc);
		}
		return;
	}
	if (s_aliasState == 0) {
		printf("hen_jit: code is updated through RO's writable view (mapping %zu MiB took %.2f ms)\n", j->size >> 20,
		       armTicksToNs(armGetSystemTick() - t0) / 1e6);
	}
	s_aliasState = 1;
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
}

int hen_jit_alias_active(void) { return s_aliasState == 1; }

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
		const Result rc = jit_publish_ranges(&j->lib, ranges, j->dirtyCount);
		const u64 pns = armTicksToNs(armGetSystemTick() - p0);
		atomic_fetch_add(&s_pub[0], bytes);
		atomic_fetch_add(&s_pub[1], j->dirtyCount);
		atomic_fetch_add(&s_pub[2], pns);
		atomic_fetch_add(&s_pub[pns < 40000 ? 3 : pns < 100000 ? 4 : pns < 1000000 ? 5 : 6], 1);
#if HEN_DEV
		if (pns > 10000000) {
			// (the HEN answers from its own process: this is how long it kept the caller waiting)
			printf("hen: slow: %llu ms in one publish of JIT code to RO\n", (unsigned long long) (pns / 1000000));
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

// Code mappings in use (0 once every JIT is destroyed: one left at exit keeps heap pages
// locked for the next program the loader runs in this process).
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
	if (size == 0 || begin >= j->size) {
		return;
	}
	const size_t end = begin + size < j->size ? begin + size : j->size;
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
