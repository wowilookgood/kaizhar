// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Kaizhar (Azahar for Switch 2 homebrew): a minimal frontend. Opened from hbmenu it shows the
// title picker (title_menu.cpp). With a program on the command line (as nxlink passes it) it
// runs that program and exits:
//
//     kaizhar.nro [<path to .3dsx/.cci/.cxi/...> | --title <id in hex>] [options]
//
// Options of every build:
//   Running:   --renderer sw  --interpreter  --no-audio  --res N  --layout N  --keep-awake
//              --debug-log  --log FILTER  --n3ds-clock  --n3ds-clock-off  --n3ds-kernel-auto  --n3ds-handheld N
//   CPU JIT:   --no-jit-list  --fresh-jit-list  --jit-start-ms N  --no-jit-speculation
//              --no-fast-dispatch  --translation-worker N  --jit-worker-depth N  --jit-ahead N
//              --jit-tier N  --jit-bump N  --standin-rule N  --substitute  --no-jit-alias
//              --no-link-table  --no-direct-links  --no-inline-exclusive  --no-live-fpsr
//              --no-lazy-resched  --no-read-table  --no-fold-cond-branch  --no-code-align-2m
//              --no-compact-links
//   Shaders:   --cold-shaders  --sync-shaders  --sw-shaders  --shader-jit  --no-shader-jit
//              --cpu-vertex-limit N  --cpu-shade-share N  --no-generic-fs  --generic-all
//              --generic-first  --no-generic-first  --no-generic-seed  --stale-profile
//              --whole-pipelines  --pipe-dedup  --no-pipe-dedup
//   Renderer:  --gpu-thread  --no-gpu-thread  --gpu-thread-sync-writes  --sync-depth-reads
//              --no-depth-scale-blit  --max-lag-ms N
//   Host:      --load-boost  --no-load-boost  --load-boost-system  --boost-strict
//              --boost-linger MS  --no-boost-cancel  --no-handheld-hold  --cpu-boost N  --no-file-defer
//              --no-fs-canon  --no-pointer  --no-wake-all  --cia-chunk-kib N
//
// Options of development builds only (HEN_DEV):
//   Test runs: --hen-exit-after SECONDS  --repeat N  --director PORT  --movie-record NAME
//              --movie-play NAME  --screenshot-at S[,S...]  --screenshot-frame N[,N...]
//              --cold-driver  --keyboard-test  --crash-test  --mic N  --mic-probe
//   Logging:   --profile  --profile-pipelines  --no-blackbox  --clock-info  --jit-trace
//              --exit-counts
//              --pipe-log  --dump-spirv  --dump-spirv-all  --crumbs  --vk-trace
//
// User data is under /switch/Kaizhar/ on the SD card (a /switch/Azahar folder of the earlier
// test builds is renamed once at start: switch_platform.c).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <atomic>
#include <string>
#include <vector>

#include <pthread.h>

#include <fmt/format.h>

#include "audio_core/input_details.h"
#include "audio_core/sink_details.h"
#include "common/hen_stats.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/arm/dynarmic/hen_block_list.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/wait_object.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/applets/swkbd.h"
#include "core/hle/applets/erreula.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/image_interface.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/service.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "input_common/main.h"
#include "citra_switch/emu_window_switch.h"
#include "citra_switch/input_switch.h"
#include "citra_switch/switch_platform.h"
#include "citra_switch/title_menu.h"
#include "video_core/gpu.h"
#include "video_core/hen_gpu_thread.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

// Where the Vulkan renderer gets its functions (vk_platform.cpp): null for the driver itself,
// or the call tracer (hen/vktrace).
extern "C" {
using VulkanEntryPoint = void (*(*)(void* instance, const char* name))();
VulkanEntryPoint hen_vulkan_entry_point = nullptr;
#if HEN_DEV
void (*hen_vktrace_get_instance_proc_addr(void* instance, const char* name))();
#endif
}

// --director PORT (also --hen-chain PORT): chained test runs, development builds only. The
// code is hen-chain's, a local library outside this repository (HEN_CHAIN_DIR in CMakeLists.txt
// here; HEN_CHAIN is 1 when HEN_DEV is on and the library is there). It takes its arguments out
// of argv before main, and is asked for the next program at the end of EmulatorMain.
#if HEN_CHAIN
#include <hen_chain.h>
extern "C" void sw_chain_next(void); // switch_platform.c: hen_chain_next(), once
static int ChainPort() {
    return hen_chain_port();
}
#else
[[maybe_unused]] static int ChainPort() {
    return 0;
}
#endif

#if HEN_DEV
extern "C" bool hen_crumbs_enabled; // HEN_CRUMB output (common/hen_crumb.h)
extern "C" int hen_pipe_log;
extern "C" int hen_jit_trace;
extern "C" int hen_exit_counts; // hen/jit/hen_jit.c
extern "C" void hen_jit_exit_stats(unsigned long long* out);
extern "C" void hen_jit_block_stats(unsigned long long* out);
extern "C" int hen_profile;
extern "C" int hen_blackbox;
extern "C" int hen_dump_spirv;
extern "C" void hen_jit_profile_start(void);
extern "C" void hen_jit_profile_stop(void);
extern "C" void hen_jit_profile_report(void);
extern "C" unsigned hen_prof_top_guest[8];
#endif

extern "C" void hen_jit_reach_stats(unsigned long long* out);
extern "C" void hen_jit_translation_stats(unsigned long long* out);
extern "C" void hen_jit_split_stats(unsigned long long* out);
extern "C" void hen_jit_emit_parts_stats(unsigned long long* out);
extern "C" int hen_translation_worker;
extern "C" void hen_jit_stats(unsigned long long* syncs, unsigned long long* ns,
                              unsigned long long* maps, unsigned long long* clears);

// Sound statistics (audio_core/dsp_interface.cpp, switch_audio.c) and the output mode
// (audio_core/switch_sink.cpp).
extern "C" void hen_audio_dsp_stats(unsigned* out);
extern "C" void sw_audio_stats(unsigned* out);
extern "C" void hen_gfx_stats(unsigned long long* out);

extern "C" unsigned long long hen_homebrew_cache_id; // hen/jit/hen_jit.c
extern "C" int hen_link_table;
extern "C" int hen_jit_alias;
extern "C" int hen_worker_depth_limit;
extern "C" int hen_emu_ahead;
extern "C" int hen_tier;
extern "C" int hen_boost_lenient, hen_boost_linger_ms, hen_boost_cancel, hen_handheld_hold;
extern "C" int hen_fs_canon, hen_generic_fs, hen_pointer, hen_generic_seed;
static bool hen_sw_shaders = false;
extern "C" int hen_generic_first, hen_stale_profile;
extern "C" int hen_cache_regen_sync;
extern "C" unsigned long long hen_vblank_count;
extern "C" unsigned long long hen_generic_stats[3], hen_generic_unsupported;
extern "C" unsigned long long hen_part_cache_hits; // (the generic fragment shader's part)
extern "C" unsigned long long hen_fs_configs_raw, hen_fs_configs_built;
extern "C" int hen_bump;
extern "C" int hen_inline_exclusive;
extern "C" int hen_read_table; // hen/jit/hen_jit.c; core/memory.h, PageTable::Pointers
extern "C" int hen_fold_cond_branch; // hen/jit/hen_jit.c; dynarmic's a32_branch.cpp, arm_B
extern "C" int hen_code_align_2m;    // hen/jit/hen_jit.c, hen_jit_create
extern "C" int hen_compact_links;    // hen/jit/hen_jit.c; dynarmic's address_space.cpp, HenLinkStub
extern "C" void hen_file_defer_finish(void); // switch_file_defer.c
extern "C" int hen_cpu_vertex_limit;
extern "C" int hen_cpu_shade_share;
extern "C" int hen_max_lag_ms;
extern "C" int hen_pipe_dedup;
extern "C" int hen_file_defer; // switch_file_defer.c
extern "C" unsigned long long hen_jit_code_ticks(void);
extern "C" unsigned long long hen_fast_lane_skips;
extern "C" unsigned long long hen_standin_stats[7];
extern "C" int hen_substitute;
extern "C" unsigned long long hen_substituted;
extern "C" unsigned long long hen_present_gaps[6];
extern "C" int hen_standin_window;
extern "C" unsigned long long hen_standin_part_ns[6];
extern "C" unsigned long long hen_core_ns[4], hen_core_ticks[4], hen_core_runs[4];
extern "C" int hen_depth_lag;
extern "C" int hen_lazy_resched;
extern "C" int hen_n3ds_clock; // hen/jit/hen_jit.c; core/core_timing.cpp
extern "C" int hen_n3ds_handheld; // hen/jit/hen_jit.c
namespace Core {
bool HenClockStats(double* out); // core/core_timing.cpp
}
extern "C" int hen_gpu_thread;       // hen/jit/hen_jit.c; video_core/hen_gpu_thread.cpp
extern "C" int hen_gpu_defer_writes;
extern "C" int hen_live_fpsr;
extern "C" int hen_direct_links;
extern "C" int hen_load_boost;
extern "C" unsigned long long hen_cpu_shade_ns_per_vertex;
extern "C" void hen_boost_off(void);
extern "C" void hen_boost_stats(unsigned long long* out);
extern "C" void hen_jit_link_stats(unsigned long long* out);
extern "C" void hen_jit_compact_stats(unsigned long long* out);
extern "C" unsigned long long hen_resched_skipped;
extern "C" unsigned long long hen_pipe_parked;
extern "C" long long hen_limiter_given_up_us, hen_limiter_slept_us;
extern "C" int hen_shader_jit;
extern "C" void hen_gfx_part_stats(unsigned long long* out);
extern "C" void hen_gfx_part_repeat_stats(unsigned long long* out);
extern "C" void hen_jit_tier_stats(unsigned long long* out);
extern "C" void hen_jit_early_stats(unsigned long long* out);
extern "C" int hen_publish_early;
extern "C" int hen_interp_chain;
extern "C" unsigned long long hen_jit_chain_stats(void);
extern "C" void hen_jit_lock_stats(unsigned long long* out);
extern "C" void hen_jit_slow_reads(unsigned* megabyte, unsigned* count, int top);
extern "C" unsigned hen_jit_slow_read_last(unsigned megabyte);
extern "C" void hen_jit_guest_stats(unsigned* exclusive, unsigned* svc, unsigned* svc_count, int top);
extern "C" unsigned long long hen_jit_svc_times(unsigned* svc, unsigned long long* ns, unsigned* count, int top);
extern "C" void hen_jit_publish_stats(unsigned long long* out);
extern "C" unsigned hen_cia_chunk_bytes;
extern "C" int hen_pipeline_libraries;
extern "C" int hen_depth_scale_blit;

// Progress counters for the boot-hang report (hle_ipc.cpp, core_timing.cpp, thread.cpp).
extern std::atomic<unsigned> hen_async_queued, hen_async_started, hen_async_finished, hen_async_threw,
    hen_async_negative, hen_ts_pushed, hen_ts_moved, hen_wake_fired, hen_wake_not_waiting;

extern "C" int hen_cond_wake_all; // switch_platform.c
namespace {

// The statistics reports: development builds only.
#if HEN_DEV
void ReportAudio() {
    unsigned d[11]{}, o[4]{};
    hen_audio_dsp_stats(d);
    sw_audio_stats(o);
    if (d[0] == 0) {
        return;
    }
    std::printf("hen: audio: %u fills, %u short (%u frames held); fifo %u..%u (avg %u) frames; "
                "stretched %u (entered %u times), ratio %.3f..%.3f, %u skips; device: %u wakes, %u ran dry, longest gap "
                "%.1f ms, longest fill %.2f ms\n",
                d[0], d[1], d[2], d[4], d[5], d[6], d[3], d[10], d[3] ? d[7] / 10000.0 : 0.0,
                d[8] / 10000.0, d[9], o[0], o[1], o[2] / 1000.0, o[3] / 1000.0);
}

void ReportJitList() {
    const auto s = Core::HenBlockList::GetStats();
    std::printf("hen: jit list: %zu listed, %zu compiled ahead (%.0f ms at start, %.0f ms in idle "
                "time), %zu waiting, %zu new this session, %zu compiled on speculation\n",
                s.listed, s.compiled, s.start_ms, s.idle_ms, s.waiting, s.recorded, s.speculated);
}

// Graphics compile work since the last report (common/hen_stats.h).
void ReportGfx() {
    unsigned long long g[3 * HEN_GFX_KINDS]{};
    hen_gfx_stats(g);
    if (g[3 * HEN_GFX_MEM_READ_CALL] + g[3 * HEN_GFX_MEM_WRITE_CALL] != 0) {
        std::printf("hen: mem: %llu reads and %llu writes left generated code; of memory the "
                    "renderer caches: %llu reads, %llu writes\n",
                    g[3 * HEN_GFX_MEM_READ_CALL], g[3 * HEN_GFX_MEM_WRITE_CALL],
                    g[3 * HEN_GFX_MEM_FLUSH], g[3 * HEN_GFX_MEM_INVALIDATE]);
    }
    // (hen_read_table) Pages the renderer began to cache whose reads stay in generated code,
    // and how often drawing to one took that away and a write-back gave it back.
    if (g[3 * HEN_GFX_READ_TABLE_KEPT] + g[3 * HEN_GFX_READ_TABLE_CLEARED] +
            g[3 * HEN_GFX_READ_TABLE_RESTORED] != 0) {
        std::printf("hen: mem: read table: %llu cached pages kept readable, %llu entries cleared "
                    "(drawn to), %llu restored (written back)\n",
                    g[3 * HEN_GFX_READ_TABLE_KEPT], g[3 * HEN_GFX_READ_TABLE_CLEARED],
                    g[3 * HEN_GFX_READ_TABLE_RESTORED]);
    }
    if (g[3 * HEN_GFX_DOWNLOAD_FINISH] != 0) {
        const auto avg = [&](int kind) {
            return g[3 * kind] ? g[3 * kind + 1] / 1e6 / g[3 * kind] : 0.0;
        };
        std::printf("hen: download: %llu surface downloads: submit+wait %.2f ms each (longest "
                    "%.1f), convert %.2f ms each; %llu driver waits, %.2f ms each\n",
                    g[3 * HEN_GFX_DOWNLOAD_FINISH], avg(HEN_GFX_DOWNLOAD_FINISH),
                    g[3 * HEN_GFX_DOWNLOAD_FINISH + 2] / 1e6, avg(HEN_GFX_DOWNLOAD_ENCODE),
                    g[3 * HEN_GFX_GPU_WAIT], avg(HEN_GFX_GPU_WAIT));
        if (g[3 * HEN_GFX_DOWNLOAD_DEPTH] != 0) {
            std::printf("hen: download: of those, %llu of depth: submit+wait %.2f ms each (longest %.1f); %llu given the previous read's data\n",
                        g[3 * HEN_GFX_DOWNLOAD_DEPTH],
                        g[3 * HEN_GFX_DOWNLOAD_DEPTH + 1] / 1e6 / g[3 * HEN_GFX_DOWNLOAD_DEPTH],
                        g[3 * HEN_GFX_DOWNLOAD_DEPTH + 2] / 1e6, g[3 * HEN_GFX_DOWNLOAD_LAGGED]);
        }
    }
    if (g[12] != 0) {
        std::printf("hen: gfx: vertex shaders on the CPU: %llu draws, %llu vertices, %.0f ms "
                    "(longest %.1f); %llu of the draws because their pipeline wasn't ready\n",
                    g[12], g[16], g[13] / 1e6, g[14] / 1e6, g[3 * HEN_GFX_CPU_INSTEAD]);
        std::printf("hen: gfx: a vertex shaded on the CPU costs %llu ns (running mean)\n",
                    hen_cpu_shade_ns_per_vertex);
    }
    if (hen_pipe_parked != 0) {
        std::printf("hen: gfx: %llu pipelines waited for a vertex shader another was compiling\n",
                    hen_pipe_parked);
        hen_pipe_parked = 0;
    }
    {
        static unsigned long long told_raw, told_built;
        if (hen_fs_configs_raw != told_raw || hen_fs_configs_built != told_built) {
            told_raw = hen_fs_configs_raw;
            told_built = hen_fs_configs_built;
            std::printf("hen: gfx: fragment shaders: %llu configurations seen, %llu built (in all)\n",
                        told_raw, told_built);
        }
        if (hen_generic_stats[0] != 0 || hen_generic_unsupported != 0) {
            std::printf("hen: gfx: generic fragment shader: %llu draws; %llu pipelines linked on the "
                        "renderer's thread (%.1f ms); %llu draws it does not support\n",
                        hen_generic_stats[0], hen_generic_stats[1], hen_generic_stats[2] / 1e6,
                        hen_generic_unsupported);
            hen_generic_stats[0] = hen_generic_stats[1] = hen_generic_stats[2] = 0;
            hen_generic_unsupported = 0;
        }
        static unsigned long long told_hits;
        if (hen_part_cache_hits != told_hits) {
            std::printf("hen: gfx: fragment shader parts reused: %llu (in all)\n", hen_part_cache_hits);
            told_hits = hen_part_cache_hits;
        }
    }
    {
        unsigned long long boost[9];
        hen_boost_stats(boost);
        if (boost[0] != 0 || boost[1] != 0) {
            std::printf("hen: boost: on %llu ms, %llu switches; asked for by: loading %llu, new code %llu, "
                        "pipelines %llu, busy %llu, held (New 3DS mode or handheld) %llu (vertical blanks); %s, longest switch %llu us\n",
                        boost[0], boost[1], boost[2], boost[3], boost[4], boost[7], boost[8],
                        boost[6] ? "by the CPU clock" : "by the system's boost mode", boost[5]);
        }
    }
    // --gpu-thread (video_core/hen_gpu_thread.cpp): what went to the GPU thread, how long it
    // was busy, and what the emulation thread paid in waiting for it, by reason.
    if (VideoCore::HenGpu::Active()) {
        namespace G = VideoCore::HenGpu;
        unsigned long long t[G::StatCount];
        G::Stats(t);
        const auto wait = [&](int kind) { return t[11 + 2 * kind] / 1e6; };
        const auto count = [&](int kind) { return t[10 + 2 * kind]; };
        double waited = 0;
        for (int kind = 0; kind < G::WaitKinds; kind++) {
            waited += wait(kind);
        }
        std::printf("hen: gputhread: %llu commands queued, %llu run, busy %.0f ms, longest queue "
                    "%llu; %llu interrupts delivered; emulation thread waited %.0f ms (longest "
                    "%.1f): vblank %.0f (%llu), idle %.0f (%llu), read %.0f (%llu), write %.0f "
                    "(%llu), full %.0f (%llu), map %.0f (%llu), regs %.0f (%llu), other %.0f "
                    "(%llu)\n",
                    t[0], t[1], t[3] / 1e6, t[4], t[7], waited, t[8] / 1e6, wait(G::WaitVblank),
                    count(G::WaitVblank), wait(G::WaitIdle), count(G::WaitIdle),
                    wait(G::WaitRead), count(G::WaitRead), wait(G::WaitWrite),
                    count(G::WaitWrite), wait(G::WaitFull), count(G::WaitFull),
                    wait(G::WaitMap), count(G::WaitMap), wait(G::WaitRegs), count(G::WaitRegs),
                    wait(G::WaitOther), count(G::WaitOther));
        std::printf("hen: gputhread: writes to cached pages: %llu deferred (%llu ranges queued, "
                    "%llu applied by the GPU thread), %llu done at once (GPU thread idle)\n",
                    t[5], t[2], t[9], t[6]);
    }
    std::printf("hen: limiter: slept %lld ms, gave up %lld ms of lag\n",
                hen_limiter_slept_us / 1000, hen_limiter_given_up_us / 1000);
    hen_limiter_slept_us = hen_limiter_given_up_us = 0;
    if (g[3 * HEN_GFX_DRAW_SKIPPED] != 0) {
        std::printf("hen: gfx: %llu draws (%llu vertices) left out because their pipeline wasn't ready; %llu of them waiting for a stand-in pipeline\n",
                    g[3 * HEN_GFX_DRAW_SKIPPED], g[3 * HEN_GFX_DRAW_SKIPPED + 1], hen_fast_lane_skips);
        hen_fast_lane_skips = 0;
    }
    if (hen_substituted != 0) {
        std::printf("hen: gfx: %llu draws made with a substitute pipeline\n", hen_substituted);
        hen_substituted = 0;
    }
    if (hen_present_gaps[0] != 0) {
        std::printf("hen: shown: %llu frames; gaps over 40 ms: %llu, over 70 ms: %llu, over 120 ms: %llu; "
                    "longest %.0f ms; %.0f ms in gaps over 40 ms\n",
                    hen_present_gaps[0], hen_present_gaps[1], hen_present_gaps[2], hen_present_gaps[3],
                    hen_present_gaps[4] / 1e3, hen_present_gaps[5] / 1e3);
        for (auto& value : hen_present_gaps) {
            value = 0;
        }
    }
    if (hen_standin_stats[0] + hen_standin_stats[3] != 0) {
        std::printf("hen: gfx: stand-in pipelines: %llu built on the emulation thread (%.1f ms, longest "
                    "%.1f), %llu by the workers (%.1f ms, longest %.1f)\n",
                    hen_standin_stats[0], hen_standin_stats[1] / 1e6, hen_standin_stats[2] / 1e6,
                    hen_standin_stats[3], hen_standin_stats[4] / 1e6, hen_standin_stats[5] / 1e6);
        std::printf("hen: gfx: stand-in parts: input interface %.1f ms, vertex side %.1f, fragment shader "
                    "%.1f, output interface %.1f, link %.1f; %llu parts new to the driver\n",
                    hen_standin_part_ns[0] / 1e6, hen_standin_part_ns[1] / 1e6, hen_standin_part_ns[2] / 1e6,
                    hen_standin_part_ns[3] / 1e6, hen_standin_part_ns[4] / 1e6, hen_standin_part_ns[5]);
        for (auto& value : hen_standin_part_ns) {
            value = 0;
        }
        for (auto& value : hen_standin_stats) {
            value = 0;
        }
    }
    if (g[0] + g[3] + g[6] + g[9] == 0) {
        return;
    }
    const auto ms = [&](int kind, int field) { return g[3 * kind + field] / 1e6; };
    if (g[3 * HEN_GFX_FILE_SYNC] + g[3 * HEN_GFX_FILE_ASYNC] != 0) {
        std::printf("hen: files: %llu reads on the emulation thread (%.0f ms, longest %.1f), %llu on "
                    "another thread (%.0f ms, longest %.1f); %.1f MiB, largest %llu KiB\n",
                    g[3 * HEN_GFX_FILE_SYNC], ms(HEN_GFX_FILE_SYNC, 1), ms(HEN_GFX_FILE_SYNC, 2),
                    g[3 * HEN_GFX_FILE_ASYNC], ms(HEN_GFX_FILE_ASYNC, 1), ms(HEN_GFX_FILE_ASYNC, 2),
                    g[3 * HEN_GFX_FILE_BYTES + 1] / 1048576.0, g[3 * HEN_GFX_FILE_BYTES + 2] / 1024);
    }
    {
        unsigned long long rep[6];
        hen_gfx_part_repeat_stats(rep);
        if (rep[0] + rep[3] != 0) {
            std::printf("hen: gfx: part requests: vertex side %llu, %llu of them repeats (%.1f ms); "
                        "fragment %llu, %llu repeats (%.1f ms)\n",
                        rep[0], rep[1], rep[2] / 1e6, rep[3], rep[4], rep[5] / 1e6);
        }
    }
    {
        unsigned long long part[8];
        hen_gfx_part_stats(part);
        if (part[2] + part[4] != 0) {
            std::printf("hen: gfx: pipeline parts compiled: vertex side %llu (%.0f ms), fragment shader %llu (%.0f ms), "
                        "interfaces %llu (%.0f ms)\n",
                        part[2], part[3] / 1e6, part[4], part[5] / 1e6, part[0] + part[6],
                        (part[1] + part[7]) / 1e6);
        }
    }
    std::printf("hen: gfx: pipelines: %llu compiled (%.0f ms, longest %.0f), %llu from the "
                "driver's cache (%.0f ms); render thread waited %llu times (%.0f ms, longest "
                "%.0f); shaders: %llu compiled (%.0f ms, longest %.0f)\n",
                g[0], ms(0, 1), ms(0, 2), g[3], ms(1, 1), g[6], ms(2, 1), ms(2, 2), g[9],
                ms(3, 1), ms(3, 2));
}
#endif // HEN_DEV

const char* StatusName(Core::System::ResultStatus status) {
    using Status = Core::System::ResultStatus;
    switch (status) {
    case Status::Success:
        return "success";
    case Status::ErrorNotInitialized:
        return "not initialized";
    case Status::ErrorGetLoader:
        return "no loader for this file (missing, or not a 3DS program)";
    case Status::ErrorSystemMode:
        return "couldn't determine the system mode";
    case Status::ErrorLoader:
        return "loader error";
    case Status::ErrorLoader_ErrorEncrypted:
        return "the program is encrypted";
    case Status::ErrorLoader_ErrorInvalidFormat:
        return "invalid format";
    case Status::ErrorSystemFiles:
        return "missing system files";
    case Status::ShutdownRequested:
        return "shutdown requested";
    default:
        return "other";
    }
}

extern "C" void hen_jit_compile_stats(unsigned long long* out);
extern "C" void hen_jit_miss_stats(unsigned long long* out);
extern "C" unsigned hen_jit_thread_times(double* wall_ms, double* cpu_ms);
extern "C" void hen_jit_run_stats(unsigned long long* out);
extern "C" int hen_fast_dispatch;

#if HEN_DEV
void ReportJit(const char* when) {
    unsigned long long r[3]{};
    hen_jit_run_stats(r);
    if (r[2] != 0) {
        {
            char line[200];
            int at = std::snprintf(line, sizeof(line), "hen: cores:");
            for (int c = 0; c < 4; c++) {
                if (hen_core_runs[c] != 0) {
                    at += std::snprintf(line + at, sizeof(line) - at,
                                        " %d: %llu ms, %.0f M ticks, %llu runs;", c,
                                        hen_core_ns[c] / 1000000, hen_core_ticks[c] / 1e6,
                                        hen_core_runs[c]);
                }
                hen_core_ns[c] = hen_core_ticks[c] = hen_core_runs[c] = 0;
            }
            std::puts(line);
        }
        std::printf("hen: jit run: %.0f ms in generated code (%llu slices), %llu dispatcher "
                    "lookups\n",
                    r[1] / 1e6, r[2], r[0]);
        {
            // (generated-code time includes the calls it makes: memory, system calls, services)
            const unsigned long long ticks = hen_jit_code_ticks();
            // (hen_fold_cond_branch: the blocks that end in a conditional branch which was a
            // block of its own before; all three figures are of every block compiled so far)
            unsigned long long b[3]{};
            hen_jit_block_stats(b);
            std::printf("hen: code: %.0f M ticks of program code executed, %.0f M a second of time in generated code; "
                        "%llu blocks compiled so far, %llu of them with a folded conditional branch, %.1f ticks a block\n",
                        ticks / 1e6, r[1] != 0 ? ticks / 1e6 / (r[1] / 1e9) : 0.0, b[1], b[2],
                        b[1] != 0 ? static_cast<double>(b[0]) / b[1] : 0.0);
            // (hen_exit_counts, hen/jit/hen_jit.c) How generated code left its blocks. Table
            // hits: what entered the dispatcher less what it did not find or run. Linked
            // direct branches are not counted: estimated as the blocks these ticks make at
            // the mean length of all blocks compiled, less the exits counted.
            if (hen_exit_counts) {
                unsigned long long e[10]{};
                hen_jit_exit_stats(e);
                const unsigned long long entered = e[1] + e[2] + e[3] + e[4];
                const unsigned long long hits = entered - std::min(entered, e[5] + e[6] + e[7]);
                const double block_ticks = e[9] != 0 ? static_cast<double>(e[8]) / e[9] : 0.0;
                const double blocks = block_ticks > 0.0 ? ticks / block_ticks : 0.0;
                std::printf("hen: exits: returns %llu predicted, %llu to the dispatcher; indirect "
                            "%llu; other %llu; direct unlinked %llu; dispatcher: about %llu table "
                            "hits, %llu lookups in C++, %llu halts, %llu out of ticks; direct "
                            "linked about %.1f M (estimate: %.1f ticks a compiled block)\n",
                            e[0], e[1], e[2], e[3], e[4], hits, e[7], e[5], e[6],
                            std::max(0.0, blocks - static_cast<double>(e[0] + entered)) / 1e6,
                            block_ticks);
            }
        }
    }
    unsigned long long c[4]{};
    hen_jit_compile_stats(c);
    if (c[2] != 0) {
        std::printf("hen: jit compile: %llu blocks in %llu calls: IR %.0f ms, emit %.0f ms "
                    "(%.3f ms a block)\n",
                    c[2], c[3], c[0] / 1e6, c[1] / 1e6, (c[0] + c[1]) / 1e6 / c[2]);
    }
    unsigned long long tier[5]{};
    unsigned long long early[2];
    hen_jit_early_stats(early);
    if (early[0] || early[1]) {
        std::printf("hen: jit worker: %llu batches published early, %llu wake-ups skipped\n", early[0], early[1]);
    }
    hen_jit_tier_stats(tier);
    if (tier[0] + tier[1] + tier[2] != 0) {
        std::printf("hen: jit tier: %llu blocks interpreted (%llu instructions, %.0f ms, %.2f us a block); "
                    "compiled here instead: %llu after %d visits, %llu the interpreter refused\n",
                    tier[0], tier[3], tier[4] / 1e6, tier[0] ? tier[4] / 1e3 / tier[0] : 0.0, tier[1],
                    hen_tier, tier[2]);
        // (of the blocks interpreted: those that followed another without leaving the interpreter)
        std::printf("hen: jit tier: %llu blocks interpreted in a chain%s\n", hen_jit_chain_stats(),
                    hen_interp_chain ? "" : " (--no-interp-chain)");
    }
    {
        unsigned megabyte[3], count[3];
        hen_jit_slow_reads(megabyte, count, 3);
        if (count[0] >= 20000) {
            std::printf("hen: mem: reads that left generated code, by address: %03x00000 %u, %03x00000 %u, "
                        "%03x00000 %u\n",
                        megabyte[0], count[0], megabyte[1], count[1], megabyte[2], count[2]);
        }
    }
    {
        // (before hen_jit_guest_stats, which clears the counts)
        unsigned svc[4], count[4];
        unsigned long long ns[4];
        const unsigned long long total = hen_jit_svc_times(svc, ns, count, 4);
        if (total >= 200'000'000ull) {
            std::printf("hen: svc: %.0f ms in system calls; most: 0x%02x %.0f ms (%u), 0x%02x %.0f ms (%u), "
                        "0x%02x %.0f ms (%u), 0x%02x %.0f ms (%u)\n",
                        total / 1e6, svc[0], ns[0] / 1e6, count[0], svc[1], ns[1] / 1e6, count[1],
                        svc[2], ns[2] / 1e6, count[2], svc[3], ns[3] / 1e6, count[3]);
        }
    }
    if (hen_resched_skipped >= 1000) {
        std::printf("hen: resched: %llu reschedule requests from system calls dropped (nothing "
                    "to switch to)\n",
                    hen_resched_skipped);
    }
    hen_resched_skipped = 0;
    if (double c[7]; Core::HenClockStats(c)) {
        // A New 3DS title in its 804 MHz mode: what CPU work (instructions and system call
        // charges, in 268 MHz ticks) was divided by, between 1 and 3 (core_timing.cpp: the
        // automatic clock; --n3ds-clock holds 3, --n3ds-clock-off 1), and what that came to.
        std::printf("hen: clock: 804 MHz mode: factor %.2f to %.2f, mean %.2f, %.0f steps down, "
                    "%.0f up; %.0f M ticks of CPU work charged as %.0f M\n",
                    c[0], c[2], c[1], c[3], c[4], c[5] / 1e6, c[6] / 1e6);
    }
    {
        unsigned exclusive[2], svc[4], svc_count[4];
        hen_jit_guest_stats(exclusive, svc, svc_count, 4);
        if (exclusive[0] + exclusive[1] >= 100000 || svc_count[0] >= 50000) {
            std::printf("hen: guest: exclusive stores %u done, %u failed; system calls: 0x%02x %u, 0x%02x %u, "
                        "0x%02x %u, 0x%02x %u\n",
                        exclusive[0], exclusive[1], svc[0], svc_count[0], svc[1], svc_count[1], svc[2],
                        svc_count[2], svc[3], svc_count[3]);
        }
    }
    unsigned long long locks[3]{};
    hen_jit_lock_stats(locks);
    if (locks[0] != 0) {
        std::printf("hen: jit lock: taken %llu times outside the worker, %.0f ms waiting (longest %.1f)\n",
                    locks[0], locks[1] / 1e6, locks[2] / 1e6);
    }
    unsigned long long miss[7]{};
    hen_jit_miss_stats(miss);
    if (miss[0] + miss[1] + miss[2] + miss[3] != 0) {
        std::printf("hen: jit misses: %llu found after the lock, %llu queued for the worker, %llu known "
                    "targets not queued, %llu never named; waiting for the lock %.0f ms (%llu waits "
                    "over 1 ms), in misses after it %.0f ms\n",
                    miss[0], miss[1], miss[2], miss[3], miss[4] / 1e6, miss[5], miss[6] / 1e6);
    }
    unsigned long long parts[7]{};
    hen_jit_emit_parts_stats(parts);
    if (parts[6] != 0) {
        std::printf("hen: jit emit: %llu blocks: code %.0f ms, tables %.0f, links %.0f, frontier %.0f, "
                    "executable %.0f, ranges %.0f\n",
                    parts[6], parts[0] / 1e6, parts[1] / 1e6, parts[2] / 1e6, parts[3] / 1e6,
                    parts[4] / 1e6, parts[5] / 1e6);
    }
    unsigned long long split[3]{};
    hen_jit_split_stats(split);
    if (split[2] != 0) {
        std::printf("hen: jit split: %llu blocks compiled ahead here: translating %.0f ms, emitting %.0f ms\n",
                    split[2], split[0] / 1e6, split[1] / 1e6);
    }
    unsigned long long tr[3]{};
    hen_jit_translation_stats(tr);
    if (tr[0] + tr[1] + tr[2] > 0) {
        std::printf("hen: jit worker: %llu blocks translated ahead, %llu used, %llu out of date\n",
                    tr[0], tr[1], tr[2]);
    }
    unsigned long long reach[5]{};
    hen_jit_reach_stats(reach);
    if (reach[0] + reach[1] + reach[2] + reach[3] + reach[4] > 0) {
        std::printf("hen: jit reach: updates reaching back from the newest code <=64K %llu, "
                    "<=256K %llu, <=1M %llu, <=4M %llu, further %llu\n",
                    reach[0], reach[1], reach[2], reach[3], reach[4]);
    }
    unsigned long long syncs = 0, ns = 0, maps = 0, clears = 0;
    {
        unsigned long long pub[7];
        hen_jit_publish_stats(pub);
        const unsigned long long count = pub[3] + pub[4] + pub[5] + pub[6];
        if (count != 0) {
            std::printf("hen: jit publishes: %llu, %.1f KiB and %.1f ranges each, %.1f us in the request each; "
                        "%llu under 40 us, %llu under 100 us, %llu under 1 ms, %llu longer\n",
                        count, pub[0] / 1024.0 / count, static_cast<double>(pub[1]) / count,
                        pub[2] / 1000.0 / count, pub[3], pub[4], pub[5], pub[6]);
        }
    }
    hen_jit_stats(&syncs, &ns, &maps, &clears);
    std::printf("hen: jit %s: %llu code updates (%llu chunk maps), %.1f ms in total (%.3f ms each); "
                "%llu cache clears\n",
                when, syncs, maps, ns / 1e6, syncs ? ns / 1e6 / syncs : 0.0, clears);
    unsigned long long links[3];
    hen_jit_link_stats(links);
    // (hen_compact_links: the one-instruction sites among those, the stubs their unlinked
    // form goes through, and what a block takes without them)
    unsigned long long compact[6]{}, blocks[3]{};
    hen_jit_compact_stats(compact);
    hen_jit_block_stats(blocks);
    std::printf("hen: jit links: %llu sites written as direct branches, %llu through link slots, "
                "%llu of those made direct since; compact sites: %llu direct, %llu through a stub, "
                "%llu of those made direct since (%llu left: their block was invalidated), %llu stubs; "
                "%.0f bytes a block, %.0f with the stubs\n",
                links[0], links[1], links[2], compact[0], compact[1], compact[2], compact[5], compact[3],
                blocks[1] != 0 ? static_cast<double>(compact[4]) / blocks[1] : 0.0,
                blocks[1] != 0 ? static_cast<double>(compact[4] + 40 * compact[3]) / blocks[1] : 0.0);
}
#endif // HEN_DEV

/// Every emulated thread: state, where it is, and the objects it waits for.
static void DumpEmulatedThreads(Core::System& system) {
    static const char* const status_names[] = {"running", "ready", "wait-arbiter", "wait-sleep",
                                               "wait-ipc", "wait-any", "wait-all", "wait-hle",
                                               "dormant", "dead"};
    std::printf("hen: hang: service calls on host threads: %u queued, %u started, %u finished, %u threw, "
                "%u asked for no wake-up; wake-ups from other threads: %u queued, %u taken over; "
                "thread wake-ups fired %u (%u for a thread not waiting)\n",
                hen_async_queued.load(), hen_async_started.load(), hen_async_finished.load(),
                hen_async_threw.load(), hen_async_negative.load(), hen_ts_pushed.load(),
                hen_ts_moved.load(), hen_wake_fired.load(), hen_wake_not_waiting.load());
    std::printf("hen: hang: no frames and an idle emulated CPU for 10 s; emulated threads:\n");
    for (u32 core = 0; core < system.GetNumCores(); core++) {
        const auto& manager = system.Kernel().GetThreadManager(core);
        for (const auto& thread : manager.GetThreadList()) {
            const unsigned status = static_cast<unsigned>(thread->status);
            std::printf("hen: hang: core %u thread %u (object %u) %s, priority %u, pc %08x lr %08x",
                        core, thread->thread_id, thread->GetObjectId(),
                        status < 10 ? status_names[status] : "?", thread->current_priority,
                        thread->context.cpu_registers[15], thread->context.cpu_registers[14]);
            if (thread->status == Kernel::ThreadStatus::WaitArb) {
                std::printf(", address %08x", thread->wait_address);
            }
            for (const auto& object : thread->wait_objects) {
                std::printf("; waits for %s \"%s\" (object %u)", object->GetTypeName().c_str(),
                            object->GetName().c_str(), object->GetObjectId());
            }
            std::printf("\n");
        }
    }
    std::fflush(stdout);
}

// Crashes leave no report on this runtime, and an uncaught C++ exception (Vulkan-Hpp throws on
// errors) would end the same way: say what it was first.
[[noreturn]] void OnTerminate() {
    if (const std::exception_ptr ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "\nhen: terminate: uncaught exception: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "\nhen: terminate: uncaught non-std exception\n");
        }
    } else {
        std::fprintf(stderr, "\nhen: terminate: no active exception\n");
    }
    std::fflush(stderr);
    std::abort();
}

/// The 3DS keyboard applet, answered with the console's own keyboard. If that can't be shown,
/// the answer is the 3DS user name, as the default applet gives.
class SwitchKeyboard final : public Frontend::SoftwareKeyboard {
public:
    void Execute(const Frontend::KeyboardConfig& config_) override {
        SoftwareKeyboard::Execute(config_);
        const u8 ok = config.button_config == Frontend::ButtonConfig::Triple   ? 2
                      : config.button_config == Frontend::ButtonConfig::Dual ? 1
                                                                             : 0;
        const unsigned max_length = config.max_text_length ? config.max_text_length : 64;
        std::string text;
        sw_watch_enable(false);
        for (int attempt = 0; attempt < 3; attempt++) {
            std::vector<char> buffer(max_length * 4 + 4);
            const int result = sw_keyboard(config.hint_text.c_str(), text.c_str(), max_length,
                                           buffer.data(), buffer.size());
            if (result <= 0) {
                text.clear();
                if (result == 0 && ok > 0) {
                    sw_watch_enable(true);
                    Finalize("", 0); // Cancel
                    return;
                }
                break;
            }
            text = buffer.data();
            if (ValidateFilters(text) == Frontend::ValidationError::None &&
                ValidateInput(text) == Frontend::ValidationError::None) {
                sw_watch_enable(true);
                Finalize(text, ok);
                return;
            }
            std::printf("hen: keyboard: the title doesn't accept that text; asking again\n");
        }
        sw_watch_enable(true);
        Finalize("Azahar", ok);
    }
    void ShowError(const std::string& error) override {
        std::printf("hen: keyboard: %s\n", error.c_str());
    }
};

} // namespace

int EmulatorMain(int argc, char** argv) {
    std::string program;
#if HEN_DEV
    int exit_after_s = 0;
    // --movie-record NAME / --movie-play NAME: the emulator's input movies (core/movie.cpp), kept
    // in /switch/Kaizhar/movies/NAME.ctm. Inputs are stored per input poll of the emulated
    // console, so a replay follows the same route whatever the host's speed; the clock starts
    // from the movie's time. A replay needs the save data the recording started from.
    std::string movie_record, movie_play;
    bool clock_info = false;   // --clock-info: CPU/GPU clock rates and temperature in the log
    int mic = 1;             // --mic N: 0 none, 1 the system's microphone, 2 the emulator's static noise
    bool mic_probe = false;  // --mic-probe: what the audio input service answers and records
    int repeat_left = 0;     // --repeat N
    bool shot_by_frame = false;
    std::vector<int> shot_times; // --screenshot-at S[,S...]: pictures sent over the log
#endif
    bool keep_awake = false; // --keep-awake
    int layout_override = -1;  // --layout N (0..3: title_menu.cpp's list)
    bool software_renderer = false;
    bool debug_log = false;
    bool interpreter = false;
    u64 title_id_arg = 0; // --title <id>
    const char* log_filter = nullptr; // --log "<filter>", e.g. "*:Info Service:Debug"
    bool no_audio = false;
    bool jit_list = true; // compile ahead from the list of blocks earlier sessions used
    int resolution = 0; // internal resolution: 0 = what the layout shows (the smallest whole
                        // multiple of 400x240 that covers the top screen as drawn), or 1..10
    bool fresh_jit_list = false; // start from an empty list (a first session, for measuring)
    int jit_start_ms = 4000; // at most; the whole list usually takes less
    bool cold_shaders = false; // an empty shader cache, in its own directory
    bool async_shaders = true; // skip draws whose pipeline is still compiling
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) {
            software_renderer = std::strcmp(argv[++i], "sw") == 0;
        } else if (std::strcmp(argv[i], "--debug-log") == 0) {
            debug_log = true;
        } else if (std::strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            // An installed title by its ID (its path has spaces, which the netloader's
            // argument passing doesn't keep).
            // (looked up below, once the user folder is known)
            title_id_arg = std::strtoull(argv[++i], nullptr, 16);
        } else if (std::strcmp(argv[i], "--no-depth-scale-blit") == 0) {
            hen_depth_scale_blit = 0;
        } else if (std::strcmp(argv[i], "--whole-pipelines") == 0) {
            hen_pipeline_libraries = 0; // for comparing
        } else if (std::strcmp(argv[i], "--cia-chunk-kib") == 0 && i + 1 < argc) {
            hen_cia_chunk_bytes = static_cast<unsigned>(std::atoi(argv[++i])) * 1024; // for comparing
        } else if (std::strcmp(argv[i], "--translation-worker") == 0 && i + 1 < argc) {
            // (hen/jit/hen_jit.c) 0: no second thread compiles ahead. Anything else: the default.
            if (std::atoi(argv[++i]) == 0) {
                hen_translation_worker = 0;
            }
        } else if (std::strcmp(argv[i], "--jit-worker-depth") == 0 && i + 1 < argc) {
            hen_worker_depth_limit = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--jit-ahead") == 0 && i + 1 < argc) {
            hen_emu_ahead = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--cpu-vertex-limit") == 0 && i + 1 < argc) {
            hen_cpu_vertex_limit = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--max-lag-ms") == 0 && i + 1 < argc) {
            hen_max_lag_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--cpu-shade-share") == 0 && i + 1 < argc) {
            hen_cpu_shade_share = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--shader-jit") == 0) {
            hen_shader_jit = 1;
        } else if (std::strcmp(argv[i], "--no-shader-jit") == 0) {
            hen_shader_jit = 0;
        } else if (std::strcmp(argv[i], "--no-inline-exclusive") == 0) {
            hen_inline_exclusive = 0;
        } else if (std::strcmp(argv[i], "--jit-tier") == 0 && i + 1 < argc) {
            hen_tier = std::min(std::atoi(argv[++i]), 60000);
        } else if (std::strcmp(argv[i], "--no-jit-alias") == 0) {
            hen_jit_alias = 0; // for comparing
        } else if (std::strcmp(argv[i], "--no-link-table") == 0) {
            hen_link_table = 0; // for comparing
        } else if (std::strcmp(argv[i], "--load-boost") == 0) {
            hen_load_boost = 1;
        } else if (std::strcmp(argv[i], "--no-load-boost") == 0) {
            hen_load_boost = 0;
        } else if (std::strcmp(argv[i], "--load-boost-system") == 0) {
            hen_load_boost = 3; // (the system's boost mode docked too, as before 2026-11-12)
        } else if (std::strcmp(argv[i], "--no-fs-canon") == 0) {
            hen_fs_canon = 0;
        } else if (std::strcmp(argv[i], "--no-generic-seed") == 0) {
            hen_generic_seed = 0;
        } else if (std::strcmp(argv[i], "--no-jit-publish-early") == 0) {
            hen_publish_early = 0;
        } else if (std::strcmp(argv[i], "--interp-chain") == 0) {
            hen_interp_chain = 1;
        } else if (std::strcmp(argv[i], "--no-pointer") == 0) {
            hen_pointer = 0;
        } else if (std::strcmp(argv[i], "--no-generic-fs") == 0) {
            hen_generic_fs = 0;
        } else if (std::strcmp(argv[i], "--stale-profile") == 0) {
            hen_stale_profile = 1;
        } else if (std::strcmp(argv[i], "--cache-regen-sync") == 0) {
            hen_cache_regen_sync = 1; // (stale shader caches regenerated before the start)
        } else if (std::strcmp(argv[i], "--no-generic-first") == 0) {
            hen_generic_first = 0;
        } else if (std::strcmp(argv[i], "--generic-first") == 0) {
            hen_generic_first = 1;
        } else if (std::strcmp(argv[i], "--generic-all") == 0) {
            hen_generic_fs = 2;
        } else if (std::strcmp(argv[i], "--sw-shaders") == 0) {
            // (every draw's vertices shaded on the CPU: slow; with --generic-all, every draw
            // the generic fragment shader supports goes through it)
            hen_sw_shaders = true;
        } else if (std::strcmp(argv[i], "--boost-strict") == 0) {
            hen_boost_lenient = 0;
        } else if (std::strcmp(argv[i], "--no-handheld-hold") == 0) {
            hen_handheld_hold = 0; // (handheld: the system's boost mode only while loading)
        } else if (std::strcmp(argv[i], "--no-boost-cancel") == 0) {
            hen_boost_cancel = 0; // (end the system's boost with appletSetCpuBoostMode(Normal))
        } else if (std::strcmp(argv[i], "--boost-linger") == 0 && i + 1 < argc) {
            hen_boost_linger_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--cpu-boost") == 0 && i + 1 < argc) {
            hen_load_boost = 0; // (the manual experiment: no automatic switching on top of it)
            sw_cpu_boost(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-direct-links") == 0) {
            hen_direct_links = 0;
        } else if (std::strcmp(argv[i], "--no-live-fpsr") == 0) {
            hen_live_fpsr = 0;
        } else if (std::strcmp(argv[i], "--no-lazy-resched") == 0) {
            hen_lazy_resched = 0;
        } else if (std::strcmp(argv[i], "--no-read-table") == 0) {
            hen_read_table = 0;
        } else if (std::strcmp(argv[i], "--no-fold-cond-branch") == 0) {
            hen_fold_cond_branch = 0;
        } else if (std::strcmp(argv[i], "--no-code-align-2m") == 0) {
            hen_code_align_2m = 0;
        } else if (std::strcmp(argv[i], "--no-compact-links") == 0) {
            hen_compact_links = 0;
        } else if (std::strcmp(argv[i], "--n3ds-clock") == 0) {
            hen_n3ds_clock = 1; // (for comparing: 804 MHz whatever the host has to spare)
        } else if (std::strcmp(argv[i], "--n3ds-handheld") == 0 && i + 1 < argc) {
            // (a New 3DS title in handheld: 1 the CPU clock held as docked, 2 the system's boost
            // mode held, 0 nothing held)
            hen_n3ds_handheld = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--n3ds-kernel-auto") == 0) {
            hen_n3ds_clock = 3; // (for comparing: system calls charged at the factor code gets)
        } else if (std::strcmp(argv[i], "--n3ds-clock-off") == 0) {
            hen_n3ds_clock = 0; // (for comparing: charged as at 268 MHz, upstream's)
        } else if (std::strcmp(argv[i], "--gpu-thread") == 0) {
            hen_gpu_thread = 1;
        } else if (std::strcmp(argv[i], "--gpu-thread-sync-writes") == 0) {
            hen_gpu_thread = 1;
            hen_gpu_defer_writes = 0;
        } else if (std::strcmp(argv[i], "--sync-depth-reads") == 0) {
            hen_depth_lag = 0;
        } else if (std::strcmp(argv[i], "--no-pipe-dedup") == 0) {
            hen_pipe_dedup = 0;
        } else if (std::strcmp(argv[i], "--pipe-dedup") == 0) {
            hen_pipe_dedup = 1;
        } else if (std::strcmp(argv[i], "--no-gpu-thread") == 0) {
            hen_gpu_thread = 0;
        } else if (std::strcmp(argv[i], "--standin-rule") == 0 && i + 1 < argc) {
            hen_standin_window = std::atoi(argv[++i]) == 0 ? 0 : 2; // (see hen/jit/hen_jit.c)
        } else if (std::strcmp(argv[i], "--jit-bump") == 0 && i + 1 < argc) {
            hen_bump = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--substitute") == 0) {
            hen_substitute = 1;
        } else if (std::strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            layout_override = std::atoi(argv[++i]); // (test runs: not the saved layout)
        } else if (std::strcmp(argv[i], "--keep-awake") == 0) {
            keep_awake = true;
        } else if (std::strcmp(argv[i], "--no-wake-all") == 0) {
            hen_cond_wake_all = 0; // (switch_platform.c: A/B for the boot hang)
        } else if (std::strcmp(argv[i], "--no-file-defer") == 0) {
            hen_file_defer = 0;
        } else if (std::strcmp(argv[i], "--interpreter") == 0) {
            interpreter = true; // for telling a JIT bug from anything else
        } else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_filter = argv[++i];
        } else if (std::strcmp(argv[i], "--no-audio") == 0) {
            no_audio = true;
        } else if (std::strcmp(argv[i], "--no-jit-list") == 0) {
            jit_list = false;
        } else if (std::strcmp(argv[i], "--no-fast-dispatch") == 0) {
            hen_fast_dispatch = 0;
        } else if (std::strcmp(argv[i], "--res") == 0 && i + 1 < argc) {
            resolution = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-jit-speculation") == 0) {
            Core::HenBlockList::SetSpeculation(false);
        } else if (std::strcmp(argv[i], "--fresh-jit-list") == 0) {
            fresh_jit_list = true;
        } else if (std::strcmp(argv[i], "--jit-start-ms") == 0 && i + 1 < argc) {
            jit_start_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--cold-shaders") == 0) {
            cold_shaders = true;
        } else if (std::strcmp(argv[i], "--sync-shaders") == 0) {
            async_shaders = false;
#if HEN_DEV
        } else if (std::strcmp(argv[i], "--hen-exit-after") == 0 && i + 1 < argc) {
            exit_after_s = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--screenshot-frame") == 0 && i + 1 < argc) {
            // (as --screenshot-at, but by the program's vertical blank count: the same moment
            // of the program in a run that is slower or faster. Replaces --screenshot-at.)
            shot_by_frame = true;
            shot_times.clear();
            for (const char* c = argv[++i]; *c != 0;) {
                shot_times.push_back(std::atoi(c));
                while (*c != 0 && *c != ',') {
                    c++;
                }
                if (*c == ',') {
                    c++;
                }
            }
        } else if (std::strcmp(argv[i], "--screenshot-at") == 0 && i + 1 < argc) {
            for (const char* c = argv[++i]; *c != 0;) {
                shot_times.push_back(std::atoi(c));
                while (*c != 0 && *c != ',') {
                    c++;
                }
                c += *c == ',';
            }
        } else if (std::strcmp(argv[i], "--cold-driver") == 0) {
            // The driver's own shader cache (shared by every Vulkan program on the console):
            // emptied, for measuring a true first launch. Other programs compile again once.
            const std::string dir = "/config/ouncevk/shader-cache/";
            unsigned removed = 0;
            FileUtil::ForeachDirectoryEntry(
                nullptr, dir,
                [&removed](u64*, const std::string& directory, const std::string& name) {
                    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".bin") == 0) {
                        removed += FileUtil::Delete(directory + name) ? 1 : 0;
                    }
                    return true;
                });
            std::printf("hen: --cold-driver: removed %u file(s) from %s\n", removed, dir.c_str());
        } else if (std::strcmp(argv[i], "--crash-test") == 0) {
            std::printf("hen: --crash-test: writing to address 8\n");
            *reinterpret_cast<volatile int*>(8) = 1; // (to see the crash report)
        } else if (std::strcmp(argv[i], "--no-blackbox") == 0) {
            hen_blackbox = 0;
        } else if (std::strcmp(argv[i], "--profile") == 0) {
            hen_profile = 1;
        } else if (std::strcmp(argv[i], "--profile-pipelines") == 0) {
            hen_profile = 2;
        } else if (std::strcmp(argv[i], "--exit-counts") == 0) {
            hen_exit_counts = 1;
        } else if (std::strcmp(argv[i], "--jit-trace") == 0) {
            hen_jit_trace = 1; std::setvbuf(stdout, nullptr, _IONBF, 0);
        } else if (std::strcmp(argv[i], "--pipe-log") == 0) {
            hen_pipe_log = 1;
        } else if (std::strcmp(argv[i], "--dump-spirv-all") == 0) {
            hen_dump_spirv = 2;
        } else if (std::strcmp(argv[i], "--dump-spirv") == 0) {
            hen_dump_spirv = 1;
        } else if (std::strcmp(argv[i], "--movie-record") == 0 && i + 1 < argc) {
            movie_record = argv[++i];
        } else if (std::strcmp(argv[i], "--movie-play") == 0 && i + 1 < argc) {
            movie_play = argv[++i];
        } else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            // (test: the title is started this many times more in this process, as from the
            // title list, each for --hen-exit-after seconds)
            repeat_left = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--clock-info") == 0) {
            clock_info = true;
        } else if (std::strcmp(argv[i], "--mic") == 0 && i + 1 < argc) {
            mic = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--mic-probe") == 0) {
            mic_probe = true;
        } else if (std::strcmp(argv[i], "--keyboard-test") == 0) {
            char text[64];
            const int result = sw_keyboard("Keyboard test", "", 16, text, sizeof(text));
            std::printf("hen: keyboard test: result %d, text \"%s\"\n", result, text);
        } else if (std::strcmp(argv[i], "--crumbs") == 0) {
            hen_crumbs_enabled = true;
        } else if (std::strcmp(argv[i], "--vk-trace") == 0) {
            hen_vulkan_entry_point = &hen_vktrace_get_instance_proc_addr;
#endif
        } else if (argv[i][0] != '-') {
            program = argv[i];
        }
    }

    // Test runs nobody attends (a timed run, a replay, a chained run): development builds only.
#if HEN_DEV
    const auto unattended = [&] {
        return exit_after_s > 0 || ChainPort() > 0 || !movie_play.empty();
    };
#else
    const auto unattended = [] { return false; };
#endif

    FileUtil::SetUserPath("/switch/Kaizhar/");
    if (title_id_arg != 0) {
        program = Service::AM::GetTitleContentPath(Service::FS::MediaType::SDMC, title_id_arg);
        std::printf("hen: --title %016llX: %s\n", static_cast<unsigned long long>(title_id_arg),
                    program.c_str());
    }
    if (cold_shaders) {
        const std::string dir = "/switch/Kaizhar/shaders-cold/";
        FileUtil::DeleteDirRecursively(dir);
        FileUtil::CreateFullPath(dir);
        FileUtil::UpdateUserPath(FileUtil::UserPath::ShaderDir, "/switch/Kaizhar/shaders-cold");
        std::printf("hen: shader directory: %s\n",
                    FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir).c_str());
        std::printf("hen: --cold-shaders: empty shader cache in %s\n", dir.c_str());
    }
    Settings::values.async_shader_compilation = async_shaders;
    if (hen_sw_shaders) {
        Settings::values.use_hw_shader = false;
    }
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    Common::Log::Filter filter{Common::Log::Level::Info};
    if (debug_log) {
        // Everything the renderer says, written before the call returns (a crash loses queued
        // messages).
        filter.ParseFilterString("*:Info Render:Debug HW.GPU:Debug");
        Settings::values.instant_debug_log = true;
    }
    if (log_filter != nullptr) {
        filter.ParseFilterString(log_filter);
        Settings::values.instant_debug_log = true;
    }
    Common::Log::SetGlobalFilter(filter);

    // Every service module high-level emulated (as the other frontends default to).
    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false);
    }
    Settings::values.graphics_api =
        software_renderer ? Settings::GraphicsAPI::Software : Settings::GraphicsAPI::Vulkan;
    Settings::values.output_type = no_audio ? AudioCore::SinkType::Null : AudioCore::SinkType::Auto;
    // The microphone: Auto is the system's (audio_core/switch_input.cpp), opened when a title
    // starts sampling.
    Settings::values.input_type = AudioCore::InputType::Auto;
#if HEN_DEV
    if (mic != 1) {
        Settings::values.input_type = mic == 2 ? AudioCore::InputType::Static : AudioCore::InputType::Null;
    }
    std::printf("hen: mic: --mic %d (%s)\n", mic,
                std::string(AudioCore::GetInputDetails(Settings::values.input_type.GetValue()).name).c_str());
    if (mic_probe) {
        sw_mic_probe();
    }
#endif
    Settings::values.resolution_factor = static_cast<u32>(std::clamp(resolution, 0, 10)); // 0: what the layout shows
    Settings::values.use_cpu_jit = !interpreter;
    // (glslang's SPIR-V optimizer before the driver was tried: no gain)
    Settings::values.disable_spirv_optimizer = true;
    // The shader JIT's code would share pool chunks with the CPU JIT's, which are briefly
    // unmapped while the CPU JIT compiles (hen/jit/hen_jit.c): interpreter for the shaders the
    // GPU doesn't run itself.
    Settings::values.use_shader_jit = hen_shader_jit != 0; // (--no-shader-jit: the interpreter)

    InputCommon::Init();
    SwitchInput::Init();

    auto& system = Core::System::GetInstance();
    Frontend::RegisterDefaultApplets(system);
    system.RegisterSoftwareKeyboard(std::make_shared<SwitchKeyboard>());
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());

    // Without a program on the command line (opened from hbmenu): the title picker, and back to
    // it when a title ends. With one (nxlink, for testing): that title, then exit.
    const bool picker = program.empty();
    int rc = 0;
    for (;;) {
        std::string name = program;
        if (picker) {
            const auto choice = TitleMenu::Run();
            if (!choice) {
                break;
            }
            program = choice->path;
            name = choice->name;
        } else {
            TitleMenu::ApplySavedLayout(layout_override);
        }
        bool closing = false; // the system asked the app to close
#if HEN_DEV
        bool timed_stop = false; // --hen-exit-after ended this title
#endif
        std::string failure;
        try {
            EmuWindow_Switch window;
            system.ApplySettings();
            std::printf("hen: loading %s\n", program.c_str());
            hen_limiter_slept_us = hen_limiter_given_up_us = 0; // (time in the title list is not lag)
            // Homebrew that plays sound looks for the sound processor's firmware in
            // sdmc:/3ds/dspfirm.cdc (on a console the user dumps it there) and many programs stop
            // with an error without it. The high-level sound emulation used here never runs
            // that firmware and accepts any contents, so an absent file gets a placeholder.
            // A file the user put there is never touched.
            if (Settings::values.audio_emulation.GetValue() == Settings::AudioEmulation::HLE) {
                const std::string dir = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir) + "3ds/";
                const std::string firmware = dir + "dspfirm.cdc";
                if (!FileUtil::Exists(firmware)) {
                    FileUtil::CreateFullPath(dir);
                    std::string placeholder =
                        "Placeholder made by Kaizhar: not the sound processor's firmware.\n";
                    placeholder.resize(0x400, '\0');
                    if (FileUtil::WriteStringToFile(false, firmware, placeholder)) {
                        std::printf("hen: wrote a placeholder %s\n", firmware.c_str());
                    }
                }
            }
            // The cartridge slot: the last cartridge image started stays in it, as on a 3DS, so
            // that other applications (save managers, say) see that title.
            {
                const std::string slot_file =
                    FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "hen-cartridge.txt";
                std::string in_slot;
                FileUtil::ReadFileToString(true, slot_file, in_slot);
                system.EjectCartridge();
                system.InsertCartridge(program);
                if (!system.GetCartridge().empty()) {
                    if (in_slot != program) {
                        FileUtil::WriteStringToFile(true, slot_file, program);
                    }
                } else if (!in_slot.empty() && FileUtil::Exists(in_slot)) {
                    system.InsertCartridge(in_slot);
                }
                std::printf("hen: cartridge slot: %s\n",
                            system.GetCartridge().empty() ? "empty" : system.GetCartridge().c_str());
            }
#if HEN_DEV
            const std::string movie_dir = "/switch/Kaizhar/movies/";
            const std::string movie_file =
                movie_dir + (movie_record.empty() ? movie_play : movie_record) + ".ctm";
            if (!movie_record.empty()) {
                FileUtil::CreateFullPath(movie_dir);
                system.Movie().PrepareForRecording();
            } else if (!movie_play.empty()) {
                const auto valid = system.Movie().ValidateMovie(movie_file);
                if (valid == Core::Movie::ValidationResult::Invalid) {
                    std::printf("hen: movie: %s is missing or not a movie; playing without it\n",
                                movie_file.c_str());
                    movie_play.clear();
                } else {
                    system.Movie().PrepareForPlayback(movie_file);
                }
            }
#endif
            const auto load = system.Load(window, program);
            std::printf("hen: load: %s\n", StatusName(load));
#if HEN_DEV
            if (clock_info) {
                sw_clock_report("after load");
            }
#endif
            // Test runs only (a timed run, a replay, a chained run, or --keep-awake): nobody
            // touches the controller, and the system would dim the screen and go to sleep.
            const bool keep_awake_now = load == Core::System::ResultStatus::Success &&
                                        (keep_awake || unattended());
            if (keep_awake_now) {
                sw_keep_awake(true);
            }
#if HEN_DEV
            if (load == Core::System::ResultStatus::Success && !movie_record.empty()) {
                system.Movie().StartRecording(movie_file, "Kaizhar");
                std::printf("hen: movie: recording to %s\n", movie_file.c_str());
            } else if (load == Core::System::ResultStatus::Success && !movie_play.empty()) {
                system.Movie().SetPlaybackCompletionCallback([&system] {
                    std::printf("hen: movie: playback finished after %llu inputs\n",
                                static_cast<unsigned long long>(system.Movie().GetTotalInputCount()));
                });
                system.Movie().StartPlayback(movie_file);
                std::printf("hen: movie: playing %s (%llu inputs)\n", movie_file.c_str(),
                            static_cast<unsigned long long>(system.Movie().GetTotalInputCount()));
            }
            ReportJit("after load");
#endif
#if HEN_DEV
            hen_jit_profile_start();
#endif
            if (load == Core::System::ResultStatus::Success) {
                // As the other frontends do after loading: per-program settings, and the
                // renderer's shader caches (the Vulkan renderer has no shader cache object at
                // all before this).
                u64 program_id{};
                system.GetAppLoader().ReadProgramId(program_id);
                system.GPU().ApplyPerProgramSettings(program_id);
                // Homebrew has no title ID: its caches go by the file's name and size instead.
                hen_homebrew_cache_id = 0;
                if (program_id == 0) {
                    u64 hash = 0xcbf29ce484222325ull;
                    const std::string key = program.substr(program.rfind('/') + 1) + ":" +
                                            std::to_string(FileUtil::GetSize(program));
                    for (const char c : key) {
                        hash = (hash ^ static_cast<u8>(std::tolower(static_cast<unsigned char>(c)))) *
                               0x100000001b3ull;
                    }
                    hen_homebrew_cache_id = 0xFF00000000000000ull | (hash >> 8);
                    program_id = hen_homebrew_cache_id;
                    std::printf("hen: homebrew cache id %016llX\n",
                                static_cast<unsigned long long>(program_id));
                }
                system.GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(false, nullptr);
                if (jit_list) {
                    if (fresh_jit_list) {
                        FileUtil::Delete(fmt::format("/switch/Kaizhar/jit-fresh/{:016X}.blocks",
                                                     program_id));
                    }
                    Core::HenBlockList::Open(program_id, static_cast<unsigned>(jit_start_ms),
                                             fresh_jit_list ? "jit-fresh/" : "jit/");
#if HEN_DEV
                    ReportJitList();
#endif
                }

                const uint64_t start = sw_time_ms();
                uint64_t last_report = start;
                sw_watch_enable(true);
#if HEN_DEV
                // Screenshots for runs nobody watches: both screens at 400x480, as base64 text.
                constexpr u32 ShotW = 400, ShotH = 480;
                std::vector<u8> shot(ShotW * ShotH * 4);
                std::atomic<int> shot_state{0}; // 0 idle, 1 requested, 2 ready
                size_t next_shot = 0;
#endif
                for (;;) {
                    sw_watch_beat();
#if HEN_DEV
                    if (next_shot < shot_times.size() && shot_state == 0 &&
                        (shot_by_frame
                             ? hen_vblank_count >= static_cast<unsigned long long>(shot_times[next_shot])
                             : sw_time_ms() - start >=
                                   static_cast<uint64_t>(shot_times[next_shot]) * 1000)) {
                        shot_state = 1;
                        system.GPU().Renderer().RequestScreenshot(
                            shot.data(), [&shot_state](bool) { shot_state = 2; },
                            Layout::DefaultFrameLayout(ShotW, ShotH, false, false));
                    }
                    if (shot_state == 2) {
                        static const char digits[] =
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        std::printf("hen: shot begin %d %u %u\n", shot_times[next_shot], ShotW, ShotH);
                        std::string line;
                        for (size_t at = 0; at < shot.size(); at += 3) {
                            const u32 v = shot[at] << 16 | (at + 1 < shot.size() ? shot[at + 1] : 0) << 8 |
                                          (at + 2 < shot.size() ? shot[at + 2] : 0);
                            line += digits[v >> 18];
                            line += digits[v >> 12 & 63];
                            line += digits[v >> 6 & 63];
                            line += digits[v & 63];
                            if (line.size() >= 16000) {
                                std::printf("hen: shot %s\n", line.c_str());
                                line.clear();
                            }
                        }
                        std::printf("hen: shot %s\nhen: shot end\n", line.c_str());
                        next_shot++;
                        shot_state = 0;
                    }
#endif
                    if (!sw_main_loop()) {
                        closing = true;
                        break;
                    }
                    const auto status = system.RunLoop();
                    if (status != Core::System::ResultStatus::Success) {
                        std::printf("hen: run loop: %s (%s)\n", StatusName(status),
                                    system.GetStatusDetails().c_str());
                        if (status != Core::System::ResultStatus::ShutdownRequested) {
                            failure = StatusName(status);
                        }
                        break;
                    }
                    if (picker && EmuWindow_Switch::Input().menu_combo) {
                        std::printf("hen: both sticks pressed: back to the titles\n");
                        break;
                    }
                    const uint64_t now = sw_time_ms();
                    if (now - last_report >= 5000) {
                        last_report = now;
                        double hen_thread_cpu_ms = 0;
                        {
                            // How much of the interval this thread had a core (it runs the
                            // emulated CPU): less than the interval's busy time means it waited.
                            double wall_ms = 0, cpu_ms = 0;
                            const unsigned rc = hen_jit_thread_times(&wall_ms, &cpu_ms);
#if HEN_DEV
                            std::printf("hen: thread: interval %.0f ms, emulation thread on a core %.0f ms (0x%x)\n",
                                        wall_ms, cpu_ms, rc);
#else
                            (void)rc;
#endif
                            hen_thread_cpu_ms = cpu_ms;
                        }
                        const auto perf = system.GetAndResetPerfStats();
                        // The rare boot hang (three seen in about 110 boots: the program stops
                        // after FS InitializeWithSdkVersion, no frames, this thread idle): when
                        // nothing has been shown for two intervals and the emulated CPU hardly
                        // ran, say what every emulated thread is waiting for. Once per stretch.
                        {
                            static int idle_intervals = 0;
                            if (perf.game_fps < 0.5 && hen_thread_cpu_ms < 300.0) {
                                if (++idle_intervals == 2) {
                                    DumpEmulatedThreads(system);
                                }
                            } else {
                                idle_intervals = 0;
                            }
                        }
#if HEN_DEV
                        const double vblank_ms = perf.time_vblank_interval * 1000.0;
                        std::printf(
                            "hen: perf: speed %.0f%%, game %.1f fps, system %.1f fps; per vblank "
                            "%.1f ms: gpu %.1f, swap %.1f, svc %.1f, ipc %.1f, rest (cpu) %.1f\n",
                            perf.emulation_speed * 100.0, perf.game_fps, perf.system_fps, vblank_ms,
                            perf.time_gpu * 1000.0, perf.time_swap * 1000.0,
                            perf.time_hle_svc * 1000.0, perf.time_hle_ipc * 1000.0,
                            perf.time_remaining * 1000.0);
                        ReportJit("running");
                        hen_jit_profile_report();
                        // (the code of the most sampled blocks, for disassembling: what kind of
                        // program code the emulated CPU's time goes to)
                        for (const unsigned pc : hen_prof_top_guest) {
                            if (pc < 0x00100000 || pc >= 0x10000000) {
                                continue;
                            }
                            std::printf("hen: profile: code %08x:", pc);
                            for (unsigned k = 0; k < 24; k++) {
                                std::printf(" %08x", system.Memory().Read32((pc & ~1u) + 4 * k));
                            }
                            std::printf("\n");
                        }
                        ReportAudio();
                        ReportGfx();
                        if (!movie_record.empty()) {
                            // (kept on the card as it grows: quitting with HOME ends the
                            // program without the shutdown that would save it)
                            system.Movie().SaveMovie();
                        }
#endif
                        if (jit_list) {
                            Core::HenBlockList::Flush();
#if HEN_DEV
                            ReportJitList();
#endif
                        }
                    }
#if HEN_DEV
                    if (exit_after_s > 0 &&
                        now - start >= static_cast<uint64_t>(exit_after_s) * 1000) {
                        std::printf("hen: --hen-exit-after %d: stopping\n", exit_after_s);
                        closing = true;
                        timed_stop = true;
                        break;
                    }
#endif
                }
            } else {
                rc = 1;
                failure = StatusName(load);
            }
            if (jit_list) {
                Core::HenBlockList::Flush(true);
#if HEN_DEV
                ReportJitList();
#endif
            }
#if HEN_DEV
            if (!movie_record.empty() || !movie_play.empty()) {
                const auto inputs = system.Movie().GetCurrentInputIndex();
                system.Movie().Shutdown(); // (saves a recording)
                std::printf("hen: movie: ended at input %llu\n", static_cast<unsigned long long>(inputs));
            }
#endif
            system.Shutdown();
        } catch (const std::exception& e) {
            // (Vulkan-Hpp throws on errors: a title that can't start shouldn't take the app down.)
            std::printf("hen: exception: %s\n", e.what());
            failure = e.what();
            rc = 1;
        }
#if HEN_DEV
        if (clock_info) {
            sw_clock_report("title ending");
        }
#endif
        sw_cpu_clock_end();
        sw_watch_enable(false);
        hen_boost_off();
        if (keep_awake || unattended()) {
            sw_keep_awake(false);
        }
        std::printf("hen: title ended\n");
        const std::string program_error = HLE::Applets::ErrEula::TakeLastMessage();
        if (!program_error.empty()) {
            std::printf("hen: the program reported: %s\n", program_error.c_str());
        }
#if HEN_DEV
        if (!picker && timed_stop && repeat_left > 0) {
            repeat_left--;
            std::printf("hen: --repeat: starting the title again\n");
            continue;
        }
#endif
        if (!picker || closing) {
            break;
        }
        // An error the program itself wanted to show (through the error applet) before it ended.
        if (!program_error.empty()) {
            if (failure.empty() && picker && !closing) {
                std::string text = program_error;
                std::replace(text.begin(), text.end(), '\n', ' ');
                TitleMenu::ShowMessage(name + " reported an error", text.substr(0, 90),
                                       text.size() > 90 ? text.substr(90, 90) : std::string{});
            }
        }
        if (!failure.empty()) {
            TitleMenu::ShowMessage("Couldn't run the title", name, failure);
        }
        rc = 0;
    }
#if HEN_DEV
    hen_jit_profile_stop();
    ReportJit("at exit");
#endif
    SwitchInput::Shutdown();
    InputCommon::Shutdown();
    sw_mic_shutdown();
    Common::Log::Stop();
    hen_file_defer_finish();
    hen_boost_off();
    sw_cpu_boost_end();
    std::printf("hen: exiting\n");
#if HEN_CHAIN
    sw_chain_next(); // (--director: what the loader starts next, asked while the log still works)
#endif
    return rc;
}

// The main thread's stack is 1 MiB on this runtime, too small for the emulator (shader
// generation and compilation alone need more): everything runs on a thread with a 64 MiB stack.
int main(int argc, char** argv) {
    std::set_terminate(&OnTerminate);
    struct Args {
        int argc;
        char** argv;
        int rc;
        volatile bool done;
    } args{argc, argv, 1, false};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 * 1024);
    pthread_t thread;
    const int err = pthread_create(
        &thread, &attr,
        [](void* p) -> void* {
            auto* a = static_cast<Args*>(p);
            sw_watch_register();
            a->rc = EmulatorMain(a->argc, a->argv);
            sw_watch_enable(false);
            a->done = true;
            return nullptr;
        },
        &args);
    if (err != 0) {
        std::printf("hen: couldn't start the emulator thread: %d\n", err);
        return 1;
    }
    while (!args.done) {
        sw_watch_poll();
    }
    pthread_join(thread, nullptr);
    return args.rc;
}
