// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "video_core/hen_workers.h"

namespace VideoCore::HenWorkers {
namespace {

std::mutex hook_mutex;
QueueFunc hook_func = nullptr;
void* hook_owner = nullptr;

/// One ParallelFor. The helpers hold it by shared_ptr: one that starts after the caller has
/// returned finds no index left and touches nothing else.
struct Batch {
    explicit Batch(u32 count_, const std::function<void(u32)>* work_)
        : count{count_}, work{work_} {}

    void Run() {
        for (;;) {
            const u32 index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= count) {
                return;
            }
            (*work)(index);
            if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == count) {
                std::scoped_lock lock{mutex};
                finished.notify_all();
            }
        }
    }

    const u32 count;
    const std::function<void(u32)>* const work; ///< (the caller's: valid until done == count)
    std::atomic<u32> next{0};
    std::atomic<u32> done{0};
    std::mutex mutex;
    std::condition_variable finished;
};

} // namespace

void Set(QueueFunc func, void* owner) {
    std::scoped_lock lock{hook_mutex};
    hook_func = func;
    hook_owner = owner;
}

void Clear(void* owner) {
    std::scoped_lock lock{hook_mutex};
    if (hook_owner == owner) {
        hook_func = nullptr;
        hook_owner = nullptr;
    }
}

bool Queue(std::function<void()> work) {
    // (the lock is held across the call: Clear then cannot return while a worker pool that is
    // about to go is still being handed work)
    std::scoped_lock lock{hook_mutex};
    if (hook_func == nullptr) {
        return false;
    }
    hook_func(hook_owner, std::move(work));
    return true;
}

bool ParallelFor(u32 count, const std::function<void(u32)>& work) {
    if (count == 0) {
        return true;
    }
    const auto batch = std::make_shared<Batch>(count, &work);
    const u32 helpers = std::min<u32>(count - 1, 3);
    for (u32 i = 0; i < helpers; i++) {
        if (!Queue([batch] { batch->Run(); })) {
            if (i == 0) {
                return false;
            }
            break;
        }
    }
    batch->Run();
    std::unique_lock lock{batch->mutex};
    batch->finished.wait(lock, [&] { return batch->done.load(std::memory_order_acquire) == count; });
    return true;
}

} // namespace VideoCore::HenWorkers
