// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// (Switch 2 port) The renderer's shader workers, lent to code outside the renderer.
//
// A first launch stands still while the GPU thread does first-time work that other cores could
// do: compiling a PICA vertex program for the CPU (shader/shader_jit.cpp), decoding a
// compressed texture (rasterizer_cache/texture_codec.h). The Vulkan pipeline cache owns four
// shader workers that are idle most of the time; while it exists it registers a way to queue
// work on them here. With none registered (another renderer, start-up, shutdown) Queue and
// ParallelFor return false and the caller does the work itself.
#pragma once

#include <functional>

#include "common/common_types.h"

namespace VideoCore::HenWorkers {

using QueueFunc = void (*)(void* owner, std::function<void()> work);

/// The pipeline cache's constructor and destructor (nullptr). The owner must finish what was
/// queued before it goes: Clear returns only when no call to its function is in progress.
void Set(QueueFunc func, void* owner);
void Clear(void* owner);

/// Queues work on a shader worker. False: there are none, nothing was queued.
bool Queue(std::function<void()> work);

/// Calls work(0) .. work(count - 1), each once, on the calling thread and on up to three
/// shader workers, and returns when all have returned. The calling thread takes part, so busy
/// workers cost nothing: it then does all of them itself. False: no workers; nothing was
/// called. The calls must not depend on each other's order.
bool ParallelFor(u32 count, const std::function<void(u32)>& work);

} // namespace VideoCore::HenWorkers
