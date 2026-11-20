// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Switch 2 homebrew port, development builds: what went on during a gap between two frames
// shown. Each kind is a running total of nanoseconds that is never reset; the renderer keeps
// the totals as they were at the last frame, and when a frame comes more than 40 ms after the
// one before it prints the differences ("hen: gap: ..." in renderer_vulkan.cpp). The totals are
// in hen/jit/hen_jit.c. Other builds: the calls are empty.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum HenGapKind {
    HEN_GAP_JIT_COMPILE,    // blocks compiled on the emulation thread (translating and emitting)
    HEN_GAP_JIT_LOCK,       // waiting for the compile lock, outside the worker
    HEN_GAP_INTERPRETER,    // code run by the interpreter (not compiled yet)
    HEN_GAP_PIPELINES,      // pipelines compiled by the driver (the workers' time, summed)
    HEN_GAP_SHADERS,        // shaders generated and compiled (the workers' time, summed)
    HEN_GAP_PIPELINE_WAIT,  // the render thread waiting for a pipeline
    HEN_GAP_CPU_DRAWS,      // draws whose vertex shader ran on the CPU
    HEN_GAP_UPLOADS,        // textures uploaded to the GPU
    HEN_GAP_DOWNLOADS,      // surfaces read back from the GPU (submitting and waiting)
    HEN_GAP_DRIVER_WAIT,    // waiting in the driver for the GPU
    HEN_GAP_CACHE_FLUSH,    // the program read memory the renderer has cached
    HEN_GAP_GPU_THREAD,     // the emulation thread waiting for the GPU thread
    HEN_GAP_TITLE_READS,    // the title's file reads done on the emulation thread
    HEN_GAP_FILE_CALLS,     // every fread, fwrite, fflush and fclose, on any thread
    HEN_GAP_SERVICE_CALLS,  // the emulator's service calls (the emulation thread inside them)
    HEN_GAP_PUBLISH,        // requests that hand new JIT code to the system (the caller waits)
    HEN_GAP_PUBLISH_COUNT,  // (a count of those, not a time)
    HEN_GAP_KINDS
};
enum { HEN_GAP_LONGEST_FILE, HEN_GAP_LONGEST_SERVICE, HEN_GAP_LONGEST_KINDS };

#if defined(__SWITCH__) && HEN_DEV
void hen_gap_add(int kind, unsigned long long ns);
// The longest single file call / service call since the last frame, with what it was.
void hen_gap_longest(int which, unsigned long long ns, const char* what, unsigned detail);
// The totals now, and the longest of each (taken: they start again). what: 64 bytes each.
void hen_gap_read(unsigned long long* totals, unsigned long long* longest_ns, char (*what)[64]);
// The time the emulation thread [0] and the JIT's compile worker [1] have had a core, in all, ns.
void hen_gap_thread_cpu(unsigned long long* out);
// One kind's running total as it is now (nothing is taken).
unsigned long long hen_gap_total(int kind);
#else
static inline void hen_gap_add(int kind, unsigned long long ns) {
    (void)kind;
    (void)ns;
}
static inline void hen_gap_longest(int which, unsigned long long ns, const char* what, unsigned detail) {
    (void)which;
    (void)ns;
    (void)what;
    (void)detail;
}
#endif

#ifdef __cplusplus
}
#endif
