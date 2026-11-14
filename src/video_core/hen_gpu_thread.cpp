// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// (Switch 2 port) The GPU thread, --gpu-thread (off by default).
//
// Why: New 3DS titles keep the one emulation thread busy all the time (Xenoblade Chronicles
// 3D: 61 to 78% speed), and 0.7 to 0.9 s of every 5 s of it went to about 2000 GSP requests:
// PICA command lists run to the end inside the program's system call (register writes, vertex
// loading, vertex shaders on the CPU, the surface cache, recording Vulkan commands). On the
// console the GPU does that while the program carries on, and says so with an interrupt.
//
// The rules:
//  1. One owner. The PICA state, the rasterizer, its surface cache and the Vulkan scheduler's
//     recording side belong to the GPU thread while anything is queued or running (pending
//     != 0), and to the emulation thread otherwise. The emulation thread is the only one that
//     queues, so "pending == 0" stays true for it until it queues again. Nothing in the
//     renderer is locked; everything the emulation thread wants from it goes through Drain
//     first (GPU::Renderer(), register access, the frame swap, page table changes, ...).
//  2. Interrupts follow the work. The GPU thread never touches the kernel or core timing: an
//     interrupt (P3D, PPF, PSC0/1) is noted when its work is finished and signalled by the
//     emulation thread, at the top of a run loop pass or at the vertical blank. The program
//     may reuse a command buffer, vertex array or texture once it has seen the interrupt, and
//     by then the GPU thread has read them.
//  3. The vertical blank waits. GPU::VBlankCallback drains before it signals the blank and
//     swaps, so a frame is presented whole and no work spans a blank in emulated time.
//  4. Idle waits. When no emulated thread can run, emulated time would jump to the next event
//     while the GPU thread has had no host time at all; Poll waits for its next result then.
//  5. Memory the renderer has cached. A read of such a page by the program needs the renderer
//     only if something drawn is waiting to be written back to it: the cache's page bitmap
//     (hen_dirty_pages) is read without a lock for that. A bit the GPU thread sets a moment
//     later belongs to work whose interrupt the program has not seen yet, which on the console
//     is a race too; work whose interrupt was delivered has all its bits set. If a bit is set:
//     Drain, then flush as before. A write must invalidate the cached surface. With the GPU
//     thread idle that is done at once, as before. With it busy the bytes are written and the
//     invalidation is queued behind the work already there (in order with later lists),
//     unless the page has drawn data waiting (then Drain: a download queued earlier must not
//     land on top of the program's bytes).
//
// What this leaves open: the page tables are changed by the GPU thread (a surface registered or removed) while generated code reads
// them. A store by the program to a page in the instant it becomes cached is not seen as an
// invalidation; that needs the program to write a texture while a list that uses it for the
// first time is being drawn, which is a race on the console as well.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <vector>
#include <pthread.h>

#include "common/thread.h"
#include "core/hle/service/gsp/gsp_command.h"
#include "video_core/hen_gpu_thread.h"

#ifdef __SWITCH__
extern "C" int hen_gpu_thread;       // hen/jit/hen_jit.c: --gpu-thread
extern "C" int hen_gpu_defer_writes; // --gpu-thread-sync-writes: 0
#else
static int hen_gpu_thread = 0;
static int hen_gpu_defer_writes = 1;
#endif

#if HEN_DEV
extern "C" void hen_blackbox_watch(void);
#endif

namespace VideoCore {
/// The surface cache's hen_dirty_pages while a cache exists (rasterizer_cache.h).
const u64* hen_dirty_pages_live = nullptr;
/// Set around an invalidation that stands for a run of writes of up to 8 bytes each: the
/// cache then removes the surfaces it touches, as it does for each such write on its own.
bool hen_invalidate_small = false;
} // namespace VideoCore

namespace VideoCore::HenGpu {
namespace {

using Clock = std::chrono::steady_clock;

struct Item {
    bool is_command;
    bool small;
    PAddr addr;
    u32 size;
    Service::GSP::Command command;
};

struct Range {
    PAddr addr;
    u32 size;
    bool small;
};

/// Commands waiting (not counting invalidations): past this the emulation thread waits.
constexpr std::size_t MaxQueuedCommands = 64;
/// Deferred writes since the GPU thread was last idle: past this the emulation thread waits
/// once, so that the pages get uncached and the writes stop leaving generated code.
constexpr u32 MaxDeferredWrites = 8192;
constexpr std::size_t MaxLocalRanges = 32;

std::mutex mutex;
std::condition_variable work_cv; ///< the GPU thread waits here
std::condition_variable done_cv; ///< the emulation thread waits here
std::deque<Item> queue;
std::size_t queued_commands = 0;
std::vector<std::pair<Service::GSP::InterruptId, u64>> finished;
std::exception_ptr failure;
bool stop = false;

std::atomic<u32> pending{0}; ///< items queued and not finished
std::atomic<u32> waiting{0}; ///< the emulation thread is waiting for this thread (Drain, Poll)
std::atomic<bool> have_finished{false};
std::atomic<bool> running{false};
std::atomic<bool> failed{false};
pthread_t thread;
Hooks hooks;

// Emulation thread only.
Range local[MaxLocalRanges];
std::size_t local_count = 0;
u32 deferred_since_idle = 0;

std::atomic<u64> stat_queued{0}, stat_executed{0}, stat_invalidations{0}, stat_busy_ns{0},
    stat_longest_queue{0}, stat_deferred{0}, stat_direct{0}, stat_delivered{0},
    stat_longest_wait_ns{0}, stat_applied{0};
std::atomic<u64> stat_waits[WaitKinds]{}, stat_wait_ns[WaitKinds]{};

u64 Since(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

void NoteWait(Wait reason, Clock::time_point start) {
    const u64 ns = Since(start);
    stat_waits[reason].fetch_add(1, std::memory_order_relaxed);
    stat_wait_ns[reason].fetch_add(ns, std::memory_order_relaxed);
    if (ns > stat_longest_wait_ns.load(std::memory_order_relaxed)) {
        stat_longest_wait_ns.store(ns, std::memory_order_relaxed);
    }
}

void Invalidate(const Range& range) {
    hen_invalidate_small = range.small;
    hooks.invalidate(range.addr, range.size);
    hen_invalidate_small = false;
}

/// (same index as RasterizerCache::HenDirtyAny)
bool DirtyAny(PAddr addr, u32 size) {
    const u64* pages = hen_dirty_pages_live;
    if (pages == nullptr) {
        return true;
    }
    for (u64 page = addr >> 12; page <= (static_cast<u64>(addr) + size - 1) >> 12; page++) {
        if (__atomic_load_n(&pages[(page & 0x1FFFF) >> 6], __ATOMIC_ACQUIRE) &
            (1ull << (page & 63))) {
            return true;
        }
    }
    return false;
}

void* Main(void*) {
    Common::SetCurrentThreadName("GpuThread");
#if HEN_DEV
    hen_blackbox_watch(); // (hen/jit/hen_jit.c: this thread does the rendering work now; sampled for stall dumps)
#endif
    for (;;) {
        Item item;
        {
            std::unique_lock lock{mutex};
            work_cv.wait(lock, [] { return stop || !queue.empty(); });
            if (queue.empty()) {
                break;
            }
            item = queue.front();
            queue.pop_front();
            if (item.is_command) {
                queued_commands--;
            }
        }
        const auto start = Clock::now();
        // (Vulkan-Hpp throws on errors. After one, the rest of the queue is dropped and the
        // emulation thread gets the exception at its next Drain or Poll, where the frontend
        // catches it as it does its own.)
        if (!failed.load(std::memory_order_relaxed)) {
            try {
                if (item.is_command) {
                    hooks.execute(item.command);
                    stat_executed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    Invalidate(Range{item.addr, item.size, item.small});
                    stat_applied.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                std::scoped_lock lock{mutex};
                failure = std::current_exception();
                failed = true;
            }
        }
        stat_busy_ns.fetch_add(Since(start), std::memory_order_relaxed);
        {
            std::scoped_lock lock{mutex};
            pending.fetch_sub(1);
        }
        done_cv.notify_all();
    }
    return nullptr;
}

void Rethrow() {
    if (!failed.load(std::memory_order_relaxed)) {
        return;
    }
    std::exception_ptr error;
    {
        std::scoped_lock lock{mutex};
        std::swap(error, failure);
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

/// The invalidations noted by DeferWrite go to the renderer: directly when it is the
/// emulation thread's, else onto the queue. They are kept back until here so that the GPU
/// thread cannot act on one before the program's bytes are in memory (the write follows the
/// call that notes it), and so that a run of small writes becomes one item.
void PublishLocal() {
    if (local_count == 0) {
        return;
    }
    if (pending.load() == 0) {
        for (std::size_t i = 0; i < local_count; i++) {
            Invalidate(local[i]);
        }
        local_count = 0;
        return;
    }
    {
        std::scoped_lock lock{mutex};
        for (std::size_t i = 0; i < local_count; i++) {
            queue.push_back(Item{false, local[i].small, local[i].addr, local[i].size, {}});
        }
        pending.fetch_add(static_cast<u32>(local_count));
    }
    stat_invalidations.fetch_add(local_count, std::memory_order_relaxed);
    local_count = 0;
    work_cv.notify_one();
}

void Deliver() {
    if (!have_finished.load()) {
        return;
    }
    std::vector<std::pair<Service::GSP::InterruptId, u64>> list;
    {
        std::scoped_lock lock{mutex};
        list.swap(finished);
        have_finished = false;
    }
    for (const auto& [id, delay_ns] : list) {
        hooks.deliver(id, delay_ns);
    }
    stat_delivered.fetch_add(list.size(), std::memory_order_relaxed);
}

} // namespace

bool Start(Hooks hooks_) {
    if (running) {
        return true;
    }
    hooks = std::move(hooks_);
    stop = false;
    failed = false;
    failure = nullptr;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // (what the emulation thread has, citra_switch/main.cpp: this thread runs the code that
    // needed it, shader generation among it)
    pthread_attr_setstacksize(&attr, 64 * 1024 * 1024);
    const int err = pthread_create(&thread, &attr, &Main, nullptr);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        std::printf("hen: gputhread: couldn't create the thread (%d): staying on the emulation "
                    "thread\n",
                    err);
        hooks = {};
        return false;
    }
    running = true;
    std::printf("hen: gputhread: started (writes to cached pages: %s)\n",
                hen_gpu_defer_writes ? "deferred" : "wait");
    return true;
}

void Stop() {
    if (!running) {
        return;
    }
    try {
        Drain(WaitOther);
    } catch (...) {
        // (the title is ending anyway)
    }
    {
        std::scoped_lock lock{mutex};
        stop = true;
    }
    work_cv.notify_all();
    pthread_join(thread, nullptr);
    running = false;
    queue.clear();
    queued_commands = 0;
    finished.clear();
    have_finished = false;
    pending = 0;
    local_count = 0;
    deferred_since_idle = 0;
    hooks = {};
    std::printf("hen: gputhread: stopped\n");
}

bool Active() {
    return running.load(std::memory_order_relaxed);
}

bool Behind() {
    return waiting.load(std::memory_order_relaxed) != 0 || pending.load(std::memory_order_relaxed) > 1;
}

bool OnThread() {
    return running.load(std::memory_order_relaxed) && pthread_equal(pthread_self(), thread);
}

void Queue(const Service::GSP::Command& command) {
    PublishLocal();
    std::size_t depth;
    {
        std::unique_lock lock{mutex};
        if (queued_commands >= MaxQueuedCommands) {
            const auto start = Clock::now();
            done_cv.wait(lock, [] { return queued_commands < MaxQueuedCommands; });
            NoteWait(WaitFull, start);
        }
        queue.push_back(Item{true, false, 0, 0, command});
        queued_commands++;
        pending.fetch_add(1);
        depth = queue.size();
    }
    work_cv.notify_one();
    stat_queued.fetch_add(1, std::memory_order_relaxed);
    if (depth > stat_longest_queue.load(std::memory_order_relaxed)) {
        stat_longest_queue.store(depth, std::memory_order_relaxed);
    }
}

void Drain(Wait reason) {
    if (!Active() || OnThread()) {
        return;
    }
    PublishLocal();
    if (pending.load() != 0) {
        const auto start = Clock::now();
        std::unique_lock lock{mutex};
        waiting.fetch_add(1);
        done_cv.wait(lock, [] { return pending.load() == 0; });
        waiting.fetch_sub(1);
        lock.unlock();
        NoteWait(reason, start);
    }
    deferred_since_idle = 0;
    Rethrow();
}

void Poll(bool all_idle) {
    if (!Active()) {
        return;
    }
    PublishLocal();
    if (all_idle && pending.load() != 0 && !have_finished.load()) {
        const auto start = Clock::now();
        std::unique_lock lock{mutex};
        waiting.fetch_add(1);
        done_cv.wait(lock, [] { return pending.load() == 0 || have_finished.load(); });
        waiting.fetch_sub(1);
        lock.unlock();
        NoteWait(WaitIdle, start);
    }
    Deliver();
    Rethrow();
}

void DrainAndDeliver(Wait reason) {
    if (!Active()) {
        return;
    }
    Drain(reason);
    Deliver();
}

void PostInterrupt(Service::GSP::InterruptId id, u64 delay_ns) {
    {
        std::scoped_lock lock{mutex};
        finished.emplace_back(id, delay_ns);
        have_finished = true;
    }
    done_cv.notify_all();
}

bool ReadIsClean(PAddr addr, u32 size) {
    // (the size limit is the one RasterizerCache::FlushRegion has for the same test)
    return size <= 0x10000 && !DirtyAny(addr, size);
}

bool DeferWrite(PAddr addr, u32 size) {
    if (size == 0) {
        return true;
    }
    if (pending.load() == 0) {
        PublishLocal();
        deferred_since_idle = 0;
        stat_direct.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!hen_gpu_defer_writes || deferred_since_idle >= MaxDeferredWrites ||
        static_cast<u64>(addr) + size > 0xFFFFFFFFull || DirtyAny(addr, size)) {
        Drain(WaitWrite);
        return false;
    }
    deferred_since_idle++;
    stat_deferred.fetch_add(1, std::memory_order_relaxed);
    const bool small = size <= 8;
    // (a loop storing words or bytes in address order, either direction: one growing range)
    for (std::size_t i = local_count; i-- > 0;) {
        Range& range = local[i];
        if (range.small != small || addr > range.addr + range.size || addr + size < range.addr) {
            continue;
        }
        const PAddr end = std::max(range.addr + range.size, addr + size);
        range.addr = std::min(range.addr, addr);
        range.size = end - range.addr;
        return true;
    }
    if (local_count == MaxLocalRanges) {
        PublishLocal();
    }
    local[local_count++] = Range{addr, size, small};
    return true;
}

void Stats(unsigned long long* out) {
    out[0] = stat_queued.exchange(0);
    out[1] = stat_executed.exchange(0);
    out[2] = stat_invalidations.exchange(0);
    out[3] = stat_busy_ns.exchange(0);
    out[4] = stat_longest_queue.exchange(0);
    out[5] = stat_deferred.exchange(0);
    out[6] = stat_direct.exchange(0);
    out[7] = stat_delivered.exchange(0);
    out[8] = stat_longest_wait_ns.exchange(0);
    out[9] = stat_applied.exchange(0);
    for (int i = 0; i < WaitKinds; i++) {
        out[10 + 2 * i] = stat_waits[i].exchange(0);
        out[11 + 2 * i] = stat_wait_ns[i].exchange(0);
    }
}

} // namespace VideoCore::HenGpu
