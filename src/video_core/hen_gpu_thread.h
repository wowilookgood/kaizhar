// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// (Switch 2 port) --gpu-thread: PICA command lists, memory fills and display transfers run on
// a thread of their own instead of inside the program's system call. See hen_gpu_thread.cpp
// for the rules; everything here is a no-op returning at once while the thread is not running.

#include <functional>

#include "common/common_types.h"
#include "core/hle/service/gsp/gsp_interrupt.h"

namespace Service::GSP {
struct Command;
}

namespace VideoCore::HenGpu {

/// Why the emulation thread waited for the GPU thread (the "hen: gputhread:" line).
enum Wait {
    WaitVblank, ///< the vertical blank: the frame is presented complete
    WaitIdle,   ///< no emulated thread could run: the program is waiting for the GPU
    WaitRead,   ///< the program read memory the GPU has drawn to and not written back
    WaitWrite,  ///< the program wrote memory the renderer has cached, and it could not be deferred
    WaitFull,   ///< the queue was full
    WaitMap,    ///< a change to a page table
    WaitRegs,   ///< a GPU register read or written directly, a DMA request
    WaitOther,  ///< anything else that reaches the renderer (settings, title start and end)
    WaitKinds
};

struct Hooks {
    /// A queued GSP command, on the GPU thread.
    std::function<void(const Service::GSP::Command&)> execute;
    /// An invalidation of guest memory (what a write by the program asks for), on whichever
    /// thread owns the renderer at the time.
    std::function<void(PAddr, u32)> invalidate;
    /// An interrupt whose work is done, on the emulation thread.
    Service::GSP::InterruptHandler deliver;
};

/// Starts the thread (emulation thread). False: it could not be created; nothing changes.
bool Start(Hooks hooks);
/// Finishes the queue, stops and joins the thread (emulation thread). Completions not yet
/// delivered are dropped: this is the end of the title.
void Stop();

/// Whether the thread is running.
bool Active();
/// Whether the caller is the GPU thread.
bool OnThread();
/// True while the emulation thread waits for the GPU thread, or more work is queued behind what
/// it is doing: nothing optional should be done on it now.
bool Behind();

/// Emulation thread: hands a command to the GPU thread.
void Queue(const Service::GSP::Command& command);
/// Emulation thread: returns when the GPU thread has nothing left to do. From then until the
/// next Queue the renderer belongs to the emulation thread. A no-op on the GPU thread.
void Drain(Wait reason);
/// Emulation thread, once per run loop pass: delivers the interrupts of finished work.
/// all_idle: no emulated thread ran in the last pass; then this waits for the GPU thread's
/// next result first, so emulated time does not run ahead of work the program is waiting for.
void Poll(bool all_idle);
/// Emulation thread, at the vertical blank: Drain, then deliver.
void DrainAndDeliver(Wait reason);

/// GPU thread: an interrupt to signal once the emulation thread gets to it.
void PostInterrupt(Service::GSP::InterruptId id, u64 delay_ns);

/// Emulation thread: the program is about to read this memory. True: nothing drawn is waiting
/// to be written back to it, so the read needs nothing from the renderer.
bool ReadIsClean(PAddr addr, u32 size);
/// Emulation thread: the program is about to write this memory, which the renderer has cached.
/// True: the invalidation was taken over (it reaches the renderer in order, later). False: the
/// renderer is the caller's now (the GPU thread is idle); invalidate as without the thread.
bool DeferWrite(PAddr addr, u32 size);

/// For the report: see ReportGfx in citra_switch/main.cpp. Resets the counters.
void Stats(unsigned long long* out);
constexpr int StatCount = 10 + 2 * WaitKinds;

} // namespace VideoCore::HenGpu
