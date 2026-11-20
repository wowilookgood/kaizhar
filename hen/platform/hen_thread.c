// Threads created through pthreads (every std::thread, SDL's threads, Dawn's
// workers), via libounce's __syscall_thread_create. Linked with
// -Wl,--wrap=__syscall_thread_create (see CMakeLists.txt). Two changes:
//
// - Stack: without a size, libounce gives 128 KiB. Linux gives 8 MiB and macOS
//   512 KiB, which is what Dusklight, aurora and Dawn are tested with (Dawn's
//   shader compiler, Tint, recurses deeply). Give them 8 MiB, as on Linux.
//
// - Cores: Horizon runs a thread created this way only on the process's
//   default core; it doesn't spread threads across cores the way Linux does.
//   So the game, aurora's render worker, the pipeline compilers and Dawn's
//   workers all shared one core (measured: four pipeline workers
//   compiled slower than one). Each new thread first allows itself every core
//   the process may use, with no preferred core, and the scheduler spreads them.
#include <switch.h>

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEN_DEFAULT_THREAD_STACK (8u << 20)

struct __pthread_t;

int __real___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg, void* stack_addr,
                                   size_t stack_size);

struct hen_thread_start {
	void* (*func)(void*);
	void* arg;
};

static u32 process_core_mask(void) {
	static u32 mask = 0;
	if (mask == 0) {
		u64 info = 0;
		mask = R_SUCCEEDED(svcGetInfo(&info, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && info != 0 ? (u32) info
		                                                                                            : 1u;
	}
	return mask;
}

// Live threads by start routine, for the exit report (hen_app.c): the next
// app runs in this same process, and a thread still alive at exit
// keeps running this app's code.
#define HEN_MAX_THREADS 128
static void* (*s_live[HEN_MAX_THREADS])(void*);
static void* s_liveArg[HEN_MAX_THREADS];
static void* s_liveMirror[HEN_MAX_THREADS];
static const Thread* s_liveThread[HEN_MAX_THREADS];  // (for hen_thread_stop_live)
static char s_liveName[HEN_MAX_THREADS][24];         // (hen_thread_name, for hen_thread_table)
static Mutex s_liveLock;
#if HEN_DEV
// ("hen: thread cpu:") A thread's CPU time at the last report, in system ticks; what the
// threads that ended since then had used since their last report, and how many they were.
static bool s_cpuOn;
static u64 s_cpuLast[HEN_MAX_THREADS];
static u64 s_cpuEnded;
static unsigned s_cpuEndedCount;
#endif

// Threads whose start routine returned, with their stacks. libnx mirrors a
// thread's stack (svcMapMemory: the heap pages become inaccessible) until the
// thread is closed (pthread_join); a thread nobody joins keeps its stack
// mapped after the app exits, and the next app, which runs in this same
// process, finds those heap pages locked (the homebrew menu: no GPU, black screen).
// hen_thread_release_stacks() unmaps them at exit.
#define HEN_MAX_FINISHED 256
static struct {
	void* (*func)(void*);
	void* mirror;
	void* mem;
	size_t size;
	Handle left;  // a thread that ended through hen_thread_leave: its handle, to wait for its end
} s_finished[HEN_MAX_FINISHED];
static int s_finishedCount;

// For the heap report at start and exit (hen/jit/hen_jit.c, development builds): whose stack
// the heap address lies in. 1: a live thread's, 2: a finished thread's that nobody joined yet;
// *func is its start routine. 0: no thread known here.
int hen_thread_stack_owner(uintptr_t at, void* (**func)(void*)) {
	int found = 0;
	mutexLock(&s_liveLock);
	for (int i = 0; i < HEN_MAX_THREADS && !found; i++) {
		const Thread* const t = s_liveThread[i];
		if (t != NULL && t->stack_mem != NULL && at >= (uintptr_t) t->stack_mem &&
		    at < (uintptr_t) t->stack_mem + t->stack_sz) {
			*func = s_live[i];
			found = 1;
		}
	}
	for (int i = 0; i < s_finishedCount && !found; i++) {
		if (s_finished[i].mem != NULL && at >= (uintptr_t) s_finished[i].mem &&
		    at < (uintptr_t) s_finished[i].mem + s_finished[i].size) {
			*func = s_finished[i].func;
			found = 2;
		}
	}
	mutexUnlock(&s_liveLock);
	return found;
}

// True in a thread that is ending through hen_thread_leave (set just before it is noted as
// finished).
static __thread bool s_leaving;
// Threads that ended through hen_thread_leave: how many were seen to have ended before their
// stacks were released, and how many had not ended after the wait.
int hen_thread_left_stats[2];

static int live_add(void* (*func)(void*), void* arg, void* mirror, const Thread* self) {
	mutexLock(&s_liveLock);
	int slot = -1;
	for (int i = 0; i < HEN_MAX_THREADS; i++) {
		if (s_live[i] == NULL) {
			s_live[i] = func;
			s_liveArg[i] = arg;
			s_liveMirror[i] = mirror;
			s_liveThread[i] = self;
			s_liveName[i][0] = 0;
#if HEN_DEV
			s_cpuLast[i] = 0;  // (a new thread: all it has used counts)
#endif
			slot = i;
			break;
		}
	}
	mutexUnlock(&s_liveLock);
	return slot;
}

static void live_remove(int slot, void* (*func)(void*), const Thread* self) {
	mutexLock(&s_liveLock);
	if (slot >= 0) {
#if HEN_DEV
		// (this is the ending thread itself; what it used since the last report)
		u64 used = 0;
		if (s_cpuOn && s_live[slot] != NULL &&
		    R_SUCCEEDED(svcGetInfo(&used, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX))) {
			s_cpuEnded += used - (used >= s_cpuLast[slot] ? s_cpuLast[slot] : 0);
			s_cpuEndedCount++;
		}
#endif
		s_live[slot] = NULL;
		s_liveMirror[slot] = NULL;
		s_liveThread[slot] = NULL;
	}
	if (self != NULL && self->stack_mirror != NULL && s_finishedCount < HEN_MAX_FINISHED) {
		s_finished[s_finishedCount].func = func;
		s_finished[s_finishedCount].mirror = self->stack_mirror;
		s_finished[s_finishedCount].mem = self->stack_mem;
		s_finished[s_finishedCount].size = self->stack_sz;
		s_finished[s_finishedCount].left = s_leaving ? self->handle : INVALID_HANDLE;
		++s_finishedCount;
	}
	mutexUnlock(&s_liveLock);
}

// At exit: unmaps the stacks of finished threads still mapped (never joined),
// except a mirror a live thread uses now. Fills funcs (up to max) with their
// start routines; returns how many were released, and their total size.
int hen_thread_release_stacks(void* (**funcs)(void*), int max, size_t* bytes) {
	int released = 0;
	*bytes = 0;
	mutexLock(&s_liveLock);
	for (int i = 0; i < s_finishedCount; i++) {
		bool inUse = false;
		for (int j = 0; j < HEN_MAX_THREADS; j++) {
			inUse |= s_live[j] != NULL && s_liveMirror[j] == s_finished[i].mirror;
		}
		MemoryInfo info;
		u32 page;
		if (inUse) {
			continue;
		}
		// A thread that left from inside a wait is between "off the live list" and its last
		// system call: its stack is released once the kernel says the thread has ended (its
		// handle is signalled then). Its handle is left open: closing it is the owner's
		// (a join), and a second close could hit a handle given out again since.
		if (s_finished[i].left != INVALID_HANDLE) {
			s32 index = -1;
			const Result ended = svcWaitSynchronization(&index, &s_finished[i].left, 1, 500000000ull);
			++hen_thread_left_stats[R_SUCCEEDED(ended) ? 0 : 1];
			if (R_FAILED(ended)) {
				continue;  // (still running on that stack: left mapped)
			}
		}
		if (R_FAILED(svcQueryMemory(&info, &page, (u64) (uintptr_t) s_finished[i].mirror)) ||
		    (info.type & 0xff) != MemType_MappedMemory || info.addr != (u64) (uintptr_t) s_finished[i].mirror) {
			continue;  // closed already (or the address now belongs to someone else)
		}
		// The mapping is the whole of what libnx mirrored: the stack and the
		// page after it (thread-local storage, reent), more than stack_sz.
		const Result rc = svcUnmapMemory(s_finished[i].mirror, s_finished[i].mem, info.size);
		if (R_SUCCEEDED(rc)) {
			if (released < max) {
				funcs[released] = s_finished[i].func;
			}
			++released;
			*bytes += info.size;
		}
	}
	mutexUnlock(&s_liveLock);
	return released;
}

int hen_thread_live(void* (**funcs)(void*), void** args, int max) {
	mutexLock(&s_liveLock);
	int n = 0;
	for (int i = 0; i < HEN_MAX_THREADS; i++) {
		if (s_live[i] != NULL) {
			if (n < max) {
				funcs[n] = s_live[i];
				args[n] = s_liveArg[i];
			}
			n++;
		}
	}
	mutexUnlock(&s_liveLock);
	return n;
}

// At exit, for threads still running after everything was shut down (a worker nobody
// stopped): waits up to wait_ms for them to end by themselves, then pauses each one that is
// left and unmaps its stack. The next program runs in this same process: a thread
// left running keeps executing this program's code over the next one's memory (the
// program started next then crashes at once), and its stack keeps heap pages locked. A paused
// thread is never resumed. Returns how many were stopped this way.
extern void hen_thread_stuck_report(Handle thread, void* start_routine) __attribute__((weak));
int hen_thread_stop_live(int wait_ms) {
	for (int waited = 0; waited < wait_ms; waited += 10) {
		int alive = 0;
		mutexLock(&s_liveLock);
		for (int i = 0; i < HEN_MAX_THREADS; i++) {
			alive += s_live[i] != NULL;
		}
		mutexUnlock(&s_liveLock);
		if (alive == 0) {
			return 0;
		}
		svcSleepThread(10000000);
	}
	int stopped = 0;
	mutexLock(&s_liveLock);
	for (int i = 0; i < HEN_MAX_THREADS; i++) {
		const Thread* t = s_liveThread[i];
		if (s_live[i] == NULL || t == NULL || t->handle == CUR_THREAD_HANDLE || t == threadGetSelf()) {
			continue;
		}
		if (R_FAILED(svcSetThreadActivity(t->handle, ThreadActivity_Paused))) {
			continue;
		}
		++stopped;
		// (where it was: a thread that never ends is stuck in something, and that is the bug)
		if (hen_thread_stuck_report != NULL) {
			hen_thread_stuck_report(t->handle, (void*) s_live[i]);
		}
		MemoryInfo info;
		u32 page;
		if (t->stack_mirror != NULL && R_SUCCEEDED(svcQueryMemory(&info, &page, (u64) (uintptr_t) t->stack_mirror)) &&
		    (info.type & 0xff) == MemType_MappedMemory && info.addr == (u64) (uintptr_t) t->stack_mirror) {
			svcUnmapMemory(t->stack_mirror, t->stack_mem, info.size);
		}
		s_live[i] = NULL;
		s_liveMirror[i] = NULL;
		s_liveThread[i] = NULL;
	}
	mutexUnlock(&s_liveLock);
	return stopped;
}

// Ends the calling thread from wherever it is (an app's exit code wakes a thread left waiting
// and has it call this instead of returning to code that is going away). Its stack goes on the
// finished list, to be unmapped by hen_thread_release_stacks once the thread is gone.
void hen_thread_leave(void) {
	const Thread* self = threadGetSelf();
	int slot = -1;
	void* (*func)(void*) = NULL;
	mutexLock(&s_liveLock);
	for (int i = 0; i < HEN_MAX_THREADS; i++) {
		if (s_live[i] != NULL && s_liveThread[i] == self) {
			slot = i;
			func = s_live[i];
			break;
		}
	}
	mutexUnlock(&s_liveLock);
	if (slot >= 0) {
		s_leaving = true;
		live_remove(slot, func, self);
	}
	svcExitThread();
}

// mimalloc (HEN_MIMALLOC builds): releases the thread's heap at thread exit.
extern void mi_thread_done(void) __attribute__((weak));

static void* hen_thread_trampoline(void* p) {
	const struct hen_thread_start start = *(struct hen_thread_start*) p;
	free(p);
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1 /* no preferred core */, process_core_mask());
	const Thread* self = threadGetSelf();
	const int slot = live_add(start.func, start.arg, self != NULL ? self->stack_mirror : NULL, self);
	void* result = start.func(start.arg);
	live_remove(slot, start.func, self);
	if (mi_thread_done != NULL) {
		mi_thread_done();
	}
	return result;
}

int __wrap___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg, void* stack_addr,
                                   size_t stack_size) {
	if (stack_addr == NULL && stack_size == 0) {
		stack_size = HEN_DEFAULT_THREAD_STACK;
	}
	struct hen_thread_start* start = malloc(sizeof(*start));
	if (start == NULL) {
		return ENOMEM;
	}
	start->func = func;
	start->arg = arg;
	process_core_mask();  // look it up before the thread needs it
	const int rc = __real___syscall_thread_create(thread, hen_thread_trampoline, start, stack_addr, stack_size);
	if (rc != 0) {
		free(start);
	}
	return rc;
}

#if HEN_DEV
// ---- Which host core a thread runs on, and which cores each thread may use ----
//
// The emulation thread needs 6-10% more host time for the same work in some runs; the guess is
// the core the kernel has it on. Three threads count the core they are on at a point they pass
// often (svcGetCurrentProcessorNumber: one short system call), for the "hen: thread:" line.

// 0: off. 1: the emulation thread samples once a slice of the emulated CPU. 2: once a vertical
// blank (chosen at the start if one sample costs 400 ns or more).
int hen_core_sampling;
#define HEN_CORE_KINDS 3 // 0 emulation thread, 1 GPU thread, 2 audio thread
static struct {
	atomic_ullong count[8];
	atomic_ullong changes;
	int last;
} s_core[HEN_CORE_KINDS];

static void core_sample(int kind) {
	const int core = (int) (svcGetCurrentProcessorNumber() & 7);
	atomic_fetch_add_explicit(&s_core[kind].count[core], 1, memory_order_relaxed);
	if (core != s_core[kind].last) {
		if (s_core[kind].last >= 0) {
			atomic_fetch_add_explicit(&s_core[kind].changes, 1, memory_order_relaxed);
		}
		s_core[kind].last = core;
	}
}
// The emulation thread: after a slice (at_vblank 0) and at a vertical blank (1); one of them counts.
void hen_core_sample_emu(int at_vblank) {
	if (hen_core_sampling == 1 + at_vblank) {
		core_sample(0);
	}
}
// The GPU thread (kind 1, once a queue item) and the audio thread (kind 2, once a wake-up).
void hen_core_sample(int kind) {
	if (hen_core_sampling != 0) {
		core_sample(kind);
	}
}
// Turns the sampling on (once), from the emulation thread: times the call first.
void hen_core_sampling_start(void) {
	if (hen_core_sampling != 0) {
		return;
	}
	for (int k = 0; k < HEN_CORE_KINDS; k++) {
		s_core[k].last = -1;
	}
	volatile u32 sink = 0;
	const u64 t0 = armGetSystemTick();
	for (int i = 0; i < 1024; i++) {
		sink += svcGetCurrentProcessorNumber();
	}
	const unsigned ns = (unsigned) (armTicksToNs(armGetSystemTick() - t0) / 1024);
	hen_core_sampling = ns < 400 ? 1 : 2;
	printf("hen: host cores: one sample (svcGetCurrentProcessorNumber) costs %u ns: the emulation thread samples "
	       "once a %s, the GPU thread once a queue item, the audio thread once a wake-up\n",
	       ns, hen_core_sampling == 1 ? "slice of the emulated CPU" : "vertical blank (400 ns or more: too dear for every slice)");
}
// Since the last call: "1:288 3:12" (core:samples, cores without a sample left out) into out;
// returns the core changes seen between two samples, -1 if the thread took no sample.
int hen_core_report(int kind, char* out, size_t size) {
	size_t n = 0;
	unsigned long long total = 0;
	out[0] = 0;
	for (int core = 0; core < 8; core++) {
		const unsigned long long count = atomic_exchange_explicit(&s_core[kind].count[core], 0, memory_order_relaxed);
		total += count;
		if (count != 0 && n < size) {
			n += (size_t) snprintf(out + n, size - n, "%s%d:%llu", n != 0 ? " " : "", core, count);
		}
	}
	const unsigned long long changes = atomic_exchange_explicit(&s_core[kind].changes, 0, memory_order_relaxed);
	return total != 0 ? (int) changes : -1;
}
#endif

// A name for the calling thread in hen_thread_table (from Common::SetCurrentThreadName and by
// hand). A thread not made through pthreads (the program's first) gets the one extra entry.
static Handle s_firstHandle;
static char s_firstName[24];
void hen_thread_name(const char* name) {
	const Thread* self = threadGetSelf();
	bool found = false;
	mutexLock(&s_liveLock);
	for (int i = 0; i < HEN_MAX_THREADS && !found; i++) {
		if (s_live[i] != NULL && s_liveThread[i] == self && self != NULL) {
			snprintf(s_liveName[i], sizeof(s_liveName[i]), "%s", name);
			found = true;
		}
	}
	if (!found && s_firstHandle == 0) {
		s_firstHandle = threadGetCurHandle();
		snprintf(s_firstName, sizeof(s_firstName), "%s", name);
	}
	mutexUnlock(&s_liveLock);
}

#if HEN_DEV
static size_t table_entry(char* line, size_t n, size_t size, const char* name, uintptr_t func, Handle handle,
                          bool self) {
	extern char _start[];
	s32 preferred = -9, priority = -1;
	u64 mask = 0;
	const Result rc = svcGetThreadCoreMask(&preferred, &mask, handle);
	svcGetThreadPriority(&priority, handle);
	if (n >= size) {
		return n;
	}
	if (name[0] != 0) {
		n += (size_t) snprintf(line + n, size - n, " %s", name);
	} else {
		n += (size_t) snprintf(line + n, size - n, " +0x%lx", (unsigned long) (func - (uintptr_t) _start));
	}
	if (n >= size) {
		return size;
	}
	if (R_FAILED(rc)) {
		n += (size_t) snprintf(line + n, size - n, " (rc 0x%x) prio %d;", rc, priority);
	} else {
		n += (size_t) snprintf(line + n, size - n, " 0x%llx/%d prio %d%s;", (unsigned long long) mask, preferred,
		                       priority, self ? " (this thread, the emulation thread)" : "");
	}
	return n < size ? n : size;
}
#endif
// The calling thread's name, for the crash record: no lock taken and nothing allocated
// (the fault handler calls it; the table's lock may be held by the thread that faulted).
// "" when the thread has none.
const char* hen_thread_name_self_nolock(void) {
	const Thread* self = threadGetSelf();
	if (self != NULL) {
		for (int i = 0; i < HEN_MAX_THREADS; i++) {
			if (s_live[i] != NULL && s_liveThread[i] == self) {
				return s_liveName[i];
			}
		}
	}
	if (s_firstHandle != 0 && threadGetCurHandle() == s_firstHandle) {
		return s_firstName;
	}
	return "";
}

#if HEN_DEV
// One line: every thread made through pthreads that is alive (and the first thread, if named):
// name or start routine (+offset in the program, for the .elf), the cores it may run on as a
// mask, its preferred core (-1: none), its priority (lower runs first).
// Returns the number of live threads.
int hen_thread_table(const char* when) {
	static char line[4096];
	size_t n = 0;
	int count = 0;
	const Thread* self = threadGetSelf();
	mutexLock(&s_liveLock);
	if (s_firstHandle != 0) {
		n = table_entry(line, n, sizeof(line), s_firstName, 0, s_firstHandle, false);
	}
	for (int i = 0; i < HEN_MAX_THREADS; i++) {
		const Thread* t = s_liveThread[i];
		if (s_live[i] == NULL || t == NULL) {
			continue;
		}
		count++;
		n = table_entry(line, n, sizeof(line), s_liveName[i], (uintptr_t) s_live[i], t->handle, t == self);
	}
	mutexUnlock(&s_liveLock);
	line[n < sizeof(line) ? n : sizeof(line) - 1] = 0;
	printf("hen: threads: %s: %d made by us alive, process cores 0x%x; name or start routine, core mask/preferred core, priority:%s\n",
	       when, count, process_core_mask(), line);
	return count;
}
// How many threads made through pthreads are alive (for "has the table changed").
int hen_thread_count(void) {
	int count = 0;
	mutexLock(&s_liveLock);
	for (int i = 0; i < HEN_MAX_THREADS; i++) {
		count += s_live[i] != NULL;
	}
	mutexUnlock(&s_liveLock);
	return count;
}

// ---- CPU time of every thread, the cores' idle time, a speed probe ----
//
// For the slow state of the emulation thread (6-10% more host time for the same work in some
// runs), which the host core and the placement of memory do not explain: is another thread of
// ours busier then, or something outside the process; and is the machine itself slower for
// this thread at that time (memory or plain arithmetic). Development builds.

#include <pthread.h>

static u64 s_firstCpuLast;
static u64 s_cpuWallLast;
static bool s_cpuRefusedTold;

static u64 thread_ticks(Handle handle, const char* name, uintptr_t func) {
	extern char _start[];
	u64 ticks = 0;
	const Result rc = svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, UINT64_MAX);
	if (R_FAILED(rc)) {
		if (!s_cpuRefusedTold) {
			s_cpuRefusedTold = true;
			printf("hen: thread cpu: svcGetInfo(ThreadTickCount) refused for %s +0x%lx: rc 0x%x (said once; such "
			       "threads count as 0)\n",
			       name[0] != 0 ? name : "a thread", (unsigned long) (func != 0 ? func - (uintptr_t) _start : 0), rc);
		}
		return 0;
	}
	return ticks;
}

// Turns it on and takes every live thread's CPU time as the starting point (the title's start).
void hen_thread_cpu_start(void) {
	mutexLock(&s_liveLock);
	s_cpuOn = true;
	s_cpuEnded = 0;
	s_cpuEndedCount = 0;
	s_cpuWallLast = armGetSystemTick();
	if (s_firstHandle != 0) {
		s_firstCpuLast = thread_ticks(s_firstHandle, s_firstName, 0);
	}
	for (int i = 0; i < HEN_MAX_THREADS; i++) {
		const Thread* t = s_liveThread[i];
		if (s_live[i] != NULL && t != NULL) {
			s_cpuLast[i] = thread_ticks(t->handle, s_liveName[i], (uintptr_t) s_live[i]);
		}
	}
	mutexUnlock(&s_liveLock);
}

// One line: the CPU time each thread of the table used since the last call, most first, as
// "name@slot ms" (the slot tells threads of one name apart; "first" has none), threads under
// 1 ms left out and summed; the total; the threads that ended meanwhile.
void hen_thread_cpu_report(unsigned seq) {
	static char line[6144];
	static struct {
		u64 ticks;
		int slot;  // -1: the first thread
	} used[HEN_MAX_THREADS + 1];
	static char names[HEN_MAX_THREADS + 1][40];
	extern char _start[];
	int count = 0;
	mutexLock(&s_liveLock);
	const u64 wall = armGetSystemTick();
	const double interval_ms = armTicksToNs(wall - s_cpuWallLast) / 1e6;
	s_cpuWallLast = wall;
	if (s_firstHandle != 0) {
		const u64 now = thread_ticks(s_firstHandle, s_firstName, 0);
		used[count].ticks = now >= s_firstCpuLast ? now - s_firstCpuLast : 0;
		used[count].slot = -1;
		snprintf(names[count], sizeof(names[count]), "%s", s_firstName);
		s_firstCpuLast = now;
		count++;
	}
	for (int i = 0; i < HEN_MAX_THREADS; i++) {
		const Thread* t = s_liveThread[i];
		if (s_live[i] == NULL || t == NULL) {
			continue;
		}
		const u64 now = thread_ticks(t->handle, s_liveName[i], (uintptr_t) s_live[i]);
		used[count].ticks = now >= s_cpuLast[i] ? now - s_cpuLast[i] : 0;
		used[count].slot = i;
		if (s_liveName[i][0] != 0) {
			snprintf(names[count], sizeof(names[count]), "%s@%d", s_liveName[i], i);
		} else {
			snprintf(names[count], sizeof(names[count]), "+0x%lx@%d",
			         (unsigned long) ((uintptr_t) s_live[i] - (uintptr_t) _start), i);
		}
		s_cpuLast[i] = now;
		count++;
	}
	const u64 ended = s_cpuEnded;
	const unsigned endedCount = s_cpuEndedCount;
	s_cpuEnded = 0;
	s_cpuEndedCount = 0;
	mutexUnlock(&s_liveLock);

	// Most first (an insertion sort over the indices: 30 threads).
	static int order[HEN_MAX_THREADS + 1];
	for (int i = 0; i < count; i++) {
		int at = i;
		for (; at > 0 && used[order[at - 1]].ticks < used[i].ticks; at--) {
			order[at] = order[at - 1];
		}
		order[at] = i;
	}
	size_t n = 0;
	u64 total = ended, small = 0;
	int smallCount = 0;
	const u64 one_ms = armNsToTicks(1000000);
	for (int k = 0; k < count; k++) {
		const u64 ticks = used[order[k]].ticks;
		total += ticks;
		if (ticks < one_ms) {
			small += ticks;
			smallCount++;
		} else if (n < sizeof(line)) {
			n += (size_t) snprintf(line + n, sizeof(line) - n, " %s %.1f,", names[order[k]], armTicksToNs(ticks) / 1e6);
		}
	}
	line[n < sizeof(line) ? n : sizeof(line) - 1] = 0;
	printf("hen: thread cpu: #%u interval %.0f ms:%s total %.1f ms in %d threads (%d under 1 ms: %.1f ms; %u ended in "
	       "the interval: %.1f ms)\n",
	       seq, interval_ms, line, armTicksToNs(total) / 1e6, count, smallCount, armTicksToNs(small) / 1e6, endedCount,
	       armTicksToNs(ended) / 1e6);
}

// The cores' idle time. The kernel only answers InfoType_IdleTickCount for the core the asking
// thread is on, so a thread of its own goes to each core of the process in
// turn (svcSetThreadCoreMask to that core alone), once at each report of the emulation thread,
// and prints the line itself. Busy = the span less the idle time: every process's threads on
// that core, ours included, so "busy on all cores" less the total of "hen: thread cpu:" is the
// work of others (and of threads of ours that are not in the table).
static pthread_t s_idleThread;
static bool s_idleStarted;
static Mutex s_idleLock;
static CondVar s_idleWake;
static unsigned s_idleAsked, s_idleDone;
static bool s_idleQuit;

static void* idle_main(void* unused) {
	(void) unused;
	hen_thread_name("core idle");
	const u32 allowed = process_core_mask();
	u64 lastIdle[8] = {0}, lastTick[8] = {0};
	bool have[8] = {false};
	bool told = false;
	for (bool first = true;; first = false) {
		unsigned seq = 0;
		if (!first) {
			mutexLock(&s_idleLock);
			while (s_idleAsked == s_idleDone && !s_idleQuit) {
				condvarWait(&s_idleWake, &s_idleLock);
			}
			const bool quit = s_idleQuit;
			seq = s_idleDone = s_idleAsked;
			mutexUnlock(&s_idleLock);
			if (quit) {
				break;
			}
		}
		char line[320];
		size_t n = 0;
		double busy = 0, span_all = 0;
		int cores = 0;
		Result setRc = 0, infoRc = 0;
		int missed = 0;
		for (int core = 0; core < 8; core++) {
			if ((allowed >> core & 1) == 0) {
				continue;
			}
			const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
			if (R_FAILED(rc)) {
				setRc = rc;
			}
			if ((int) svcGetCurrentProcessorNumber() != core) {
				svcSleepThread(100000);  // (100 us: comes back on a core of the mask)
			}
			u64 idle = 0;
			const bool here = (int) svcGetCurrentProcessorNumber() == core;
			const Result got = svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, (u64) core);
			const u64 tick = armGetSystemTick();
			if (!here) {
				missed++;
			}
			if (R_FAILED(got)) {
				infoRc = got;
				have[core] = false;
				if (n < sizeof(line)) {
					n += (size_t) snprintf(line + n, sizeof(line) - n, " %d:-", core);
				}
				continue;
			}
			if (have[core]) {
				const double idle_ms = armTicksToNs(idle - lastIdle[core]) / 1e6;
				const double span_ms = armTicksToNs(tick - lastTick[core]) / 1e6;
				busy += span_ms - idle_ms;
				span_all += span_ms;
				cores++;
				if (n < sizeof(line)) {
					n += (size_t) snprintf(line + n, sizeof(line) - n, " %d:%.0f/%.0f", core, idle_ms, span_ms);
				}
			}
			have[core] = true;
			lastIdle[core] = idle;
			lastTick[core] = tick;
		}
		const Result back = svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, allowed);
		line[n < sizeof(line) ? n : sizeof(line) - 1] = 0;
		if (first) {
			continue;  // (the starting point)
		}
		if ((R_FAILED(setRc) || R_FAILED(infoRc) || R_FAILED(back) || missed != 0) && !told) {
			told = true;
			printf("hen: core idle: not all of it worked (said once): set core rc 0x%x, idle time rc 0x%x, mask back "
			       "rc 0x%x, %d core(s) the thread did not get to\n",
			       setRc, infoRc, back, missed);
		}
		printf("hen: core idle: #%u idle/span ms per core:%s; busy %.0f ms of %.0f on %d cores (all processes)\n", seq,
		       line, busy, span_all, cores);
	}
	return NULL;
}

void hen_core_idle_start(void) {
	if (s_idleStarted) {
		return;
	}
	s_idleQuit = false;
	s_idleAsked = s_idleDone = 0;
	s_idleStarted = pthread_create(&s_idleThread, NULL, idle_main, NULL) == 0;
	if (!s_idleStarted) {
		printf("hen: core idle: no thread: no line\n");
	}
}
// From the emulation thread's report: one sample of every core, printed by that thread.
void hen_core_idle_ask(unsigned seq) {
	if (!s_idleStarted) {
		return;
	}
	mutexLock(&s_idleLock);
	s_idleAsked = seq;
	condvarWakeOne(&s_idleWake);
	mutexUnlock(&s_idleLock);
}
void hen_core_idle_stop(void) {
	if (!s_idleStarted) {
		return;
	}
	mutexLock(&s_idleLock);
	s_idleQuit = true;
	condvarWakeOne(&s_idleWake);
	mutexUnlock(&s_idleLock);
	pthread_join(s_idleThread, NULL);
	s_idleStarted = false;
}

// A speed probe on the calling thread (the emulation thread, at its report): the same two
// small pieces of work every time, so that a change is the machine's and not the program's.
// - memory: 8192 dependent loads at random places of a 4 MiB table (each waits for the one
//   before: the time of a load that misses the caches), in four parts of 2048;
// - arithmetic: 100000 steps of a shift-and-xor generator in registers (no memory).
// About 1 ms per call. out: ns per load (mean of the four parts), ns per load (fastest part),
// ns per arithmetic step. Returns 0 without a table.
#define HEN_PROBE_ENTRIES (1u << 20)
static u32* s_probeTable;
static u32 s_probeAt;

void hen_probe_start(void) {
	if (s_probeTable != NULL) {
		return;
	}
	u32* table = malloc(HEN_PROBE_ENTRIES * sizeof(u32));
	if (table == NULL) {
		return;
	}
	// One cycle through all entries (Sattolo), from a fixed seed: the same table every run.
	for (u32 i = 0; i < HEN_PROBE_ENTRIES; i++) {
		table[i] = i;
	}
	u64 x = 0x9e3779b97f4a7c15ull;
	for (u32 i = HEN_PROBE_ENTRIES - 1; i > 0; i--) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		const u32 j = (u32) (x % i);
		const u32 t = table[i];
		table[i] = table[j];
		table[j] = t;
	}
	s_probeAt = 0;
	s_probeTable = table;
}
void hen_probe_stop(void) {
	free(s_probeTable);
	s_probeTable = NULL;
}
int hen_probe_run(double* out) {
	const u32* const table = s_probeTable;
	if (table == NULL) {
		return 0;
	}
	u32 at = s_probeAt;
	u64 total = 0, best = UINT64_MAX;
	for (int part = 0; part < 4; part++) {
		const u64 t0 = armGetSystemTick();
		for (int i = 0; i < 2048; i++) {
			at = table[at];
		}
		const u64 took = armGetSystemTick() - t0;
		total += took;
		best = took < best ? took : best;
	}
	s_probeAt = at;
	u64 x = 0x2545f4914f6cdd1dull + at;
	const u64 t1 = armGetSystemTick();
	for (int i = 0; i < 100000; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
	}
	const u64 arithmetic = armGetSystemTick() - t1;
	static volatile u64 sink;
	sink = x;  // (so that the loop is kept)
	out[0] = (double) armTicksToNs(total) / 8192.0;
	out[1] = (double) armTicksToNs(best) / 2048.0;
	out[2] = (double) armTicksToNs(arithmetic) / 100000.0;
	return 1;
}
#endif
