// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/arch.h"
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)

#include <chrono>
#include <utility>
#include "common/assert.h"
#include "common/hash.h"
#include "common/microprofile.h"
#include "video_core/shader/shader.h"
#include "video_core/shader/shader_jit.h"
#if CITRA_ARCH(arm64)
#include "video_core/shader/shader_jit_a64_compiler.h"
#endif
#if CITRA_ARCH(x86_64)
#include "video_core/shader/shader_jit_x64_compiler.h"
#endif

#ifdef __SWITCH__
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include "video_core/hen_workers.h"
#include "video_core/shader/shader_interpreter.h"

// (hen/jit/hen_jit.c) --vs-jit-async, off by default: see JitEngine::SetupBatch.
extern "C" int hen_vs_jit_async;
extern "C" int hen_vs_jit_small;
extern "C" unsigned long long hen_vs_jit_stats[6];
namespace Pica::Shader {
/// The vertices of the draw that the next SetupBatch is for (pica_core.cpp, LoadVertices);
/// 0: not said, the program is compiled before SetupBatch returns, as upstream.
u32 hen_setup_vertices = 0;
} // namespace Pica::Shader
#endif

namespace Pica::Shader {

#ifdef __SWITCH__
/// One program handed to a shader worker. The worker and the engine each hold it, so either
/// may go first. The worker only emits code into the shader's vector; the executable memory is
/// made by the engine's thread (JitShader::HenFinalize).
struct JitEngine::HenJob {
    enum : int { Queued, Running, Done };
    std::array<u32, MAX_PROGRAM_CODE_LENGTH> program_code;
    std::array<u32, MAX_SWIZZLE_DATA_LENGTH> swizzle_data;
    std::unique_ptr<JitShader> shader;
    std::atomic<int> state{Queued};
    std::mutex mutex;
    std::condition_variable done;

    /// Whoever takes the job from Queued compiles it: the worker, or the engine's thread when
    /// it cannot wait for a worker to get to it.
    void Run() {
        int expected = Queued;
        if (!state.compare_exchange_strong(expected, Running)) {
            return;
        }
        auto compiled = std::make_unique<JitShader>();
        compiled->Compile(&program_code, &swizzle_data, false);
        {
            std::scoped_lock lock{mutex};
            shader = std::move(compiled);
            state = Done;
        }
        done.notify_all();
    }
};
#endif

JitEngine::JitEngine() = default;
JitEngine::~JitEngine() = default;

void JitEngine::SetupBatch(ShaderSetup& setup, u32 entry_point) {
    ASSERT(entry_point < MAX_PROGRAM_CODE_LENGTH);
    setup.entry_point = entry_point;
#ifdef __SWITCH__
    const u32 vertices = std::exchange(hen_setup_vertices, 0); // (said for this call only)
#endif

    setup.DoProgramCodeFixup();
    const u64 code_hash = setup.GetProgramCodeHash();
    const u64 swizzle_hash = setup.GetSwizzleDataHash();

    const u64 cache_key = Common::HashCombine(code_hash, swizzle_hash);
    auto iter = cache.find(cache_key);
    if (iter != cache.end()) {
        setup.cached_shader = iter->second.get();
    } else {
#ifdef __SWITCH__
        // (Switch 2 port, --vs-jit-async; off by default) A program's first use cost the GPU
        // thread its whole compile: 50 of 340 ms sampled in the gaps of six cold routes, four of
        // eight samples in Pokemon Omega Ruby's title scene. With the switch the
        // compile is a shader worker's job, and until it is done:
        //  - a draw of hen_vs_jit_small vertices or fewer runs the program in the interpreter
        //    (Run below, cached_shader null). The interpreter is the reference implementation
        //    but NOT bit-identical to this JIT: it sums a dot product in order where the JIT
        //    adds pairs, and uses the C library's exp2 and log2 where the JIT has its own
        //    approximations. A vertex can differ in its last bits for the frame or two the
        //    compile takes; that is why this is not the default.
        //  - a larger draw (the interpreter costs about three times the JIT a vertex) waits
        //    for the job, or compiles it here if no worker has started it: as without the switch.
        const auto job_iter = hen_jobs.find(cache_key);
        if (job_iter != hen_jobs.end()) {
            const std::shared_ptr<HenJob> job = job_iter->second;
            const bool small = vertices != 0 && vertices <= static_cast<u32>(hen_vs_jit_small);
            if (job->state.load() != HenJob::Done && small) {
                setup.cached_shader = nullptr;
#if HEN_DEV
                hen_vs_jit_stats[1]++;
                hen_vs_jit_stats[2] += vertices;
#endif
                return;
            }
            if (job->state.load() != HenJob::Done) {
#if HEN_DEV
                hen_vs_jit_stats[3]++;
#endif
                job->Run(); // (nothing if a worker has it: then the wait below)
                std::unique_lock lock{job->mutex};
                job->done.wait(lock, [&] { return job->state.load() == HenJob::Done; });
            }
            std::unique_ptr<JitShader> shader;
            {
                std::scoped_lock lock{job->mutex};
                shader = std::move(job->shader);
            }
            hen_jobs.erase(job_iter);
            shader->HenFinalize();
            setup.cached_shader = shader.get();
            cache.emplace(cache_key, std::move(shader));
            return;
        }
        if (hen_vs_jit_async && vertices != 0 && vertices <= static_cast<u32>(hen_vs_jit_small)) {
            auto job = std::make_shared<HenJob>();
            job->program_code = setup.GetProgramCode();
            job->swizzle_data = setup.GetSwizzleData();
            if (VideoCore::HenWorkers::Queue([job] { job->Run(); })) {
                hen_jobs.emplace(cache_key, std::move(job));
                setup.cached_shader = nullptr;
#if HEN_DEV
                hen_vs_jit_stats[0]++;
                hen_vs_jit_stats[1]++;
                hen_vs_jit_stats[2] += vertices;
#endif
                return;
            }
        }
#if HEN_DEV
        const auto hen_start = std::chrono::steady_clock::now();
#endif
#endif
        auto shader = std::make_unique<JitShader>();
        shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());
        setup.cached_shader = shader.get();
        cache.emplace_hint(iter, cache_key, std::move(shader));
#if defined(__SWITCH__) && HEN_DEV
        hen_vs_jit_stats[4]++;
        hen_vs_jit_stats[5] += static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 hen_start)
                .count());
#endif
    }
}

MICROPROFILE_DECLARE(GPU_Shader);

void JitEngine::Run(const ShaderSetup& setup, ShaderUnit& state) const {
#ifdef __SWITCH__
    if (setup.cached_shader == nullptr && hen_vs_jit_async) {
        // (its compiled form is still a worker's job: SetupBatch above)
        static const InterpreterEngine interpreter;
        interpreter.Run(setup, state);
        return;
    }
#endif
    ASSERT(setup.cached_shader != nullptr);

    MICROPROFILE_SCOPE(GPU_Shader);

    const JitShader* shader = static_cast<const JitShader*>(setup.cached_shader);
    shader->Run(setup, state, setup.entry_point);
}

} // namespace Pica::Shader

#endif // CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
