// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Switch 2 homebrew port: timing counters for the frontend's periodic report
// (citra_switch/main.cpp).
#pragma once

#include <chrono>
#include <cstdio>

enum HenGfxKind {
    HEN_GFX_PIPELINE_COMPILE, ///< a pipeline compiled by the driver (worker thread)
    HEN_GFX_PIPELINE_CACHED,  ///< a pipeline the driver had ready (its cache)
    HEN_GFX_PIPELINE_WAIT,    ///< the render thread waiting for a pipeline
    HEN_GFX_SHADER_COMPILE,   ///< a shader generated and compiled (worker thread)
    HEN_GFX_SOFTWARE_DRAW,    ///< a draw whose vertex shader ran on the CPU (emulation thread)
    HEN_GFX_SOFTWARE_VERTICES, ///< (the "time" is the number of vertices of those draws)
    HEN_GFX_MEM_FLUSH,        ///< the program read memory the renderer has cached
    HEN_GFX_MEM_INVALIDATE,   ///< the program wrote memory the renderer has cached
    HEN_GFX_MEM_READ_CALL,    ///< a memory read that left generated code (count only)
    HEN_GFX_MEM_WRITE_CALL,   ///< a memory write that left generated code (count only)
    HEN_GFX_DOWNLOAD_FINISH,  ///< a surface download: submitting and waiting for the GPU
    HEN_GFX_GPU_WAIT,         ///< of that (and other waits): the driver's wait itself
    HEN_GFX_DOWNLOAD_ENCODE,  ///< a surface download: converting the pixels into guest memory
    HEN_GFX_CPU_INSTEAD,      ///< a draw shaded on the CPU because its pipeline wasn't ready (count)
    HEN_GFX_DOWNLOAD_DEPTH,   ///< of the downloads: those of a depth and stencil surface
    HEN_GFX_FILE_SYNC,        ///< a file read done on the emulation thread
    HEN_GFX_FILE_ASYNC,       ///< a file read done on another thread (the program's thread sleeps)
    HEN_GFX_FILE_BYTES,       ///< (the "time" is the bytes of those reads)
    HEN_GFX_DRAW_SKIPPED,     ///< a draw left out because its pipeline wasn't ready (the "time" is its vertices)
    HEN_GFX_DOWNLOAD_LAGGED,  ///< a depth download answered with the previous one's data (count)
    HEN_GFX_READ_TABLE_KEPT,  ///< a page the renderer began to cache stayed readable from generated code (count)
    HEN_GFX_READ_TABLE_CLEARED, ///< such a page was drawn to: reads of it leave generated code again (count)
    HEN_GFX_READ_TABLE_RESTORED, ///< what was drawn was written back: readable again (count)
    HEN_GFX_KINDS
};

#ifdef __SWITCH__
extern "C" void hen_gfx_note(int kind, unsigned long long ns);
#if HEN_DEV
extern "C" void hen_blackbox_dump(unsigned ms); // hen/jit/hen_jit.c
#endif
/// Adds the time from its construction to its destruction to a counter.
struct HenGfxTimer {
    explicit HenGfxTimer(int kind_) : kind{kind_}, start{std::chrono::steady_clock::now()} {}
    ~HenGfxTimer() {
        hen_gfx_note(kind, std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count());
    }
    int kind;
    std::chrono::steady_clock::time_point start;
};
/// Says so if the time from its construction to its destruction is over the limit: for the
/// emulation thread's work outside the emulated CPU, where one long call is one stutter.
/// Development builds only (HEN_DEV): nothing otherwise.
#if HEN_DEV
struct HenSlow {
    explicit HenSlow(const char* what_, int limit_ms_ = 20)
        : what{what_}, limit_ms{limit_ms_}, start{std::chrono::steady_clock::now()} {}
    ~HenSlow() {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        if (ms >= limit_ms) {
            std::printf("hen: slow: %lld ms in %s\n", static_cast<long long>(ms), what);
            if (ms >= 35) { // (under 80 ms: once a second at most)
                hen_blackbox_dump(static_cast<unsigned>(ms)); // (where the thread was meanwhile)
            }
        }
    }
    const char* what;
    int limit_ms;
    std::chrono::steady_clock::time_point start;
};
#else
struct HenSlow {
    explicit HenSlow(const char*, int = 20) {}
};
#endif
#else
inline void hen_gfx_note(int, unsigned long long) {}
struct HenGfxTimer {
    explicit HenGfxTimer(int) {}
};
struct HenSlow {
    explicit HenSlow(const char*, int = 20) {}
};
#endif
