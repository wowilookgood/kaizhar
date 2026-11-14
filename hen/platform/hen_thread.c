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
//   workers all shared one core (measured 2026-11-09: four pipeline workers
//   compiled slower than one). Each new thread first allows itself every core
//   the process may use, with no preferred core, and the scheduler spreads them.
#include <switch.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

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

// Live threads by start routine, for the exit report (hen_app.c): the HEN
// loader runs the next app in this process, and a thread still alive at exit
// keeps running this app's code.
#define HEN_MAX_THREADS 128
static void* (*s_live[HEN_MAX_THREADS])(void*);
static void* s_liveArg[HEN_MAX_THREADS];
static void* s_liveMirror[HEN_MAX_THREADS];
static const Thread* s_liveThread[HEN_MAX_THREADS];  // (for hen_thread_stop_live)
static Mutex s_liveLock;

// Threads whose start routine returned, with their stacks. libnx mirrors a
// thread's stack (svcMapMemory: the heap pages become inaccessible) until the
// thread is closed (pthread_join); a thread nobody joins keeps its stack
// mapped after the app exits, and the next app the HEN loader runs in this
// process finds those heap pages locked (hbmenu: no GPU, black screen).
// hen_thread_release_stacks() unmaps them at exit.
#define HEN_MAX_FINISHED 256
static struct {
	void* (*func)(void*);
	void* mirror;
	void* mem;
	size_t size;
} s_finished[HEN_MAX_FINISHED];
static int s_finishedCount;
// For the exit log: finished, no Thread, in use, not mapped (last type in [4]),
// unmap failed (last rc in [5]).
int hen_thread_stack_stats[6];

static int live_add(void* (*func)(void*), void* arg, void* mirror, const Thread* self) {
	mutexLock(&s_liveLock);
	int slot = -1;
	for (int i = 0; i < HEN_MAX_THREADS; i++) {
		if (s_live[i] == NULL) {
			s_live[i] = func;
			s_liveArg[i] = arg;
			s_liveMirror[i] = mirror;
			s_liveThread[i] = self;
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
		s_live[slot] = NULL;
		s_liveMirror[slot] = NULL;
		s_liveThread[slot] = NULL;
	}
	if (self == NULL || self->stack_mirror == NULL) {
		++hen_thread_stack_stats[1];
	}
	if (self != NULL && self->stack_mirror != NULL && s_finishedCount < HEN_MAX_FINISHED) {
		s_finished[s_finishedCount].func = func;
		s_finished[s_finishedCount].mirror = self->stack_mirror;
		s_finished[s_finishedCount].mem = self->stack_mem;
		s_finished[s_finishedCount].size = self->stack_sz;
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
	hen_thread_stack_stats[0] = s_finishedCount;
	for (int i = 0; i < s_finishedCount; i++) {
		bool inUse = false;
		for (int j = 0; j < HEN_MAX_THREADS; j++) {
			inUse |= s_live[j] != NULL && s_liveMirror[j] == s_finished[i].mirror;
		}
		MemoryInfo info;
		u32 page;
		if (inUse) {
			++hen_thread_stack_stats[2];
			continue;
		}
		if (R_FAILED(svcQueryMemory(&info, &page, (u64) (uintptr_t) s_finished[i].mirror)) ||
		    (info.type & 0xff) != MemType_MappedMemory || info.addr != (u64) (uintptr_t) s_finished[i].mirror) {
			++hen_thread_stack_stats[3];
			hen_thread_stack_stats[4] = (int) info.type;
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
		} else {
			hen_thread_stack_stats[5] = (int) rc;
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
// left and unmaps its stack. The HEN loader runs the next program in this process: a thread
// left running keeps executing this program's code over the next one's memory (seen once:
// the program started next crashed at once), and its stack keeps heap pages locked. A paused
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
