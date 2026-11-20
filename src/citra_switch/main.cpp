// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Kaizhar (Azahar for Switch 2 homebrew): a minimal frontend. Started without arguments it shows
// the title picker (title_menu.cpp). With a program on the command line it runs that program
// and exits:
//
//     kaizhar.nro [<path to .3dsx/.cci/.cxi/...> | --title <id in hex>] [options]
//
// Options:
//   --title ID       an installed title by its ID in hex, instead of a path
//   --renderer sw    the software renderer instead of Vulkan
//   --interpreter    the CPU interpreter instead of the JIT
//   --no-audio       no sound output
//   --res N          the internal resolution, 1 to 10 times the 3DS's (4 without the option);
//                    0: the smallest whole multiple that covers the top screen as it is shown
//   --layout N       the screen arrangement for this start instead of the saved one (an index
//                    into title_menu.cpp's list)
//   --room NICK[:PASSWORD]@ADDRESS[:PORT]  --host-room[=PORT]
//                    join a multiplayer room, or host one on this console (room_link.h; without
//                    them /switch/Kaizhar/multiplayer.txt decides, and the title list's
//                    Multiplayer screen)
//   --online-lle     the system modules online play needs run as the console's own code (also
//                    online_lle=1 in multiplayer.txt); --no-online-lle: off for this start
//   --debug-log      everything the renderer says, written before each call returns
//   --log FILTER     the emulator's log filter, e.g. "*:Info Service:Debug"
//
// Development builds (HEN_DEV) take more options, for test runs and comparisons: they are
// listed where they are parsed, in EmulatorMain below.
//
// User data is under /switch/Kaizhar/ on the SD card (a /switch/Azahar folder of the earlier
// test builds is renamed once at start: switch_platform.c).

#include <dirent.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <atomic>
#include <string>
#include <vector>

#include <pthread.h>
#if HEN_DEV
#include <fcntl.h>
#include <malloc.h>
#include <sys/stat.h>
#include <chrono>
#include <thread>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "audio_core/input_details.h"
#include "audio_core/sink_details.h"
#include "common/hen_frame_work.h"
#include "common/hen_stats.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/arm/dynarmic/hen_block_list.h"
#include "core/core.h"
#include "core/hle/kernel/hen_ipc_trace.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/wait_object.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/applets/swkbd.h"
#include "core/hle/applets/erreula.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/image_interface.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/apt/applet_manager.h"
#include "core/hle/service/apt/apt.h"
#include "core/hle/service/ir/ir.h"
#include "core/hle/service/service.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include <csetjmp>
#include "common/assert.h"
#include "citra_switch/guest_dns.h"
#include "idbe_passthrough.h" // hen/idbe
#include "citra_switch/link_3ds.h"
#include "citra_switch/tool_scan.h"

// network/artic_base/artic_base_client.cpp: a connection the frontend already holds, for the
// setup's client.
extern "C" int (*hen_artic_socket)(const char* address, unsigned port);
#include "input_common/main.h"
#include "citra_switch/emu_window_switch.h"
#include "citra_switch/input_switch.h"
#if HEN_DEV && defined(KAIZHAR_TLS)
#include "citra_switch/https_check.h"
#endif
#include "citra_switch/room_link.h"
#include "citra_switch/cache_clear.h"
#include "citra_switch/session_report.h"
#include "citra_switch/switch_platform.h"
#include "citra_switch/title_menu.h"
#include "video_core/gpu.h"
#include "video_core/hen_gpu_thread.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

// Where the Vulkan renderer gets its functions (vk_platform.cpp): null for the driver itself,
// or the call tracer (hen/vktrace).
extern "C" {
void sw_menu_shutdown(void); // (citra_switch/switch_menu.c: the title list's instance and device, at the program's end)
using VulkanEntryPoint = void (*(*)(void* instance, const char* name))();
VulkanEntryPoint hen_vulkan_entry_point = nullptr;
#if HEN_DEV
void (*hen_vktrace_get_instance_proc_addr(void* instance, const char* name))();
#endif
}

#if HEN_DEV
// "hen: driver:": which driver build this is, in the driver's words, printed once as the
// renderer makes its device. The renderer asks the entry point above for vkCreateDevice once;
// it gets the function below, which calls the real one and then asks the driver about the
// physical device. Nothing else goes through here. The driver's info string names the build
// it was made from; the version number alone is the same for every build.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                          const char* name);
namespace {
VulkanEntryPoint driver_line_next = nullptr;
PFN_vkCreateDevice driver_line_create_device = nullptr;
PFN_vkGetPhysicalDeviceProperties2 driver_line_properties = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL DriverLineCreateDevice(VkPhysicalDevice physical_device,
                                                      const VkDeviceCreateInfo* create_info,
                                                      const VkAllocationCallbacks* allocator,
                                                      VkDevice* device) {
    const VkResult result =
        driver_line_create_device(physical_device, create_info, allocator, device);
    static bool printed = false;
    if (result == VK_SUCCESS && !printed && driver_line_properties != nullptr) {
        printed = true;
        VkPhysicalDeviceDriverProperties driver{};
        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &driver;
        driver_line_properties(physical_device, &properties);
        const std::uint32_t version = properties.properties.driverVersion;
        std::printf("hen: driver: %s, %s, version %u.%u.%u, device %s\n", driver.driverName,
                    driver.driverInfo, VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                    VK_VERSION_PATCH(version), properties.properties.deviceName);
    }
    return result;
}

void (*DriverLineEntry(void* instance, const char* name))() {
    void (*function)() = driver_line_next(instance, name);
    if (function != nullptr && instance != nullptr && std::strcmp(name, "vkCreateDevice") == 0) {
        driver_line_create_device = reinterpret_cast<PFN_vkCreateDevice>(function);
        driver_line_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            driver_line_next(instance, "vkGetPhysicalDeviceProperties2"));
        return reinterpret_cast<void (*)()>(&DriverLineCreateDevice);
    }
    return function;
}
} // namespace
#endif

// An optional test library that is not included in this repository (HEN_CHAIN_DIR in
// CMakeLists.txt here; HEN_CHAIN is 1 only when HEN_DEV is on and that folder is given).
// Without it, which is how the repository builds, none of its calls are compiled and
// ChainPort below answers 0.
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

#if HEN_DEV
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
#endif
extern "C" unsigned hen_audio_events(unsigned* out, unsigned* entered);
extern "C" void hen_gfx_stats(unsigned long long* out);
extern "C" unsigned long long hen_blocks_compiled(void); // (every build: the session report)

extern "C" unsigned long long hen_homebrew_cache_id; // hen/jit/hen_jit.c
#if HEN_DEV
extern "C" int hen_link_table;
extern "C" int hen_jit_alias;
extern "C" int hen_publish_aligned; // hen/jit/hen_jit.c: --no-publish-aligned
extern "C" int hen_publish_batch;   // hen/jit/hen_jit.c: --no-publish-batch
extern "C" int hen_worker_depth_limit;
extern "C" int hen_emu_ahead;
extern "C" int hen_tier;
extern "C" int hen_boost_linger_ms;
extern "C" int hen_room_plain_io;
extern "C" int hen_fs_canon, hen_generic_fs, hen_pointer, hen_generic_seed;
#endif
static bool hen_sw_shaders = false;

// What the right stick is. It is the 3DS's C-Stick only while the running
// title reads one: ir:rst initialised (New 3DS titles), or the Circle Pad Pro asked for its
// state over ir:USER (core/hle/service/ir/ir.h). Otherwise it moves a pointer over the bottom
// screen and its click is the touch (switch_platform.c, sw_input_update). No list of titles.
// - An applet in front (the keyboard, the HOME Menu): the pointer, whatever the application
//   under it asked for; the application is not reading its pad meanwhile.
// - A title that never asks (most regular titles, system titles): the pointer throughout.
// - An input movie playing: the C-Stick, so that nothing but the movie's inputs can be
//   on the picture or in the title (the movie replaces what the title reads in any case:
//   core/movie.cpp, Handle).
// Asked on the emulation thread between passes of the run loop, at most every 50 ms; a line in
// the log at the title's start and at each change (the first forty).
#if HEN_DEV
extern "C" int hen_playing_game;  // core/hle/kernel/svc.cpp: --no-playing-game
#endif
extern "C" int hen_stick_pointer; // switch_platform.c: --no-stick-pointer
static int hen_stick_role = -1;   // (-1: not said yet for this title)
static uint64_t hen_stick_role_asked, hen_stick_role_start;
static unsigned hen_stick_role_changes;
static void RightStickRole(Core::System& system) {
    const uint64_t now = sw_time_ms();
    if (hen_stick_role >= 0 && now - hen_stick_role_asked < 50) {
        return;
    }
    hen_stick_role_asked = now;
    enum { Pointer, PointerApplet, CStickRst, CStickCpp, CStickMovie, CStickFlag };
    int role = Pointer;
    if (!hen_stick_pointer) {
        role = CStickFlag;
    } else if (system.Movie().GetPlayMode() == Core::Movie::PlayMode::Playing) {
        role = CStickMovie;
    } else if (Service::IR::hen_rst_in_use.load() || Service::IR::hen_cpp_in_use.load()) {
        role = Service::IR::hen_rst_in_use.load() ? CStickRst : CStickCpp;
        const auto apt = Service::APT::GetModule(system);
        if (apt && apt->GetAppletManager() && apt->GetAppletManager()->HenAppletInFront()) {
            role = PointerApplet;
        }
    }
    if (role == hen_stick_role) {
        return;
    }
    static const char* const text[] = {
        "the touch pointer, its click the touch (the title reads no C-Stick)",
        "the touch pointer, its click the touch (an applet is in front of a title that reads the C-Stick)",
        "the C-Stick (the title initialised ir:rst)",
        "the C-Stick (the title asked the Circle Pad Pro for its state)",
        "the C-Stick (an input movie is playing)",
        "the C-Stick (--no-stick-pointer)",
    };
    if (hen_stick_role < 0) {
        hen_stick_role_start = now;
        hen_stick_role_changes = 0;
        std::printf("hen: right stick: %s\n", text[role]);
    } else if (++hen_stick_role_changes <= 40) {
        std::printf("hen: right stick: now %s, %.1f s into the title%s\n", text[role],
                    static_cast<double>(now - hen_stick_role_start) / 1000.0,
                    hen_stick_role_changes == 40 ? " (the last change said)" : "");
    }
    hen_stick_role = role;
    sw_right_stick_role(role >= CStickRst);
}
#if HEN_DEV
extern "C" int hen_stale_profile;
// (first-time graphics work off the GPU thread; hen/jit/hen_jit.c)
extern "C" int hen_sw_standin, hen_etc1_workers, hen_vs_jit_async, hen_vs_jit_small;
// (image memory in small blocks, the number of compile workers)
extern "C" int hen_vma_small_blocks, hen_compile_workers;
extern "C" unsigned long long hen_image_memory_stats[8];
extern "C" int hen_image_memory_now(unsigned long long* out); // renderer_vulkan/vk_instance.cpp
extern "C" int hen_sw_standin_area, hen_gpu_headroom;
extern "C" unsigned long long hen_present_wait_mean_ns, hen_gpu_headroom_flips, hen_gpu_headroom_frames_without;
extern "C" unsigned long long hen_sw_standin_stats[4], hen_vs_jit_stats[6],
    hen_here_builds[3], hen_etc1_split[2];
extern "C" int hen_cache_regen_sync;
extern "C" unsigned long long hen_vblank_count;
// hen/platform/hen_thread.c: which host core the emulation, GPU and audio threads are
// on, the cores every thread may use.
extern "C" void hen_core_sampling_start(void);
extern "C" int hen_core_report(int kind, char* out, size_t size);
#endif
extern "C" void hen_thread_name(const char* name);
#if HEN_DEV
extern "C" int hen_thread_table(const char* when);
extern "C" int hen_thread_count(void);
// Same file: the CPU time of every thread, the cores' idle time (a thread of its own
// prints that line), a speed probe. hen/jit/hen_jit.c: where the emulation thread's time goes.
extern "C" void hen_thread_cpu_start(void);
extern "C" void hen_thread_cpu_report(unsigned seq);
extern "C" void hen_core_idle_start(void);
extern "C" void hen_core_idle_ask(unsigned seq);
extern "C" void hen_core_idle_stop(void);
extern "C" void hen_probe_start(void);
extern "C" void hen_probe_stop(void);
extern "C" int hen_probe_run(double* out);
extern "C" int hen_emu_stats_on;
extern "C" void hen_emu_stats(unsigned long long* ns, unsigned long long* count, unsigned long long* dispatch,
                              unsigned long long* layout);
extern "C" unsigned long long hen_jit_code_ticks_total(void);
// The background re-sort: hen/jit/hen_jit.c, hen/jit/hen_resort.h
extern "C" int hen_resort;
extern "C" int hen_resort_at_ms;
extern "C" int hen_resort_release;
extern "C" int hen_resort_poison;
extern "C" int hen_resort_chunk;
extern "C" void hen_resort_stats(long long* out);
extern "C" void hen_layout_stats(unsigned long long* out);
extern "C" unsigned long long hen_jit_interpreted_ns(void);
#endif
namespace Core {
void HenEmuCoresReport(unsigned seq, bool print, u32 num_cores); // (core/core.cpp)
}
namespace Kernel {
void HenSvcReport(unsigned seq, bool print); // (core/hle/kernel/svc.cpp)
}
namespace Core {
int HenEventStats(const char** names, unsigned long long* count, unsigned long long* ns, int max,
                  unsigned long long* total); // core/core_timing.cpp
}
#if HEN_DEV
extern "C" unsigned long long hen_shown_hist[11]; // video_core/renderer_vulkan/vk_present_window.cpp
// (same file) the GPU's time to spare by the render time and by the present fence
extern "C" int hen_headroom_render, hen_headroom_hi_us, hen_headroom_lo_us, hen_headroom_by_fence,
    hen_headroom_by_render;
extern "C" unsigned long long hen_render_latency_mean_ns, hen_render_latency_stats[4];
extern "C" int hen_assert_test_frame;
extern "C" unsigned long long hen_swap_ns[8];
#endif
// What only a development build's flags can change is a constant in the other builds.
#if HEN_DEV
#define HEN_TEST_SWITCH
#else
#define HEN_TEST_SWITCH const
#endif
// --crash-test SECONDS, development builds: the emulation thread writes to address 8 that long
// after a title started, to see the crash record on the card (switch_platform.c).
#if HEN_DEV
static unsigned crash_test_s = 0;
#endif
// --click-at SECONDS: one click of the left stick that long after a title started,
// for a route test of what the click is set to do.
static HEN_TEST_SWITCH unsigned click_at_s = 0;
// The automatic internal resolution follows the GPU: one factor down when the GPU has been the
// limit for four seconds (the rule is in the run loop). --no-auto-resolution-step turns it off;
// --resolution-step-at SECONDS forces one step then, to try the path on any route.
static bool resolution_step = true;
static HEN_TEST_SWITCH unsigned resolution_step_at_s = 0;
extern "C" unsigned long long hen_render_wait_ns; // video_core/renderer_vulkan/vk_present_window.cpp
extern "C" long long hen_limiter_slept_total_us;  // core/perf_stats.cpp

/// One factor down, between two passes of the run loop with the GPU thread idle. The renderer
/// sees the new factor at its next frame and drops its surfaces (rasterizer_cache.h); the
/// factor stays for the title's run. False: already at the 3DS's own resolution.
static bool ResolutionStepDown(EmuWindow_Switch& window, const char* why, unsigned long long seconds) {
    const u32 set = Settings::values.resolution_factor.GetValue();
    const u32 factor = set != 0 ? set : window.GetFramebufferLayout().GetScalingRatio();
    if (factor <= 1) {
        return false;
    }
    VideoCore::HenGpu::Drain(VideoCore::HenGpu::WaitOther);
    Settings::values.resolution_factor = factor - 1;
    std::printf("hen: resolution: %ux -> %ux at %llu s: %s\n", factor, factor - 1, seconds, why);
    SessionReport::Note(fmt::format("internal resolution {}x -> {}x at {} s: {}", factor, factor - 1, seconds, why));
    return true;
}

/// A short click of the left stick, between two passes of the run loop. The arrangement is
/// changed with the GPU thread idle, and the title keeps the internal resolution it started
/// with: left to follow the new arrangement it would empty the renderer's whole surface cache.
static void StickClicked(TitleMenu::StickAction action, EmuWindow_Switch& window) {
    switch (action) {
    case TitleMenu::StickAction::NextArrangement:
    case TitleMenu::StickAction::SwapScreens: {
        VideoCore::HenGpu::Drain(VideoCore::HenGpu::WaitOther);
        if (Settings::values.resolution_factor.GetValue() == 0) {
            Settings::values.resolution_factor = window.GetFramebufferLayout().GetScalingRatio();
        }
        const char* name = "swapped";
        if (action == TitleMenu::StickAction::NextArrangement) {
            name = TitleMenu::NextArrangement();
        } else {
            Settings::values.swap_screen = !Settings::values.swap_screen.GetValue();
        }
        window.Relayout();
        std::printf("hen: screens: now %s, resolution %u\n", name, Settings::values.resolution_factor.GetValue());
        break;
    }
    case TitleMenu::StickAction::Mute:
        Settings::values.audio_muted = !Settings::values.audio_muted;
        std::printf("hen: sound: %s\n", Settings::values.audio_muted ? "muted" : "on again");
        break;
    case TitleMenu::StickAction::Nothing:
        break;
    }
}
// (hen/jit/hen_jit.c)
#if HEN_DEV
extern "C" int hen_present_cpu_acquire;
extern "C" unsigned long long hen_present_acquire_wait[4];
extern "C" void hen_jit_link_skip_stats(unsigned long long* out);
extern "C" void hen_jit_memory_stats(unsigned long long* out);
#endif
extern "C" int hen_online_summary(char* out, unsigned long size); // core/hle/service/soc/soc_u.cpp
#if HEN_DEV
extern "C" unsigned hen_jit_blocks_that_fit(size_t size);
extern "C" int hen_jit_lazy_write, hen_code_cache_mib, hen_buffer_grow, hen_buffer_sized;
extern "C" int hen_jit_clear_at_ms;
extern "C" unsigned long long hen_generic_stats[3], hen_generic_unsupported;
extern "C" unsigned long long hen_generic_frame_stats[6]; // (vk_rasterizer.cpp counts)
extern "C" unsigned long long hen_part_cache_hits; // (the generic fragment shader's part)
extern "C" unsigned long long hen_fs_configs_raw, hen_fs_configs_built;
extern "C" int hen_bump;
extern "C" int hen_inline_exclusive;
extern "C" int hen_read_table; // hen/jit/hen_jit.c; core/memory.h, PageTable::Pointers
extern "C" int hen_fold_cond_branch; // hen/jit/hen_jit.c; dynarmic's a32_branch.cpp, arm_B
extern "C" int hen_code_align_2m;    // hen/jit/hen_jit.c, hen_jit_create
extern "C" int hen_compact_links;    // hen/jit/hen_jit.c; dynarmic's address_space.cpp, HenLinkStub
#endif
extern "C" void hen_file_defer_finish(void); // switch_file_defer.c
#if HEN_DEV
extern "C" int hen_cpu_shade_share;
extern "C" int hen_max_lag_ms;
extern "C" int hen_file_defer; // switch_file_defer.c
extern "C" int hen_driver_cache_memory; // switch_file_defer.c: --no-driver-cache-memory
extern "C" unsigned long long hen_jit_code_ticks(void);
extern "C" unsigned long long hen_fast_lane_skips;
extern "C" unsigned long long hen_standin_stats[7];
#endif
extern "C" unsigned long long hen_present_gaps[6];
#if HEN_DEV
extern "C" unsigned long long hen_standin_part_ns[6];
extern "C" unsigned long long hen_core_ns[4], hen_core_ticks[4], hen_core_runs[4];
extern "C" int hen_depth_lag;
extern "C" int hen_lazy_resched;
extern "C" int hen_n3ds_clock; // hen/jit/hen_jit.c; core/core_timing.cpp
extern "C" int hen_n3ds_clock_high; // hen/jit/hen_jit.c: --n3ds-clock-high
extern "C" int hen_am_scan_eager; // core/hle/service/am/am.cpp
extern "C" int hen_am_title_list; // core/hle/service/am/am.cpp: --no-am-title-list
extern "C" int hen_driver_cache_keep;   // video_core/renderer_vulkan/vk_pipeline_cache.cpp: --no-driver-cache-keep
extern "C" int hen_jit_teardown_thread; // hen/jit/hen_jit.c: --no-jit-teardown-thread
extern "C" int hen_n3ds_mode_keep;      // hen/jit/hen_jit.c: --no-n3ds-mode-keep
extern "C" int hen_launch_cache;        // hen/jit/hen_jit.c: --no-launch-cache
extern "C" int hen_jit_park;            // hen/jit/hen_jit.c: --no-jit-park
extern "C" int hen_jit_refuse_test; // hen/jit/hen_jit.c: --jit-refuse-test MIB
extern int hen_no_shutdown_fix; // core/core.cpp
extern "C" int hen_trace_modules; // core/hle/kernel/hen_ipc_trace.cpp: --trace-modules
extern "C" int hen_trace_sockets; // core/hle/service/soc/soc_u.cpp: --trace-sockets
extern "C" int hen_soc_rules;     // core/hle/service/soc/soc_u.cpp: --no-soc-rules
#endif
namespace Core {
bool HenClockStats(double* out); // core/core_timing.cpp
}
extern "C" int hen_gpu_thread;       // hen/jit/hen_jit.c; video_core/hen_gpu_thread.cpp
#if HEN_DEV
extern "C" int hen_gpu_defer_writes;
extern "C" int hen_live_fpsr;
extern "C" int hen_direct_links;
extern "C" int hen_load_boost;
extern "C" unsigned long long hen_cpu_shade_ns_per_vertex;
#endif
extern "C" void hen_boost_off(void);
extern "C" void hen_boost_stats(unsigned long long* out);
#if HEN_DEV
extern "C" void hen_jit_link_stats(unsigned long long* out);
extern "C" void hen_jit_compact_stats(unsigned long long* out);
extern "C" unsigned long long hen_resched_skipped;
#endif
extern "C" long long hen_limiter_given_up_us, hen_limiter_slept_us;
extern "C" int hen_shader_jit;
#if HEN_DEV
extern "C" void hen_gfx_part_stats(unsigned long long* out);
extern "C" void hen_gfx_part_repeat_stats(unsigned long long* out);
extern "C" void hen_jit_tier_stats(unsigned long long* out);
extern "C" void hen_jit_early_stats(unsigned long long* out);
extern "C" int hen_publish_early;
extern "C" void hen_jit_lock_stats(unsigned long long* out);
extern "C" void hen_jit_slow_reads(unsigned* megabyte, unsigned* count, int top);
extern "C" unsigned hen_jit_slow_read_last(unsigned megabyte);
extern "C" void hen_jit_guest_stats(unsigned* exclusive, unsigned* svc, unsigned* svc_count, int top);
extern "C" unsigned long long hen_jit_svc_times(unsigned* svc, unsigned long long* ns, unsigned* count, int top);
extern "C" void hen_jit_publish_stats(unsigned long long* out);
extern "C" void hen_jit_publish_path_stats(unsigned long long* out);
extern "C" unsigned hen_cia_chunk_bytes;
extern "C" int hen_pipeline_libraries;
extern "C" int hen_depth_scale_blit;
extern "C" int hen_early_depth;                          // hen_jit.c: --no-early-depth
extern "C" int hen_generic_early_depth;                  // hen_jit.c: --no-generic-early-depth
extern "C" int hen_generic_shading_rate;                 // hen_jit.c: --generic-shading-rate N
extern "C" unsigned long long hen_early_depth_stats[3]; // vk_rasterizer.cpp counts
extern "C" int hen_recycle_surfaces;                     // hen_jit.c: --recycle-surfaces
extern "C" unsigned hen_recycle_cap_mib, hen_recycle_age_frames;
#endif

// Progress counters for the boot-hang report (hle_ipc.cpp, core_timing.cpp, thread.cpp).
extern std::atomic<unsigned> hen_async_queued, hen_async_started, hen_async_finished, hen_async_threw,
    hen_async_negative, hen_ts_pushed, hen_ts_moved, hen_wake_fired, hen_wake_not_waiting;

namespace {

// The statistics reports: development builds only.
#if HEN_DEV
void ReportAudio() {
    unsigned d[11]{}, o[5]{};
    hen_audio_dsp_stats(d);
    sw_audio_stats(o);
    // One line per event: the stretcher entered, the stretcher left.
    unsigned events[4 * 32], entered = 0;
    const unsigned count = hen_audio_events(events, &entered);
    static const char* const kinds[] = {"?", "?", "stretcher entered", "stretcher left"};
    for (unsigned i = 0; i < count; i++) {
        const unsigned* e = events + 4 * i;
        std::printf("hen: audio event: %.3f s since the sound started: %s, %u ms%s, fifo %u frames\n",
                    e[0] / 1000.0, kinds[e[1] < 4 ? e[1] : 0], e[2],
                    e[1] == 2 ? " missing" : "", e[3]);
    }
    if (d[0] == 0) {
        return;
    }
    std::printf("hen: audio shortage: fewest queued %u of 5; run so far: stretcher entered %u times\n", o[4],
                entered);
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
// (g: hen_gfx_stats; boost: hen_boost_stats. Both empty their counters when read, and the
// session report wants them too: the run loop reads them once for both.)
void ReportGfx(const unsigned long long* g, const unsigned long long* boost) {
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
        // The same draws by frame and by variant of the shader: a scene start puts them
        // in a frame or two, and that frame is what the GPU is late with.
        if (hen_generic_frame_stats[4] + hen_generic_frame_stats[5] != 0) {
            std::printf("hen: gfx: generic draws: %llu with the depth write, %llu without; %llu "
                        "frame(s) had any; most in one frame %llu (with the depth write %llu, "
                        "without %llu)\n",
                        hen_generic_frame_stats[4], hen_generic_frame_stats[5],
                        hen_generic_frame_stats[0], hen_generic_frame_stats[1],
                        hen_generic_frame_stats[2], hen_generic_frame_stats[3]);
            for (auto& count : hen_generic_frame_stats) {
                count = 0;
            }
        }
        // What still ran on the GPU thread, and what no longer does.
        if (hen_here_builds[0] != 0) {
            std::printf("hen: gfx: pipelines built on the GPU thread: %llu (%.1f ms, longest %.1f)\n",
                        hen_here_builds[0], hen_here_builds[1] / 1e6, hen_here_builds[2] / 1e6);
            hen_here_builds[0] = hen_here_builds[1] = hen_here_builds[2] = 0;
        }
        // (every interval, so that the GPU's state is on record beside the counts)
        std::printf("hen: gfx: software draws whose pipeline wasn't ready: %llu drawn with the generic "
                    "fragment shader (%llu more because they cover little), %llu waited because it "
                    "couldn't take them, %llu waited because the GPU has no time to spare; present "
                    "wait %.1f ms (running mean), GPU time to spare: %s, %llu frame(s) without, %llu "
                    "change(s)\n",
                    hen_sw_standin_stats[0], hen_sw_standin_stats[3], hen_sw_standin_stats[1],
                    hen_sw_standin_stats[2], hen_present_wait_mean_ns / 1e6, hen_gpu_headroom ? "yes" : "no",
                    hen_gpu_headroom_frames_without, hen_gpu_headroom_flips);
        for (auto& value : hen_sw_standin_stats) {
            value = 0;
        }
        hen_gpu_headroom_frames_without = hen_gpu_headroom_flips = 0;
        if (hen_vs_jit_stats[4] != 0 || hen_vs_jit_stats[0] != 0) {
            std::printf("hen: gfx: vertex programs: CPU "
                        "form: %llu compiled on the GPU thread (%.1f ms), %llu by a worker "
                        "(%llu draws, %llu vertices interpreted meanwhile; %llu draws waited)\n",
                        hen_vs_jit_stats[4], hen_vs_jit_stats[5] / 1e6,
                        hen_vs_jit_stats[0], hen_vs_jit_stats[1], hen_vs_jit_stats[2],
                        hen_vs_jit_stats[3]);
            for (auto& value : hen_vs_jit_stats) {
                value = 0;
            }
        }
        // ... and which images: by size, and the surfaces asked for by who asked
        // (common/hen_frame_work.h).
        const unsigned long long* const sf = hen_surface_stats;
        const unsigned long long surfaces_asked =
            sf[HEN_SURFACE_FRAMEBUFFER] + sf[HEN_SURFACE_TEXTURE] + sf[HEN_SURFACE_MIP] +
            sf[HEN_SURFACE_TRANSFER] + sf[HEN_SURFACE_OTHER];
        if (hen_image_memory_stats[0] != 0 || surfaces_asked != 0) {
            unsigned long long now[4]{};
            hen_image_memory_now(now);
            std::printf("hen: gfx: image memory: %llu images made (%llu in memory of their own, %llu "
                        "bound into memory over 8 MiB); now %llu blocks, %.1f MiB, %llu allocations, "
                        "%.1f MiB used; by size: %llu under 4 MiB, %llu of 4 to 8, %llu over 8; "
                        "surfaces asked for: %llu framebuffer, %llu texture, %llu temporary mip, "
                        "%llu transfer, %llu other (%llu new, %llu recycled, %llu taken back at "
                        "their address); %llu copy images, %llu scaled images made anew\n",
                        hen_image_memory_stats[0], hen_image_memory_stats[1], hen_image_memory_stats[2],
                        now[0], now[2] / 1048576.0, now[1], now[3] / 1048576.0,
                        sf[HEN_IMAGE_UNDER_4], sf[HEN_IMAGE_4_TO_8], sf[HEN_IMAGE_OVER_8],
                        sf[HEN_SURFACE_FRAMEBUFFER], sf[HEN_SURFACE_TEXTURE], sf[HEN_SURFACE_MIP],
                        sf[HEN_SURFACE_TRANSFER], sf[HEN_SURFACE_OTHER], sf[HEN_SURFACE_NEW],
                        sf[HEN_SURFACE_RECYCLED], sf[HEN_SURFACE_RECYCLED_SAME], sf[HEN_IMAGE_COPY],
                        sf[HEN_IMAGE_SCALE_UP]);
            hen_image_memory_stats[0] = hen_image_memory_stats[1] = hen_image_memory_stats[2] = 0;
            for (int i = 0; i <= HEN_SURFACE_RECYCLED_SAME; i++) {
                hen_surface_stats[i] = 0;
            }
        }
        // The pool of --recycle-surfaces: hits are "recycled" in the line above, misses
        // are "new".
        if (hen_recycle_surfaces) {
            std::printf("hen: gfx: surface recycling: %llu surfaces kept, %llu destroyed for age, "
                        "%llu for room; the pool holds %llu surfaces, %.1f MiB (at most %.1f MiB "
                        "since the last report, cap %u)\n",
                        sf[HEN_POOL_KEPT], sf[HEN_POOL_EVICTED_AGE], sf[HEN_POOL_EVICTED_SIZE],
                        sf[HEN_POOL_SURFACES], sf[HEN_POOL_BYTES] / 1048576.0,
                        sf[HEN_POOL_BYTES_PEAK] / 1048576.0, hen_recycle_cap_mib);
            hen_surface_stats[HEN_POOL_KEPT] = hen_surface_stats[HEN_POOL_EVICTED_AGE] =
                hen_surface_stats[HEN_POOL_EVICTED_SIZE] = 0;
            hen_surface_stats[HEN_POOL_BYTES_PEAK] = hen_surface_stats[HEN_POOL_BYTES];
        }
        // What the renderer recorded. Counted on the thread that
        // records; a display transfer is also one of the blits.
        {
            unsigned long long w[HEN_FRAME_WORK];
            unsigned long long any = 0;
            for (int i = 0; i < HEN_FRAME_WORK; i++) {
                w[i] = hen_frame_work[i];
                hen_frame_work[i] = 0;
                any |= w[i];
            }
            if (any != 0) {
                std::printf("hen: gfx: frame work: %llu passes begun, %llu whole-surface clears, "
                            "%llu partial clears, %llu feedback copies (%.1f MiB), scale blits "
                            "%llu up and %llu down, %llu display transfers, %llu uploads, %llu "
                            "submits; %llu surface copies, %llu surface blits, %llu downloads; "
                            "%llu draws, of which a pass began at: %llu for another area, %llu "
                            "for another depth attachment, %llu for another colour attachment, "
                            "%llu for another target, %llu after something ended the same pass\n",
                            w[HEN_WORK_PASSES], w[HEN_WORK_CLEARS_WHOLE], w[HEN_WORK_CLEARS_PART],
                            w[HEN_WORK_FEEDBACK_COPIES], w[HEN_WORK_FEEDBACK_BYTES] / 1048576.0,
                            w[HEN_WORK_SCALE_UP], w[HEN_WORK_SCALE_DOWN],
                            w[HEN_WORK_DISPLAY_TRANSFERS], w[HEN_WORK_UPLOADS], w[HEN_WORK_SUBMITS],
                            w[HEN_WORK_COPIES], w[HEN_WORK_BLITS], w[HEN_WORK_DOWNLOADS],
                            w[HEN_WORK_DRAWS], w[HEN_WORK_PASS_AREA], w[HEN_WORK_PASS_DEPTH],
                            w[HEN_WORK_PASS_COLOUR], w[HEN_WORK_PASS_TARGET],
                            w[HEN_WORK_PASS_RESUMED]);
            }
        }
        if (hen_etc1_split[0] != 0) {
            std::printf("hen: gfx: ETC1 decodes shared with the workers: %llu (%llu tiles)\n",
                        hen_etc1_split[0], hen_etc1_split[1]);
            hen_etc1_split[0] = hen_etc1_split[1] = 0;
        }
        // Draws by where their depth comes from. With the flag off the second figure is
        // what the flag would move.
        if (hen_early_depth_stats[0] + hen_early_depth_stats[1] != 0) {
            std::printf("hen: gfx: early depth: %s: %llu draws %s the shader's depth write (%llu of "
                        "them with no discard in the shader), %llu with it\n",
                        hen_early_depth ? "on" : "off", hen_early_depth_stats[1],
                        hen_early_depth ? "without" : "could go without", hen_early_depth_stats[2],
                        hen_early_depth_stats[0]);
            hen_early_depth_stats[0] = hen_early_depth_stats[1] = hen_early_depth_stats[2] = 0;
        }
        static unsigned long long told_hits;
        if (hen_part_cache_hits != told_hits) {
            std::printf("hen: gfx: fragment shader parts reused: %llu (in all)\n", hen_part_cache_hits);
            told_hits = hen_part_cache_hits;
        }
    }
    {
        if (boost[0] != 0 || boost[1] != 0) {
            std::printf("hen: boost: on %llu ms, %llu switches; asked for by: loading %llu, new code %llu, "
                        "pipelines %llu, busy %llu, held (New 3DS mode or handheld) %llu (vertical blanks); %s, longest switch %llu us\n",
                        boost[0], boost[1], boost[2], boost[3], boost[4], boost[7], boost[8],
                        boost[6] ? "by the CPU clock" : "by the system's boost mode", boost[5]);
        }
        // (--n3ds-clock-high, hen_jit.c boost_tier: its own line, so the one above keeps its form)
        if (boost[9] != 0) {
            std::printf("hen: boost: high tier (--n3ds-clock-high, 1702.4 MHz): %llu vertical blanks at the high "
                        "rate, %llu switches between the two rates, short of CPU on %llu vertical blanks%s\n",
                        boost[10], boost[11], boost[12],
                        boost[9] == 2 ? "; the high rate was refused: not used in this run" : "");
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
    case Status::ErrorArticDisconnected:
        return "no connection to the setup tool, or it broke";
    default:
        return "other";
    }
}

#if HEN_DEV
extern "C" void hen_jit_compile_stats(unsigned long long* out);
extern "C" void hen_jit_miss_stats(unsigned long long* out);
#endif
extern "C" unsigned hen_jit_thread_times(double* wall_ms, double* cpu_ms);
#if HEN_DEV
extern "C" void hen_jit_run_stats(unsigned long long* out);
extern "C" int hen_fast_dispatch;
#endif

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
            // (so far, like the line above: which way the requests went, and the compile
            // worker's batches, one request each)
            unsigned long long path[7];
            hen_jit_publish_path_stats(path);
            std::printf("hen: jit publish path: %llu requests sent from page-aligned buffers here, %llu through "
                        "the library (%llu of them too large for the buffer)%s; worker batches: %llu, %.1f "
                        "blocks each, %llu of more than 8 blocks, %llu of those ended by the program "
                        "interpreting%s\n",
                        path[0], path[1], path[2], hen_publish_aligned ? "" : " (--no-publish-aligned)", path[3],
                        path[3] != 0 ? static_cast<double>(path[4]) / path[3] : 0.0, path[5], path[6],
                        hen_publish_batch ? "" : " (--no-publish-batch)");
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
    // The heap the code buffers hold: every JIT has a 128 MiB write area and, once it has
    // code, a 130 MiB block the code is mapped from; a re-sort's second buffer is the same again.
    {
        unsigned long long m[7];
        hen_jit_memory_stats(m);
        // The allocator's own figures, and what they do not say: how many more blocks
        // of a code buffer's size it could give (a block over 32 MiB needs wholly free 32 MiB
        // units side by side; found by trying).
#if HEN_DEV
        const unsigned fit = hen_jit_blocks_that_fit(static_cast<size_t>(hen_code_cache_mib) << 20);
#else
        const unsigned fit = 0;
#endif
        std::printf("hen: jit memory: %llu code buffers alive (%llu with code mapped), %llu MiB of heap taken for "
                    "them, %.1f MiB of code written at most; %llu heap block(s) set aside after a refused mapping; "
                    "heap %llu MiB, %llu MiB free by the allocator's count, room for %u%s more blocks of %d MiB (a "
                    "JIT with code needs two)\n",
                    m[0], m[1], m[2] >> 20, m[3] / 1048576.0, m[4], m[5] >> 20, m[6] >> 20, fit, fit >= 80 ? "+" : "",
                    hen_code_cache_mib);
    }
    sw_heap_report(); // (development builds: where the heap is)
    // Frames whose link upgrades were left out because the worker had the compile mutex.
    unsigned long long link_skips[2];
    hen_jit_link_skip_stats(link_skips);
    std::printf("hen: jit links: %llu sites written as direct branches, %llu through link slots, "
                "%llu of those made direct since; compact sites: %llu direct, %llu through a stub, "
                "%llu of those made direct since (%llu left: their block was invalidated), %llu stubs; "
                "%.0f bytes a block, %.0f with the stubs; upgrades left out for %llu frames so far "
                "(compile mutex taken), %llu in a row at most since the last line\n",
                links[0], links[1], links[2], compact[0], compact[1], compact[2], compact[5], compact[3],
                blocks[1] != 0 ? static_cast<double>(compact[4]) / blocks[1] : 0.0,
                blocks[1] != 0 ? static_cast<double>(compact[4] + 40 * compact[3]) / blocks[1] : 0.0,
                link_skips[0], link_skips[1]);
}

/// --file-noise KIND@SECONDS[,KIND@SECONDS...], a test of what slows the JIT's publish requests
/// (hen/jit/hen_jit.c): a spare thread that, SECONDS into the title, does one kind of work in a
/// loop for three seconds, and then says what the publish requests cost during those three
/// seconds and during the three before. Kinds:
///   read     4 KiB reads of the title's file into a heap block that is not page-aligned
///   aligned  the same reads into a block on a page boundary, one page long
///   stat     stat() of the title's file (a file request without a data buffer)
///   malloc   heap blocks of mixed sizes taken and given back, no file calls
/// Several in one run: leave six seconds or more between them. The thread is joined when the
/// title ends.
class FileNoise {
public:
    FileNoise(const std::string& spec, const std::string& file_) : file(file_) {
        for (std::size_t at = 0; at < spec.size();) {
            const std::size_t end = std::min(spec.find(',', at), spec.size());
            const std::string one = spec.substr(at, end - at);
            const std::size_t sign = one.find('@');
            if (sign != std::string::npos && sign != 0) {
                jobs.push_back({one.substr(0, sign), std::atoi(one.c_str() + sign + 1)});
            } else if (!one.empty()) {
                std::printf("hen: file-noise: \"%s\" is not KIND@SECONDS: left out\n", one.c_str());
            }
            at = end + 1;
        }
        if (!jobs.empty()) {
            start_ms = sw_time_ms();
            thread = std::thread([this] { Run(); });
        }
    }
    ~FileNoise() {
        stop.store(true);
        if (thread.joinable()) {
            thread.join();
        }
    }
    FileNoise(const FileNoise&) = delete;
    FileNoise& operator=(const FileNoise&) = delete;

private:
    struct Job {
        std::string kind;
        int at_s;
    };
    struct Sample {
        unsigned long long count, ns, over_100us, over_1ms;
    };
    static Sample Take() {
        unsigned long long pub[7];
        hen_jit_publish_stats(pub);
        return {pub[3] + pub[4] + pub[5] + pub[6], pub[2], pub[5] + pub[6], pub[6]};
    }
    bool WaitUntil(uint64_t ms) const {
        while (sw_time_ms() < ms) {
            if (stop.load()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return !stop.load();
    }
    static void Line(const char* when, const Sample& a, const Sample& b) {
        const unsigned long long count = b.count - a.count;
        std::printf("hen: file-noise:   jit publishes %s: %llu, %.1f us in the request each, %llu over 100 us, "
                    "%llu over 1 ms\n",
                    when, count, count != 0 ? (b.ns - a.ns) / 1000.0 / count : 0.0, b.over_100us - a.over_100us,
                    b.over_1ms - a.over_1ms);
    }
    void Run() const {
        constexpr uint64_t Length = 3000;
        for (const Job& job : jobs) {
            const uint64_t at = start_ms + static_cast<uint64_t>(std::max(job.at_s, 0)) * 1000;
            const int kind = job.kind == "read" ? 0 : job.kind == "aligned" ? 1 : job.kind == "stat" ? 2
                             : job.kind == "malloc" ? 3 : -1;
            if (kind < 0) {
                std::printf("hen: file-noise: no kind \"%s\" (read, aligned, stat, malloc): left out\n",
                            job.kind.c_str());
                continue;
            }
            if (!WaitUntil(at >= Length ? at - Length : 0)) {
                return;
            }
            const Sample before = Take();
            if (!WaitUntil(at)) {
                return;
            }
            const Sample began = Take();
            // (the read kinds: a block of a page and a bit with the reads 24 bytes in, or one
            // page on a page boundary)
            u8* const block = kind == 0 ? static_cast<u8*>(std::malloc(4096 + 64))
                              : kind == 1 ? static_cast<u8*>(memalign(4096, 4096)) : nullptr;
            u8* const into = kind == 0 && block != nullptr ? block + 24 : block;
            const int fd = kind <= 1 ? open(file.c_str(), O_RDONLY) : -1;
            if (kind <= 1 && (fd < 0 || block == nullptr)) {
                std::printf("hen: file-noise: %s: \"%s\" could not be opened (or no block): left out\n",
                            job.kind.c_str(), file.c_str());
                if (fd >= 0) {
                    close(fd);
                }
                std::free(block);
                continue;
            }
            std::printf("hen: file-noise: %s: started %.1f s into the title, for %llu ms%s\n", job.kind.c_str(),
                        (sw_time_ms() - start_ms) / 1000.0, static_cast<unsigned long long>(Length),
                        kind == 0 ? " (reads into a heap block off the page boundary)"
                        : kind == 1 ? " (reads into one page on a page boundary)" : "");
            unsigned long long calls = 0, failed = 0;
            uint64_t read_so_far = 0;
            const uint64_t began_ms = sw_time_ms();
            while (!stop.load(std::memory_order_relaxed) && sw_time_ms() - began_ms < Length) {
                for (int n = 0; n < 16; n++, calls++) {
                    if (kind <= 1) {
                        const ssize_t got = read(fd, into, 4096);
                        read_so_far += got > 0 ? static_cast<uint64_t>(got) : 0;
                        if (got < 4096 || read_so_far >= (64u << 20)) {
                            failed += got < 0;
                            lseek(fd, 0, SEEK_SET);
                            read_so_far = 0;
                        }
                    } else if (kind == 2) {
                        struct stat st;
                        failed += stat(file.c_str(), &st) != 0;
                    } else {
                        void* blocks[8];
                        static constexpr std::size_t sizes[8] = {24, 200, 1500, 4000, 70000, 96, 9000, 640};
                        for (int b = 0; b < 8; b++) {
                            blocks[b] = std::malloc(sizes[b]);
                            if (blocks[b] != nullptr) {
                                *static_cast<volatile char*>(blocks[b]) = 1;
                            }
                        }
                        for (int b = 0; b < 8; b++) {
                            std::free(blocks[b]);
                        }
                    }
                }
            }
            const uint64_t took = sw_time_ms() - began_ms;
            const Sample ended = Take();
            if (fd >= 0) {
                close(fd);
            }
            std::free(block);
            std::printf("hen: file-noise: %s: done: %llu calls in %llu ms (%.1f us each), %llu failed\n",
                        job.kind.c_str(), calls, static_cast<unsigned long long>(took),
                        calls != 0 ? took * 1000.0 / calls : 0.0, failed);
            Line("in the 3 s before", before, began);
            Line("meanwhile", began, ended);
        }
    }
    std::string file;
    std::vector<Job> jobs;
    uint64_t start_ms = 0;
    std::atomic<bool> stop{false};
    std::thread thread;
};
#endif // HEN_DEV

/// Every emulated thread: state, where it is, and the objects it waits for.
static void DumpEmulatedThreads(Core::System& system) {
    static const char* const status_names[] = {"running", "ready", "wait-arbiter", "wait-sleep",
                                               "wait-ipc", "wait-any", "wait-all", "wait-hle",
                                               "dormant", "dead"};
    // (to stdout and to hang.txt on the card)
    std::string text = fmt::format(
        "hen: hang: service calls on host threads: {} queued, {} started, {} finished, {} threw, "
        "{} asked for no wake-up; wake-ups from other threads: {} queued, {} taken over; "
        "thread wake-ups fired {} ({} for a thread not waiting)\n"
        "hen: hang: no frames and an idle emulated CPU for 10 s; emulated threads:\n",
        hen_async_queued.load(), hen_async_started.load(), hen_async_finished.load(), hen_async_threw.load(),
        hen_async_negative.load(), hen_ts_pushed.load(), hen_ts_moved.load(), hen_wake_fired.load(),
        hen_wake_not_waiting.load());
    for (u32 core = 0; core < system.GetNumCores(); core++) {
        const auto& manager = system.Kernel().GetThreadManager(core);
        for (const auto& thread : manager.GetThreadList()) {
            const unsigned status = static_cast<unsigned>(thread->status);
            text += fmt::format("hen: hang: core {} thread {} (object {}) {}, priority {}, pc {:08x} lr {:08x}", core,
                                thread->thread_id, thread->GetObjectId(), status < 10 ? status_names[status] : "?",
                                thread->current_priority, thread->context.cpu_registers[15],
                                thread->context.cpu_registers[14]);
            if (thread->status == Kernel::ThreadStatus::WaitArb) {
                text += fmt::format(", address {:08x}", thread->wait_address);
            }
            for (const auto& object : thread->wait_objects) {
                text += fmt::format("; waits for {} \"{}\" (object {})", object->GetTypeName(), object->GetName(),
                                    object->GetObjectId());
            }
            text += '\n';
        }
    }
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
    sw_hang_record(text.c_str());
}

// Crashes leave no report on this runtime, and an uncaught C++ exception (Vulkan-Hpp throws on
// errors) would end the same way: say what it was first.
// ---- A failed assertion of the emulator, on the thread that runs the emulated console ----------
// The emulator ends the program on the spot when one of its internal checks fails (a trap).
// Here that leaves threads unjoined and can keep the next program from starting. So while the
// run loop runs, a failed check on this thread jumps back out of it instead (common/assert.h):
// the title is marked as failed and shut down like any other, with a time limit on everything
// from there to the program's end, since the emulator was left in the middle of something and
// may wait on it for ever. The first thread watches the limit and ends the program if it
// passes. After such a title the app closes: nothing more is run on that emulator.
// Checks that fail on other threads (the renderer's, the sound's) end the program on the spot.
pthread_t assert_thread;
std::atomic<bool> assert_armed{false};
jmp_buf assert_jump;
std::atomic<uint64_t> exit_deadline_ms{0}; ///< sw_time_ms by which the program has to have ended; 0: none

// Both sticks, whatever the title does. The run loop sees the two stick buttons only in the
// input state that is read when the emulated console shows a frame, and only between two
// passes of the emulator's own loop. A title that stops showing frames, or keeps the emulated
// CPU in one long stretch (the error applet walking unmapped memory: no frame and no pass for
// a minute), could not be left that way. So the program's first thread, which sleeps 100 ms at
// a time beside the emulator, reads the pad itself:
// - both sticks held for 200 ms while a title from the title list runs: combo_request is set,
//   and the emulated cores are asked to leave generated code (System::HenKick) every 100 ms
//   until the run loop has taken the request;
// - not taken within 8 s (the emulator thread is stuck outside generated code): the program
//   ends without that thread, as after a failed assertion. Kaizhar closes; the console does
//   not have to be restarted.
std::atomic<bool> combo_armed{false};   ///< a title from the title list is in its run loop
std::atomic<bool> combo_request{false}; ///< both sticks were seen by the first thread

void OnAssertion() {
    if (!assert_armed.exchange(false) || !pthread_equal(pthread_self(), assert_thread)) {
        return;
    }
    std::printf("hen: assertion: an internal check of the emulator failed on the emulation thread: ending the "
                "title\n");
    std::fflush(stdout);
    longjmp(assert_jump, 1);
}

/// One pass of the emulator's run loop. asserted: an internal check failed in it.
[[gnu::noinline]] Core::System::ResultStatus RunLoopGuarded(Core::System& system, bool& asserted) {
    if (setjmp(assert_jump) != 0) {
        asserted = true;
        return Core::System::ResultStatus::Success;
    }
    assert_armed = true;
    const auto status = system.RunLoop();
    assert_armed = false;
    return status;
}

#if HEN_DEV
/// How many emulated threads of an application (not of a system module) are asleep inside a
/// service call with a wake-up callback: a request in flight (--exit-mid-call).
unsigned ThreadsInsideServiceCall(Core::System& system) {
    unsigned inside = 0;
    for (u32 core = 0; core < system.GetNumCores(); core++) {
        for (const auto& thread : system.Kernel().GetThreadManager(core).GetThreadList()) {
            if (!thread || thread->status != Kernel::ThreadStatus::WaitHleEvent || !thread->wakeup_callback) {
                continue;
            }
            const auto process = thread->owner_process.lock();
            if (process && process->codeset && static_cast<u32>(process->codeset->program_id >> 32) != 0x00040130) {
                inside++;
            }
        }
    }
    return inside;
}

void ReportEmulatedThreads(Core::System& system) {
    static const char* const states[] = {"running",          "ready",           "waiting on an address arbiter",
                                         "sleeping",         "waiting for a reply", "waiting for any of",
                                         "waiting for all of", "paused by a service", "not started", "ended"};
    // The threads by process, each process under a line of its own.
    struct Shown {
        u32 core;
        std::shared_ptr<Kernel::Thread> thread;
    };
    struct ProcessThreads {
        std::string name = "?";
        std::vector<Shown> threads;
    };
    std::map<u32, ProcessThreads> processes;
    for (u32 core = 0; core < system.GetNumCores(); core++) {
        for (const auto& thread : system.Kernel().GetThreadManager(core).GetThreadList()) {
            if (!thread || thread->status == Kernel::ThreadStatus::Dead) {
                continue;
            }
            const auto process = thread->owner_process.lock();
            ProcessThreads& entry = processes[process ? process->process_id : 0u];
            if (process && process->codeset) {
                entry.name = process->codeset->GetName();
            }
            entry.threads.push_back({core, thread});
        }
    }
    const u64 now_us = static_cast<u64>(system.CoreTiming().GetGlobalTimeUs().count());
    std::printf("hen: threads: %zu process(es) at %.3f s of emulated time\n", processes.size(), now_us / 1e6);
    unsigned shown = 0;
    for (const auto& [process_id, entry] : processes) {
        std::printf("hen: threads: process %u %s: %zu thread(s)\n", process_id, entry.name.c_str(),
                    entry.threads.size());
        for (const auto& [core, thread] : entry.threads) {
            if (shown++ >= 300) {
                continue; // (the lines of the processes still come)
            }
            std::string waits;
            for (const auto& object : thread->wait_objects) {
                if (object) {
                    waits += " [" + object->GetTypeName() + " " + object->GetName();
                    // Who last signalled or released it, if it is a kind that is followed
                    // (hen_ipc_trace.h: events, and mutexes and semaphores released by a thread).
                    const std::string type = object->GetTypeName();
                    if (type == "Event" || type == "Mutex" || type == "Semaphore") {
                        if (const Kernel::HenIpc::Signal* signal = Kernel::HenIpc::FindSignal(object->GetObjectId())) {
                            waits += fmt::format(", signalled {} time(s), last by {} at {:.3f} s", signal->count,
                                                 signal->thread_id != 0 ? fmt::format("thread {}", signal->thread_id)
                                                                        : std::string("the emulator"),
                                                 signal->at_us / 1e6);
                        } else {
                            waits += ", never signalled";
                        }
                    }
                    waits += "]";
                }
            }
            if (thread->status == Kernel::ThreadStatus::WaitArb) {
                waits += fmt::format(" at address {:08x}", thread->wait_address);
                const auto owner = thread->owner_process.lock();
                if (owner && system.Memory().IsValidVirtualAddress(*owner, thread->wait_address) &&
                    system.Memory().IsValidVirtualAddress(*owner, thread->wait_address + 3)) {
                    u32 value = 0;
                    system.Memory().ReadBlock(*owner, thread->wait_address, &value, sizeof(value));
                    waits += fmt::format(" (holds {:08x})", value);
                } else {
                    waits += " (not readable)";
                }
            }
            // What it last asked a service: a title that keeps running and shows nothing is
            // named by this, not by what it waits on.
            std::string asked = "no request yet";
            if (const Kernel::HenIpc::Last* last = Kernel::HenIpc::Find(thread->thread_id)) {
                asked = fmt::format("last request {} command {:08x} at {:.3f} s ({:.3f} s ago), {}",
                                    last->session, last->header, last->sent_us / 1e6,
                                    (now_us - last->sent_us) / 1e6,
                                    last->answered ? "answered" : "no answer yet");
            }
            const unsigned state = static_cast<unsigned>(thread->status);
            std::printf("hen: threads: core %u process %u %s, thread %u: %s%s; pc %08x lr %08x; %s\n", core,
                        process_id, entry.name.c_str(), thread->thread_id,
                        state < std::size(states) ? states[state] : "?", waits.c_str(),
                        thread->context.cpu_registers[15], thread->context.cpu_registers[14], asked.c_str());
        }
    }
    if (shown > 300) {
        std::printf("hen: threads: (%u more threads not shown)\n", shown - 300);
    }

    {
        u64 printed = 0, left_out = 0;
        Kernel::HenIpc::RequestLineStats(printed, left_out);
        std::printf("hen: req: %llu line(s) printed; %llu request(s) of applications left out (past the first %u of "
                    "their kind, or after %u lines)\n",
                    static_cast<unsigned long long>(printed), static_cast<unsigned long long>(left_out),
                    Kernel::HenIpc::REQ_PER_KIND, Kernel::HenIpc::REQ_LINES);
    }

    // The requests of the last five seconds of emulated time, most frequent first: a loop that
    // keeps asking shows as a count.
    constexpr u64 window_us = 5'000'000;
    const u64 since_us = now_us > window_us ? now_us - window_us : 0;
    u64 total = 0, kept_from_us = 0;
    const std::vector<Kernel::HenIpc::Count> counts = Kernel::HenIpc::Recent(since_us, total, kept_from_us);
    std::printf("hen: ipc: %llu request(s) to services in the last %.3f s of emulated time, %zu kind(s)%s\n",
                static_cast<unsigned long long>(total), (now_us - kept_from_us) / 1e6, counts.size(),
                kept_from_us > since_us ? " (older ones were not kept)" : "");
    for (std::size_t i = 0; i < counts.size() && i < 10; i++) {
        const auto found = processes.find(counts[i].process_id);
        std::printf("hen: ipc: process %u %s, session %s, command %08x: %u request(s)\n", counts[i].process_id,
                    found != processes.end() ? found->second.name.c_str() : "?", counts[i].session.c_str(),
                    counts[i].header, counts[i].count);
    }
}
#endif

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
    // --sequence SECS[+CLICKS][@ROUTE]:TEXT,...: the title list picks the titles by itself
    // (TitleMenu::SetScript), each runs SECS seconds and goes back to the list as both sticks would;
    // CLICKS changes of the arrangement on the way (one every 6 s from the 10th); ROUTE: a recorded
    // route played meanwhile (as --movie-play). For what only a session of several titles shows.
    // SECS...!ID: the title with that ID starts without its shader files (they are renamed to
    // *.aside in the shader folder; the driver's pipeline cache file of the title stays).
    struct SequenceStep {
        int secs, clicks;
        std::string route, cold_id;
    };
    std::vector<SequenceStep> sequence;
    size_t sequence_at = 0;
    std::string file_noise; // --file-noise (FileNoise)
    // --exit-mid-call, a test of shutting down with a request in flight: when the timed exit or
    // the both-sticks exit is due, the title runs on (5 s at most) until, between two passes of
    // the run loop, a thread of the application is asleep inside a service call; it ends there.
    // Nothing of the emulated console runs between that look and System::Shutdown.
    bool exit_mid_call = false;
    // --movie-record NAME / --movie-play NAME: the emulator's input movies (core/movie.cpp), kept
    // in /switch/Kaizhar/movies/NAME.ctm. Inputs are stored per input poll of the emulated
    // console, so a replay follows the same route whatever the host's speed; the clock starts
    // from the movie's time. A replay needs the save data the recording started from.
    std::string movie_record, movie_play;
    // --movie-sync N: the route waits until the title asks for the pad, then goes on from the
    // recording's pad input N (core/movie.h; N is what `hen: movie: the title asked for the pad at
    // pad input N` printed in a playback under the recording's own conditions). -1: as recorded.
    long long movie_sync = -1;
    bool clock_info = false;   // --clock-info: CPU/GPU clock rates and temperature in the log
    int mic = 1;             // --mic N: 0 none, 1 the system's microphone, 2 the emulator's static noise
    bool mic_probe = false;  // --mic-probe: what the audio input service answers and records
    int repeat_left = 0;     // --repeat N
    bool shot_by_frame = false;
    std::vector<int> shot_times; // --screenshot-at S[,S...]: pictures sent over the log
#endif
    HEN_TEST_SWITCH bool keep_awake = false; // --keep-awake
    int layout_override = -1;  // --layout N (0..3: title_menu.cpp's list)
    bool software_renderer = false;
    bool debug_log = false;
    bool interpreter = false;
    u64 title_id_arg = 0; // --title <id>
    const char* log_filter = nullptr; // --log "<filter>", e.g. "*:Info Service:Debug"
    const char* room = nullptr; // --room nick[:password]@address[:port]
    const char* host_room = nullptr; // --host-room[=port]
    bool online_lle = false;         // --online-lle
    bool no_online_lle = false;      // --no-online-lle: off for this start, whatever multiplayer.txt says
#if HEN_DEV
    bool link_fresh = false; // --link-fresh: link_3ds.h, Fresh
    const char* https_check = nullptr; // --https-check[=url]
#endif
    bool no_audio = false;
    HEN_TEST_SWITCH bool jit_list = true; // compile ahead from the list of blocks earlier sessions used
    // Internal resolution: 4x in every arrangement (3x can be told from it by eye, also on the
    // stacked arrangement). --res N: 1..10, or 0 for the smallest whole multiple of 400x240 that
    // covers the arrangement's top screen: stacked 3, large top 4. The automatic step (below)
    // still goes down from it.
    int resolution = 4;
    HEN_TEST_SWITCH bool fresh_jit_list = false; // start from an empty list (a first session, for measuring)
    const int jit_start_ms = 4000; // at most; the whole list usually takes less
#if HEN_DEV
    bool cold_shaders = false; // an empty shader cache, in its own directory
#endif
    HEN_TEST_SWITCH bool cold_driver = false, shared_driver_cache = false; // (cache_clear.h)
    HEN_TEST_SWITCH bool async_shaders = true; // skip draws whose pipeline is still compiling
#if HEN_DEV
    int power_log = 0; // --power-log [SECONDS]: the period, 0 = off
    const bool core_log = true; // the host cores in the log
#endif
    // The driver compresses render targets of 2 MiB or more and gives each its own allocation
    // for it. Off here until it pays: it lowers GPU time (Xenoblade, large top at 4x: 10 to 28 %
    // per report) but no frame comes sooner for it, and in the stacked arrangement it costs gaps
    // between frames on Pokemon Omega Ruby and lag on Smash.
    // --driver-compression turns it on, for comparing. The driver reads the variable as the
    // Vulkan instance is made; an older driver knows the word and has nothing to turn off.
    setenv("NVK_DEBUG", "no_compression", 1);
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) {
            software_renderer = std::strcmp(argv[++i], "sw") == 0;
        } else if (std::strcmp(argv[i], "--debug-log") == 0) {
            debug_log = true;
        } else if (std::strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            // An installed title by its ID (its path has spaces, which arguments sent with
            // the program over the network don't keep).
            // (looked up below, once the user folder is known)
            title_id_arg = std::strtoull(argv[++i], nullptr, 16);
        } else if (std::strcmp(argv[i], "--layout") == 0 && i + 1 < argc) {
            layout_override = std::atoi(argv[++i]); // (test runs: not the saved layout)
        } else if (std::strcmp(argv[i], "--room") == 0 && i + 1 < argc) {
            room = argv[++i];
        } else if (std::strcmp(argv[i], "--host-room") == 0) {
            host_room = "";
        } else if (std::strncmp(argv[i], "--host-room=", 12) == 0) {
            host_room = argv[i] + 12;
        } else if (std::strcmp(argv[i], "--no-online-lle") == 0) {
            no_online_lle = true;
        } else if (std::strcmp(argv[i], "--online-lle") == 0) {
            online_lle = true;
        } else if (std::strcmp(argv[i], "--interpreter") == 0) {
            interpreter = true; // for telling a JIT bug from anything else
        } else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_filter = argv[++i];
        } else if (std::strcmp(argv[i], "--no-audio") == 0) {
            no_audio = true;
        } else if (std::strcmp(argv[i], "--res") == 0 && i + 1 < argc) {
            resolution = std::atoi(argv[++i]);
#if HEN_DEV
        // (everything from here to #endif: test runs and comparisons, development builds only)
        } else if (std::strcmp(argv[i], "--no-depth-scale-blit") == 0) {
            hen_depth_scale_blit = 0;
        } else if (std::strcmp(argv[i], "--no-early-depth") == 0) {
            hen_early_depth = 0;
        } else if (std::strcmp(argv[i], "--no-generic-early-depth") == 0) {
            hen_generic_early_depth = 0;
        } else if (std::strcmp(argv[i], "--generic-shading-rate") == 0 && i + 1 < argc) {
            hen_generic_shading_rate = std::clamp(std::atoi(argv[++i]), 1, 4);
        } else if (std::strcmp(argv[i], "--recycle-surfaces") == 0) {
            hen_recycle_surfaces = 1;
        } else if (std::strcmp(argv[i], "--recycle-cap-mib") == 0 && i + 1 < argc) {
            hen_recycle_cap_mib = static_cast<unsigned>(std::atoi(argv[++i])); // for comparing
        } else if (std::strcmp(argv[i], "--recycle-age") == 0 && i + 1 < argc) {
            hen_recycle_age_frames = static_cast<unsigned>(std::atoi(argv[++i])); // for comparing
        } else if (std::strcmp(argv[i], "--whole-pipelines") == 0) {
            hen_pipeline_libraries = 0; // for comparing
        } else if (std::strcmp(argv[i], "--cia-chunk-kib") == 0 && i + 1 < argc) {
            hen_cia_chunk_bytes = static_cast<unsigned>(std::atoi(argv[++i])) * 1024; // for comparing
        } else if (std::strcmp(argv[i], "--translation-worker") == 0 && i + 1 < argc) {
            // (hen/jit/hen_jit.c) 0: no second thread compiles ahead. It takes no other value.
            if (std::atoi(argv[++i]) == 0) {
                hen_translation_worker = 0;
            } else {
                std::printf("hen: --translation-worker takes 0 only; got %s: the default kept\n", argv[i]);
            }
        } else if (std::strcmp(argv[i], "--jit-worker-depth") == 0 && i + 1 < argc) {
            hen_worker_depth_limit = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--jit-ahead") == 0 && i + 1 < argc) {
            hen_emu_ahead = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--max-lag-ms") == 0 && i + 1 < argc) {
            hen_max_lag_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--cpu-shade-share") == 0 && i + 1 < argc) {
            hen_cpu_shade_share = std::atoi(argv[++i]);
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
        } else if (std::strcmp(argv[i], "--no-load-boost") == 0) {
            hen_load_boost = 0;
        } else if (std::strcmp(argv[i], "--no-fs-canon") == 0) {
            hen_fs_canon = 0;
        } else if (std::strcmp(argv[i], "--no-generic-seed") == 0) {
            hen_generic_seed = 0;
        } else if (std::strcmp(argv[i], "--no-jit-publish-early") == 0) {
            hen_publish_early = 0;
        } else if (std::strcmp(argv[i], "--no-publish-aligned") == 0) {
            hen_publish_aligned = 0; // (the library's publish call, two heap blocks a request, to compare)
        } else if (std::strcmp(argv[i], "--no-publish-batch") == 0) {
            hen_publish_batch = 0; // (the compile worker's batches of eight blocks at most, to compare)
        } else if (std::strcmp(argv[i], "--no-pointer") == 0) {
            hen_pointer = 0;
        } else if (std::strcmp(argv[i], "--headroom-fence") == 0) {
            hen_headroom_render = 0; // (the present fence wait decides, not the render time)
        } else if (std::strcmp(argv[i], "--headroom-ms") == 0 && i + 1 < argc) {
            double hi = 0, lo = 0; // (the render time's thresholds, ms: none from HI, again from LO)
            if (std::sscanf(argv[++i], "%lf,%lf", &hi, &lo) == 2 && hi > lo && lo > 0) {
                hen_headroom_hi_us = static_cast<int>(hi * 1000);
                hen_headroom_lo_us = static_cast<int>(lo * 1000);
            } else {
                std::printf("hen: headroom: --headroom-ms wants HI,LO with HI above LO; got %s: 9,7 kept\n", argv[i]);
            }
        } else if (std::strcmp(argv[i], "--no-auto-resolution-step") == 0) {
            resolution_step = false;
        } else if (std::strcmp(argv[i], "--resolution-step-at") == 0 && i + 1 < argc) {
            resolution_step_at_s = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--click-at") == 0 && i + 1 < argc) {
            click_at_s = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-display-120") == 0) {
            sw_display_request_none(); // (no display rate asked for: the system's default)
        } else if (std::strcmp(argv[i], "--power-log") == 0) {
            // [SECONDS]: a number after it is the period (switch_platform.c)
            power_log = 30;
            if (i + 1 < argc && argv[i + 1][0] != 0 &&
                argv[i + 1][std::strspn(argv[i + 1], "0123456789")] == 0) {
                power_log = std::max(1, std::atoi(argv[++i]));
            }
        } else if (std::strcmp(argv[i], "--no-stick-pointer") == 0) {
            hen_stick_pointer = 0;
        } else if (std::strcmp(argv[i], "--no-generic-fs") == 0) {
            hen_generic_fs = 0;
        } else if (std::strcmp(argv[i], "--stale-profile") == 0) {
            hen_stale_profile = 1;
        } else if (std::strcmp(argv[i], "--cache-regen-sync") == 0) {
            hen_cache_regen_sync = 1; // (stale shader caches regenerated before the start)
        } else if (std::strcmp(argv[i], "--driver-compression") == 0) {
            unsetenv("NVK_DEBUG"); // (compressed render targets, the driver's default)
            std::printf("hen: driver: compression on (--driver-compression)\n");
        } else if (std::strcmp(argv[i], "--no-vma-small-blocks") == 0) {
            hen_vma_small_blocks = 0; // (image memory in 32 to 256 MiB blocks, chunked by the driver)
        } else if (std::strcmp(argv[i], "--compile-workers") == 0 && i + 1 < argc) {
            hen_compile_workers = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--nir-timing") == 0) {
            // (the driver reads the variable at its first shader compile and appends a report
            // to that "file" after each one: switch_file_defer.c turns it into log lines)
            setenv("NIR_PASS_TIMING", "hen:nir-timing", 1);
        } else if (std::strcmp(argv[i], "--nvk-debug") == 0 && i + 1 < argc) {
            // (the driver reads the variable with getenv as the Vulkan instance is made, which
            // is when a title starts)
            setenv("NVK_DEBUG", argv[++i], 1);
            std::printf("hen: driver: NVK_DEBUG=%s\n", argv[i]);
        } else if (std::strcmp(argv[i], "--sw-standin-area") == 0 && i + 1 < argc) {
            hen_sw_standin_area = std::atoi(argv[++i]); // (percent of the viewport; 0: no exception)
        } else if (std::strcmp(argv[i], "--no-sw-standin") == 0) {
            hen_sw_standin = 0; // (small and never-accelerated draws compile on the GPU thread)
        } else if (std::strcmp(argv[i], "--no-etc1-workers") == 0) {
            hen_etc1_workers = 0; // (ETC1 textures decoded by the GPU thread alone)
        } else if (std::strcmp(argv[i], "--vs-jit-async") == 0) {
            hen_vs_jit_async = 1; // (off by default: interpreted meanwhile, not bit-identical)
        } else if (std::strcmp(argv[i], "--vs-jit-small") == 0 && i + 1 < argc) {
            hen_vs_jit_small = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--generic-all") == 0) {
            hen_generic_fs = 2;
        } else if (std::strcmp(argv[i], "--sw-shaders") == 0) {
            // (every draw's vertices shaded on the CPU: slow; with --generic-all, every draw
            // the generic fragment shader supports goes through it)
            hen_sw_shaders = true;
        } else if (std::strcmp(argv[i], "--am-scan-eager") == 0) {
            // (a test: the title manager scans tickets and installed titles all at start, as
            // upstream; core/hle/service/am/am.cpp)
            hen_am_scan_eager = 1;
        } else if (std::strcmp(argv[i], "--no-driver-cache-keep") == 0) {
            // (a test: the driver pipeline cache saved and loaded at every change of GPU right)
            hen_driver_cache_keep = 0;
        } else if (std::strcmp(argv[i], "--no-jit-teardown-thread") == 0) {
            // (a test: an ended process's JIT destroyed on the emulation thread)
            hen_jit_teardown_thread = 0;
        } else if (std::strcmp(argv[i], "--no-launch-cache") == 0) {
            // (a test: a title started again in the same run reads its path, code, icon and
            // optional files from the card again)
            hen_launch_cache = 0;
        } else if (std::strcmp(argv[i], "--no-jit-park") == 0) {
            // (a test: the JIT of an ended applet is destroyed, and the next start of the same
            // applet translates its code again)
            hen_jit_park = 0;
        } else if (std::strcmp(argv[i], "--no-n3ds-mode-keep") == 0) {
            // (a test: an applet started by a New 3DS title turns the 804 MHz mode off)
            hen_n3ds_mode_keep = 0;
        } else if (std::strcmp(argv[i], "--no-am-title-list") == 0) {
            // (a test: no saved list of installed titles, and no reading of it at start with the
            // online modules on)
            hen_am_title_list = 0;
        } else if (std::strcmp(argv[i], "--boost-linger") == 0 && i + 1 < argc) {
            hen_boost_linger_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--cpu-boost") == 0 && i + 1 < argc) {
            hen_load_boost = 0; // (the manual experiment: no automatic switching on top of it)
            sw_cpu_boost(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-direct-links") == 0) {
            hen_direct_links = 0;
        } else if (std::strcmp(argv[i], "--present-cpu-acquire") == 0) {
            hen_present_cpu_acquire = 1; // (off by default: nothing in the GPU's queue waits for the display)
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
        } else if (std::strcmp(argv[i], "--n3ds-clock-high") == 0) {
            // (off by default: a title in the New 3DS fast mode goes from 1523.2 to 1702.4 MHz
            // while it is short of CPU; hen/jit/hen_jit.c, boost_tier)
            hen_n3ds_clock_high = 1;
        } else if (std::strcmp(argv[i], "--n3ds-clock") == 0) {
            hen_n3ds_clock = 1; // (for comparing: 804 MHz whatever the host has to spare)
        } else if (std::strcmp(argv[i], "--n3ds-clock-off") == 0) {
            hen_n3ds_clock = 0; // (for comparing: charged as at 268 MHz, upstream's)
        } else if (std::strcmp(argv[i], "--gpu-thread-sync-writes") == 0) {
            hen_gpu_thread = 1;
            hen_gpu_defer_writes = 0;
        } else if (std::strcmp(argv[i], "--sync-depth-reads") == 0) {
            hen_depth_lag = 0;
        } else if (std::strcmp(argv[i], "--no-gpu-thread") == 0) {
            hen_gpu_thread = 0;
        } else if (std::strcmp(argv[i], "--jit-bump") == 0 && i + 1 < argc) {
            hen_bump = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--keep-awake") == 0) {
            keep_awake = true;
        } else if (std::strcmp(argv[i], "--no-playing-game") == 0) {
            hen_playing_game = 0; // (A/B: the friends module is not told the title)
        } else if (std::strcmp(argv[i], "--link-fresh") == 0) {
            link_fresh = true;
        } else if (std::strcmp(argv[i], "--trace-sockets") == 0) {
            hen_trace_sockets = 1; // ("hen: soc:" lines: the emulated console's socket calls)
        } else if (std::strcmp(argv[i], "--no-soc-rules") == 0) {
            hen_soc_rules = 0; // (A/B: name lookups not through the URL rules)
        } else if (std::strcmp(argv[i], "--trace-modules") == 0) {
            hen_trace_modules = 1; // ("hen: mod:" lines: what the system modules ask the services)
        } else if (std::strcmp(argv[i], "--assert-test") == 0 && i + 1 < argc) {
            hen_assert_test_frame = std::atoi(argv[++i]); // (a failed check at that frame of the title)
        } else if (std::strcmp(argv[i], "--https-check") == 0) {
            https_check = "";
        } else if (std::strncmp(argv[i], "--https-check=", 14) == 0) {
            https_check = argv[i] + 14;
        } else if (std::strcmp(argv[i], "--room-plain-io") == 0) {
            hen_room_plain_io = 1;
        } else if (std::strcmp(argv[i], "--no-file-defer") == 0) {
            hen_file_defer = 0;
        } else if (std::strcmp(argv[i], "--no-driver-cache-memory") == 0) {
            hen_driver_cache_memory = 0;
        } else if (std::strcmp(argv[i], "--no-jit-list") == 0) {
            jit_list = false;
        } else if (std::strcmp(argv[i], "--no-fast-dispatch") == 0) {
            hen_fast_dispatch = 0;
        } else if (std::strcmp(argv[i], "--no-jit-speculation") == 0) {
            Core::HenBlockList::SetSpeculation(false);
        } else if (std::strcmp(argv[i], "--fresh-jit-list") == 0) {
            fresh_jit_list = true;
        } else if (std::strcmp(argv[i], "--no-jit-resort") == 0) {
            // The code stays where it was compiled: to compare.
            hen_resort = 0;
        } else if (std::strcmp(argv[i], "--jit-resort-at") == 0 && i + 1 < argc) {
            // (a test's trigger) One re-sort that many seconds after the title's start,
            // whatever the rules say, and no other.
            const double seconds = std::atof(argv[++i]);
            hen_resort_at_ms = seconds > 0 ? static_cast<int>(seconds * 1000.0) : 0;
        } else if (std::strcmp(argv[i], "--jit-resort-keep") == 0) {
            hen_resort_release = 0; // (the old buffer is kept for the next re-sort)
        } else if (std::strcmp(argv[i], "--no-jit-lazy-write") == 0) {
            hen_jit_lazy_write = 0; // (every JIT takes its whole write area when it is made)
        } else if (std::strcmp(argv[i], "--jit-buffer-mib") == 0 && i + 1 < argc) {
            // (124 by default; 8 to 128, an even number. With --jit-buffer-grow a small one makes
            // the growth path run: 28 -> 60 -> 92 -> 124. Under 28 is not a size the sizing rule
            // ever picks: it is for tests of a full buffer on a light route.)
            const int mib = std::atoi(argv[++i]) & ~1;
            hen_code_cache_mib = mib < 8 ? 8 : mib > 128 ? 128 : mib;
        } else if (std::strcmp(argv[i], "--jit-clear-at") == 0 && i + 1 < argc) {
            // (a test's trigger) Every JIT with code clears its cache at its first slice
            // that many seconds after the title's start, once.
            const double seconds = std::atof(argv[++i]);
            hen_jit_clear_at_ms = seconds > 0 ? static_cast<int>(seconds * 1000.0) : 0;
        } else if (std::strcmp(argv[i], "--jit-buffer-grow") == 0) {
            hen_buffer_grow = 1; // (a buffer that fills is re-sorted into a larger one)
        } else if (std::strcmp(argv[i], "--no-jit-buffer-grow") == 0) {
            hen_buffer_grow = 0; // (sized, but a buffer that fills is cleared: a test's setting)
        } else if (std::strcmp(argv[i], "--jit-buffer-sized") == 0) {
            hen_buffer_sized = 1; // (a JIT's buffer is sized from its process's code)
            hen_buffer_grow = 1;
        } else if (std::strcmp(argv[i], "--jit-resort-chunk-kib") == 0 && i + 1 < argc) {
            // (to compare) KiB of a copy in one publish request: 4 to 1024 (default 256).
            const int kib = std::atoi(argv[++i]);
            hen_resort_chunk = (kib < 4 ? 4 : kib > 1024 ? 1024 : kib) << 10;
        } else if (std::strcmp(argv[i], "--jit-resort-poison") == 0) {
            // The old code is overwritten after a change-over.
            hen_resort_poison = 1;
        } else if (std::strcmp(argv[i], "--cold-shaders") == 0) {
            cold_shaders = true;
        } else if (std::strcmp(argv[i], "--sync-shaders") == 0) {
            async_shaders = false;
        } else if (std::strcmp(argv[i], "--hen-exit-after") == 0 && i + 1 < argc) {
            exit_after_s = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--sequence") == 0 && i + 1 < argc) {
            std::vector<std::string> texts;
            std::string all = argv[++i];
            for (size_t at = 0; at < all.size();) {
                const size_t end = std::min(all.find(',', at), all.size());
                const std::string item = all.substr(at, end - at);
                at = end + 1;
                const size_t colon = item.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                const size_t plus = item.find('+'), route = item.find('@'), cold = std::min(item.find('!'), colon);
                sequence.push_back({std::atoi(item.c_str()), plus < colon ? std::atoi(item.c_str() + plus + 1) : 0,
                                    route < colon ? item.substr(route + 1, cold - route - 1) : std::string{},
                                    cold < colon ? item.substr(cold + 1, colon - cold - 1) : std::string{}});
                texts.push_back(item.substr(colon + 1));
            }
            TitleMenu::SetScript(texts);
        } else if (std::strcmp(argv[i], "--exit-mid-call") == 0) {
            exit_mid_call = true;
        } else if (std::strcmp(argv[i], "--jit-refuse-test") == 0 && i + 1 < argc) {
            // (a test's trigger: 16 leaves a 92 MiB buffer 16 MiB, which it outgrows soon)
            const int mib = std::atoi(argv[++i]);
            hen_jit_refuse_test = mib < 4 ? 4 : mib > 120 ? 120 : mib;
        } else if (std::strcmp(argv[i], "--file-noise") == 0 && i + 1 < argc) {
            file_noise = argv[++i];
        } else if (std::strcmp(argv[i], "--no-shutdown-fix") == 0) {
            hen_no_shutdown_fix = 1; // (core/core.cpp: requests in flight left as upstream leaves them)
        } else if ((std::strcmp(argv[i], "--screenshot-frame") == 0 || std::strcmp(argv[i], "--screenshot-at") == 0) &&
                   i + 1 < argc) {
            // (--screenshot-frame: as --screenshot-at, but by the program's vertical blank count:
            // the same moment of the program in a run that is slower or faster. Replaces
            // --screenshot-at.)
            if (std::strcmp(argv[i], "--screenshot-frame") == 0) {
                shot_by_frame = true;
                shot_times.clear();
            }
            for (const char* c = argv[++i]; *c != 0;) {
                shot_times.push_back(std::atoi(c));
                while (*c != 0 && *c != ',') {
                    c++;
                }
                c += *c == ',';
            }
        } else if (std::strcmp(argv[i], "--cold-driver") == 0) {
            // The driver's shader cache emptied, for measuring a true first launch: Kaizhar's
            // own folder (citra_switch/cache_clear.cpp), the shared one only with
            // --shared-driver-cache.
            cold_driver = true;
        } else if (std::strcmp(argv[i], "--shared-driver-cache") == 0) {
            shared_driver_cache = true; // (the driver's cache in /config/ouncevk, shared with other homebrew)
        } else if (std::strcmp(argv[i], "--crash-test") == 0) {
            if (i + 1 < argc && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '9') {
                crash_test_s = static_cast<unsigned>(std::atoi(argv[++i]));
            } else {
                std::printf("hen: --crash-test: writing to address 8\n");
                *reinterpret_cast<volatile int*>(8) = 1; // (to see the crash report)
            }
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
        } else if (std::strcmp(argv[i], "--movie-sync") == 0 && i + 1 < argc) {
            movie_sync = std::atoll(argv[++i]);
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
#if HEN_DEV
    if (driver_line_next == nullptr) {
        // (in front of the driver, or of the call tracer when --vk-trace asked for it)
        driver_line_next = hen_vulkan_entry_point != nullptr
                               ? hen_vulkan_entry_point
                               : reinterpret_cast<VulkanEntryPoint>(&vkGetInstanceProcAddr);
        hen_vulkan_entry_point = &DriverLineEntry;
    }
#endif

#if HEN_DEV
    if (core_log) {
        hen_core_sampling_start();
    }
    if (power_log > 0) {
        sw_power_log_request(static_cast<unsigned>(power_log));
    }
#endif

    // Test runs nobody attends (a timed run, a replay, the test library's): development builds only.
#if HEN_DEV
    const auto unattended = [&] {
        return exit_after_s > 0 || ChainPort() > 0 || !movie_play.empty() || !sequence.empty();
    };
#else
    const auto unattended = [] { return false; };
#endif

    FileUtil::SetUserPath("/switch/Kaizhar/");
    // (before the menu or a title makes the driver's device)
    Caches::SetupDriverCache(shared_driver_cache, cold_driver);
    if (title_id_arg != 0) {
        program = Service::AM::GetTitleContentPath(Service::FS::MediaType::SDMC, title_id_arg);
        std::printf("hen: --title %016llX: %s\n", static_cast<unsigned long long>(title_id_arg),
                    program.c_str());
    }
#if HEN_DEV
    if (cold_shaders) {
        const std::string dir = "/switch/Kaizhar/shaders-cold/";
        FileUtil::DeleteDirRecursively(dir);
        FileUtil::CreateFullPath(dir);
        FileUtil::UpdateUserPath(FileUtil::UserPath::ShaderDir, "/switch/Kaizhar/shaders-cold");
        std::printf("hen: shader directory: %s\n",
                    FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir).c_str());
        std::printf("hen: --cold-shaders: empty shader cache in %s\n", dir.c_str());
    }
#endif
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

    // Multiplayer rooms (room_link.h). The client has to exist before the first title loads: the
    // wireless service binds to it when it is created.
    auto& system = Core::System::GetInstance();
    RoomLink::Configure(room, host_room);
    RoomLink::Start(system);
    // Online play on a replacement for Nintendo Network needs five system modules (ACT, BOSS,
    // CECD, FRD, NIM) run as the console's own code from installed system titles, instead of the
    // emulator's stand-ins (core/hle/service/service.cpp). Off unless asked for; nothing else
    // of online play is set up here yet.
    if (no_online_lle) {
        std::printf("hen: online: --no-online-lle: the system modules for online play are the emulator's stand-ins "
                    "for this start%s\n",
                    RoomLink::OnlineLle() ? " (multiplayer.txt has online_lle=1)" : "");
    } else if (online_lle || RoomLink::OnlineLle()) {
        Settings::values.enable_required_online_lle_modules = true;
        std::printf("hen: online: the system modules for online play run as the console's own code (%s)\n",
                    online_lle ? "--online-lle" : "online_lle=1 in multiplayer.txt");
    }
#if HEN_DEV
    // --link-fresh: the card as before the first "Link a 3DS", to try a first link again.
    if (link_fresh) {
        Link3ds::Fresh(system);
        if (!RoomLink::LinkAddress().empty()) {
            RoomLink::SetSetting("link_address", "");
        }
        if (RoomLink::OnlineLle()) {
            RoomLink::SetSetting("online_lle", "0");
        }
        Settings::values.enable_required_online_lle_modules = online_lle;
        std::printf("hen: link: fresh: multiplayer.txt: link_address and online_lle cleared\n");
    }
#endif
    // (the Online play screen's switch writes that line; a launch option holds it on)
    TitleMenu::SetOnlineForced(online_lle);
    // "Link a 3DS": the name servers for the emulated console's lookups during a setup.
    GuestDns::SetServers(RoomLink::DnsServers());
    // (hen/idbe/README.md) The address of the title icon server's name, for that one request.
    hen_idbe_lookup = [](const char* name, unsigned* ipv4) -> int {
        if (name == nullptr || ipv4 == nullptr || Idbe::Host != name) {
            return 0;
        }
        static std::mutex mutex;
        static GuestDns::Answer kept;
        std::scoped_lock lock{mutex};
        if (kept.how == GuestDns::How::None) {
            kept = GuestDns::Resolve(name); // the console's DNS, then the name servers
            std::printf("hen: dns: idbe: %s: the console's DNS says %s; used %s%s%s\n", name,
                        kept.system.c_str(),
                        kept.how == GuestDns::How::None ? "nothing" : kept.text.c_str(),
                        kept.how == GuestDns::How::Own ? " from " : "",
                        kept.how == GuestDns::How::Own ? kept.server.c_str() : "");
        }
        if (kept.how == GuestDns::How::None) {
            return 0;
        }
        *ipv4 = kept.address;
        return 1;
    };
    // ...and the connection its search for the 3DS holds, for the emulator's setup client.
    hen_artic_socket = &ToolScan::Take;
#if HEN_DEV
    if (https_check != nullptr) {
#ifdef KAIZHAR_TLS
        HttpsCheck::Run(https_check);
#else
        std::printf("hen: tls: --https-check: this build has no HTTPS (KAIZHAR_TLS off)\n");
#endif
    }
#endif

    Frontend::RegisterDefaultApplets(system);
    system.RegisterSoftwareKeyboard(std::make_shared<SwitchKeyboard>());
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());

    // Without a program on the command line (opened from the homebrew menu): the title picker,
    // and back to it when a title ends. With one (for testing): that title, then exit.
    const bool picker = program.empty();
    int rc = 0;
    bool setup_again = false; // a setup is run once more: the country had to be set (link_3ds.h)
    bool setup_next = false;  // the Old 3DS set is done: the New 3DS set follows at once
    for (;;) {
        combo_armed = false; // (the title list and what follows: both sticks mean nothing here)
        combo_request = false;
        std::string name = program;
        const bool second_set = setup_next;
        if (setup_again || setup_next) {
            setup_next = false;
            name = "Link a 3DS";
        } else if (picker) {
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
        bool asserted = false; // an internal check of the emulator failed in the run loop
        // "Link a 3DS" (link_3ds.h): the program is the 3DS's own System Settings, sent by the
        // setup tool. Not a game: no room, no input movie, nothing for the cartridge slot.
        const bool setup = Link3ds::IsSetupProgram(program);
        auto last_status = Core::System::ResultStatus::Success;
        std::string last_details;
        uint64_t setup_seen = 0; // when the setup's progress was last looked at
        // The tool on the 3DS serves one client and takes a moment to listen again after one has
        // left (the run before this one, when the country had to be set): unless the search
        // already holds the connection, it is tried for a while here and then handed over
        // like the search's (tool_scan.h). An address with a port is left to the setup's own try.
        if (setup) {
            const std::string where = program.substr(program.find("://") + 3);
            if (where.find(':') == std::string::npos && !ToolScan::Holds(where)) {
                TitleMenu::Busy("Link a 3DS",
                                second_set ? "Set 1 of 2 done. Connecting for set 2..." : "Connecting to the 3DS...",
                                [&where] { ToolScan::Hold(where, 8000); });
            }
        }
#if HEN_DEV
        bool timed_stop = false; // --hen-exit-after ended this title
        SequenceStep step{0, 0, {}, {}}; // --sequence: this title's
        if (picker && !setup_again && !second_set && sequence_at < sequence.size() && name != "Link a 3DS") {
            step = sequence[sequence_at++];
            std::printf("hen: sequence: %d s, %d arrangement change(s), route %s\n", step.secs, step.clicks,
                        step.route.empty() ? "none" : step.route.c_str());
            movie_play = step.route;
            movie_sync = -1;
            if (const size_t slash = movie_play.find('/'); slash != std::string::npos) {
                movie_sync = std::atoll(movie_play.c_str() + slash + 1); // (@ROUTE/N: as --movie-sync N)
                movie_play.resize(slash);
            }
            if (!step.cold_id.empty()) {
                const std::string dir = FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) + "vulkan/transferable/";
                unsigned moved = 0;
                if (DIR* d = opendir(dir.c_str())) {
                    std::vector<std::string> names;
                    while (const dirent* e = readdir(d)) {
                        const std::string file = e->d_name;
                        if (file.rfind(step.cold_id + "_", 0) == 0 && file.find(".aside") == std::string::npos) {
                            names.push_back(file);
                        }
                    }
                    closedir(d);
                    for (const std::string& file : names) {
                        std::remove((dir + file + ".aside").c_str());
                        moved += std::rename((dir + file).c_str(), (dir + file + ".aside").c_str()) == 0;
                    }
                }
                std::printf("hen: sequence: %u shader file(s) of %s set aside in %s\n", moved, step.cold_id.c_str(), dir.c_str());
            }
        }
        unsigned step_clicks_done = 0;
        int mid_exit = 0;        // --exit-mid-call: an exit is due (1 both sticks, 2 timed)
        uint64_t mid_since = 0;
#endif
        std::string failure;
        const char* ended_how = "it ended by itself";
        try {
            EmuWindow_Switch window;
            hen_stick_role = -1; // (RightStickRole says it anew for this title)
            const TitleMenu::StickAction stick_action = TitleMenu::ApplyInputSettings();
            unsigned stick_clicks = sw_stick_clicks();
            bool click_test_due = click_at_s != 0;
            // (the automatic resolution's rule for this title)
            struct {
                unsigned bound = 0, steps = 0; // windows in a row with the GPU as the limit; steps taken
                uint64_t at = 0, quiet_until = 0;
                unsigned long long wait_ns = 0;
                long long slept_us = 0, emulated_us = 0;
                bool forced = resolution_step_at_s != 0;
            } res_rule;
            // What throws while the title loads or runs (the renderer's library throws on errors,
            // and memory can run out) ends the title, not the app: the emulated system is still
            // shut down below, or its threads and its hold on the display would stay.
            bool threw = false;
            try {
            system.ApplySettings();
            std::printf("hen: loading %s\n", program.c_str());
#if HEN_DEV
            std::printf("hen: console: %s as the title starts\n", sw_docked() ? "docked" : "handheld");
#endif
            hen_limiter_slept_us = 0; // (time in the title list is not lag)
#if HEN_DEV
            hen_limiter_given_up_us = 0;
#endif
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
            if (!setup) {
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
            if (setup) {
                movie_record.clear();
                movie_play.clear();
            }
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
            // Before the title loads, so that it finds the room there from its first look (the
            // loader tells the room which title this member runs).
            if (setup) {
                // The emulated console's name lookups, for the time of the setup (guest_dns.h).
                GuestDns::Enable(true);
            } else {
                RoomLink::Join(system);
            }
            const auto load = system.Load(window, program);
            std::printf("hen: load: %s\n", StatusName(load));
            last_status = load;
            last_details = system.GetStatusDetails();
#if HEN_DEV
            if (clock_info) {
                sw_clock_report("after load");
            }
#endif
            // Test runs only (a timed run, a replay, the test library's, or --keep-awake): nobody
            // touches the controller, and the system would dim the screen and go to sleep.
            // A setup too: its download runs for a long time with nobody at the controls.
            const bool keep_awake_now = load == Core::System::ResultStatus::Success &&
                                        (keep_awake || unattended() || setup);
            if (keep_awake_now) {
                sw_keep_awake(true);
            }
#if HEN_DEV
            if (load == Core::System::ResultStatus::Success && !movie_record.empty()) {
                system.Movie().StartRecording(movie_file, "Kaizhar");
                std::printf("hen: movie: recording to %s\n", movie_file.c_str());
                u64 movie_title{};
                system.GetAppLoader().ReadProgramId(movie_title);
                system.Movie().HenSetTitle(movie_title);
            } else if (load == Core::System::ResultStatus::Success && !movie_play.empty()) {
                system.Movie().SetPlaybackCompletionCallback([&system] {
                    std::printf("hen: movie: playback finished after %llu inputs\n",
                                static_cast<unsigned long long>(system.Movie().GetTotalInputCount()));
                });
                system.Movie().StartPlayback(movie_file);
                std::printf("hen: movie: playing %s (%llu inputs)\n", movie_file.c_str(),
                            static_cast<unsigned long long>(system.Movie().GetTotalInputCount()));
                u64 movie_title{};
                system.GetAppLoader().ReadProgramId(movie_title);
                if (movie_sync >= 0) {
                    system.Movie().HenSyncAtTitleStart(movie_title, static_cast<u64>(movie_sync));
                    std::printf("hen: movie: no input until the title asks for the pad, then from pad input %lld\n",
                                movie_sync);
                } else {
                    system.Movie().HenSetTitle(movie_title);
                }
            }
            ReportJit("after load");
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
#if HEN_DEV
                    if (fresh_jit_list) {
                        FileUtil::Delete(fmt::format("/switch/Kaizhar/jit-fresh/{:016X}.blocks",
                                                     program_id));
                    }
#endif
                    Core::HenBlockList::Open(program_id, static_cast<unsigned>(jit_start_ms),
                                             fresh_jit_list ? "jit-fresh/" : "jit/");
#if HEN_DEV
                    ReportJitList();
#endif
                }

                {
                    SessionReport::Title report;
                    system.GetAppLoader().ReadProgramId(report.id);
                    system.GetAppLoader().ReadTitle(report.name);
                    report.resolution = Settings::values.resolution_factor.GetValue();
                    const auto option = Settings::values.layout_option.GetValue();
                    report.layout = option == Settings::LayoutOption::LargeScreen  ? "large top screen"
                                    : option == Settings::LayoutOption::SideScreen ? "side by side"
                                                                                   : "top above bottom";
                    if (Settings::values.upright_screen.GetValue()) {
                        report.layout += ", portrait";
                    }
                    report.jit_list = jit_list;
                    if (jit_list) {
                        const auto listed = Core::HenBlockList::GetStats();
                        report.jit_listed = listed.listed;
                        report.jit_compiled = listed.compiled;
                    }
                    report.shader_dir = FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir);
                    report.blocks_now = hen_blocks_compiled();
                    SessionReport::Begin(report);
                    sw_crash_title(report.id); // (for the crash record)
                }
                const uint64_t start = sw_time_ms();
                uint64_t last_report = start;
#if HEN_DEV
                int threads_told = -1, thread_tables = 0; // ("hen: threads:")
                if (core_log) {
                    // Where the emulated memory is, beside the "hen_jit: buffer" lines
                    // (the code's address, printed at every new code buffer).
                    const u8* const fcram = system.Memory().GetFCRAMPointer(0);
                    const u8* const vram = system.Memory().GetPhysicalPointer(Memory::VRAM_PADDR);
                    const auto& table = system.Memory().GetCurrentPageTable();
                    std::printf("hen: place: emulated memory: fcram %p (%s on a 2 MiB boundary), vram %p, page table "
                                "pointers %p; this thread's stack near %p\n",
                                static_cast<const void*>(fcram),
                                (reinterpret_cast<uintptr_t>(fcram) & 0x1fffff) == 0 ? "" : "not",
                                static_cast<const void*>(vram),
                                table ? static_cast<const void*>(table->GetPointerArray().data()) : nullptr,
                                static_cast<const void*>(&threads_told));
                }
                // The CPU time of every thread, the cores' idle time, the emulation
                // thread's time by part, a speed probe: on from here to the end of this block.
                struct HenCoreLog {
                    explicit HenCoreLog(bool on_) : on{on_} {
                        if (on) {
                            hen_probe_start();
                            hen_core_idle_start();
                            hen_thread_cpu_start();
                            unsigned long long ns[12], count[12], dispatch[5], layout[3];
                            hen_emu_stats(ns, count, dispatch, layout); // (emptied: the title starts here)
                            Core::HenEmuCoresReport(0, false, 0); // (emptied as well)
                            Kernel::HenSvcReport(0, false);
                            hen_emu_stats_on = 1;
                        }
                    }
                    ~HenCoreLog() {
                        if (on) {
                            hen_emu_stats_on = 0;
                            hen_core_idle_stop();
                            hen_probe_stop();
                        }
                    }
                    bool on;
                    unsigned seq = 0;
                    unsigned long long ticks_before = hen_jit_code_ticks_total();
                    unsigned long long interpreted_before = hen_jit_interpreted_ns();
                    long long slept_before = 0;
                } hen_core_log{core_log};
                // (--file-noise: its thread ends, and is joined, when this block is left)
                const FileNoise noise{file_noise, program};
#endif
                sw_watch_enable(true);
                combo_request = false;
#if HEN_DEV
                combo_armed = picker && !exit_mid_call; // (--exit-mid-call has its own way out)
#else
                combo_armed = picker;
#endif
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
                        RoomLink::Step("run loop left: the system asked the program to end");
                        ended_how = "the system closed the program";
                        closing = true;
                        break;
                    }
                    RightStickRole(system);
                    const auto status = RunLoopGuarded(system, asserted);
                    if (asserted) {
                        failure = "it stopped on an internal error";
                        ended_how = "an internal check of the emulator failed";
                        RoomLink::Step("run loop left: a failed assertion");
                        // (from here to the program's end: 10 s, or the first thread ends it)
                        exit_deadline_ms = sw_time_ms() + 10000;
                        break;
                    }
                    if (status != Core::System::ResultStatus::Success) {
                        std::printf("hen: run loop: %s (%s)\n", StatusName(status),
                                    system.GetStatusDetails().c_str());
                        last_status = status;
                        last_details = system.GetStatusDetails();
                        if (status != Core::System::ResultStatus::ShutdownRequested) {
                            failure = StatusName(status);
                        }
                        RoomLink::Step("run loop left: the emulator's status");
                        break;
                    }
#if HEN_DEV
                    // (--exit-mid-call: straight after a pass of the run loop, before anything else)
                    if (mid_exit != 0) {
                        const unsigned inside = ThreadsInsideServiceCall(system);
                        const uint64_t waited = sw_time_ms() - mid_since;
                        if (inside != 0 || waited >= 5000) {
                            if (inside != 0) {
                                std::printf("hen: exit-mid-call: ended with %u thread(s) inside a service call (waited %llu ms)\n",
                                            inside, static_cast<unsigned long long>(waited));
                            } else {
                                std::printf("hen: exit-mid-call: none seen in 5000 ms\n");
                            }
                            if (mid_exit == 1) {
                                RoomLink::Step("run loop left: both sticks");
                                ended_how = "left with both sticks";
                            } else {
                                RoomLink::Step("run loop left: --hen-exit-after");
                                ended_how = "a timed test run";
                                closing = true;
                                timed_stop = true;
                            }
                            break;
                        }
                    } else if (exit_mid_call && picker && EmuWindow_Switch::Input().menu_combo) {
                        std::printf("hen: both sticks pressed: --exit-mid-call: running on until a thread is inside a service call\n");
                        mid_exit = 1;
                        mid_since = sw_time_ms();
                    }
                    if (mid_exit != 0) {
                        // (the exit is due and waits: the ordinary ways out are not taken twice)
                    } else
#endif
                    if (picker && (EmuWindow_Switch::Input().menu_combo || combo_request.load())) {
                        std::printf("hen: both sticks pressed: back to the titles%s\n",
                                    combo_request.load() ? " (seen by the first thread)" : "");
                        RoomLink::Step("run loop left: both sticks");
                        ended_how = "left with both sticks";
                        break;
                    }
                    const uint64_t now = sw_time_ms();
                    if (const unsigned clicks = sw_stick_clicks();
                        clicks != stick_clicks || (click_test_due && now - start >= click_at_s * 1000ull)) {
                        stick_clicks = clicks;
                        click_test_due = false;
                        StickClicked(stick_action, window);
                    }
#if HEN_DEV
                    if (step_clicks_done < static_cast<unsigned>(step.clicks) &&
                        now - start >= 10000ull + 6000ull * step_clicks_done) {
                        step_clicks_done++;
                        StickClicked(TitleMenu::StickAction::NextArrangement, window);
                    }
#endif
                    if (now - (res_rule.at != 0 ? res_rule.at : start) >= 1000) {
                        // The automatic internal resolution follows the GPU. In a one-second
                        // window the GPU is the limit, and speed is being lost, when the title
                        // ran under 93%, the renderer waited for the GPU a quarter of the time
                        // or more, and the limiter slept 2% of it or less. Four such windows in
                        // a row: one factor down. In 1128 five-second reports of healthy runs
                        // (six titles, both display rates, cold and warm, two arrangements)
                        // none is slow and waiting enough to hold such a stretch; Xenoblade at
                        // the large-top arrangement, the case this is for, ran at 70 to 85%
                        // with the renderer waited for 70% of the time. After a step the rule
                        // waits 15 s, twice as long after each further one, and it never goes
                        // up again in a title's run: a step drops every surface the renderer
                        // has.
                        const unsigned long long wait_ns = __atomic_load_n(&hen_render_wait_ns, __ATOMIC_RELAXED);
                        const long long slept_us = hen_limiter_slept_total_us;
                        const long long emulated_us = system.CoreTiming().GetGlobalTimeUs().count();
                        const double window_ms = static_cast<double>(now - (res_rule.at != 0 ? res_rule.at : start));
                        const bool bound = res_rule.at != 0 && (emulated_us - res_rule.emulated_us) / 1e3 < 0.93 * window_ms &&
                                           (wait_ns - res_rule.wait_ns) / 1e6 >= window_ms / 4 &&
                                           (slept_us - res_rule.slept_us) / 1e3 <= window_ms / 50;
#if HEN_DEV
                        if (bound) {
                            std::printf("hen: resolution: window %u in a row: %.0f ms, speed %.0f%%, the renderer waited %.0f ms, "
                                        "the limiter slept %.0f ms%s\n",
                                        res_rule.bound + 1, window_ms, (emulated_us - res_rule.emulated_us) / 10.0 / window_ms,
                                        (wait_ns - res_rule.wait_ns) / 1e6, (slept_us - res_rule.slept_us) / 1e3,
                                        now < res_rule.quiet_until ? " (not counted: just after a step)" : "");
                        }
#endif
                        res_rule.at = now;
                        res_rule.wait_ns = wait_ns;
                        res_rule.slept_us = slept_us;
                        res_rule.emulated_us = emulated_us;
                        res_rule.bound = bound && now >= res_rule.quiet_until ? res_rule.bound + 1 : 0;
                        const bool test = res_rule.forced && now - start >= resolution_step_at_s * 1000ull;
                        if (test || (resolution_step && resolution == 0 && res_rule.bound >= 4)) {
                            res_rule.forced = res_rule.forced && !test;
                            res_rule.bound = 0;
                            res_rule.quiet_until = now + (15000ull << std::min(res_rule.steps, 4u));
                            res_rule.steps += ResolutionStepDown(window, test ? "--resolution-step-at" : "the GPU was the limit for four seconds",
                                                                 (now - start) / 1000);
                        }
                    }
#if HEN_DEV
                    if (crash_test_s != 0 && now - start >= crash_test_s * 1000ull) {
                        std::printf("hen: --crash-test %u: writing to address 8 on the emulation thread\n", crash_test_s);
                        std::fflush(stdout);
                        *reinterpret_cast<volatile int*>(8) = 1;
                    }
#endif
                    // A setup's progress for the log, twice a minute: each look is a few hundred
                    // file lookups on the card. (The emulated screen has the installer's own bar.)
                    if (setup && now - (setup_seen ? setup_seen : start) >= 30000) {
                        setup_seen = now;
                        const Link3ds::Count count = Link3ds::Installed(Link3ds::SetOf(program));
                        std::printf("hen: link: %u of %u system titles installed after %llu s\n",
                                    count.installed, count.total,
                                    static_cast<unsigned long long>((now - start) / 1000));
                    }
                    if (now - last_report >= 5000) {
                        last_report = now;
                        RoomLink::Alive();
#if HEN_DEV
                        // (the interval is said when it changes: a dock or undock while
                        // the title runs)
                        sw_display_poll("while the title runs");
                        // Images handed to the display in this interval, by the time since
                        // the one before.
                        {
                            unsigned long long hist[11];
                            for (int k = 0; k < 11; k++) {
                                hist[k] = __atomic_exchange_n(&hen_shown_hist[k], 0, __ATOMIC_RELAXED);
                            }
                            static unsigned long long vblanks_before;
                            const unsigned long long vblanks = hen_vblank_count - vblanks_before;
                            vblanks_before = hen_vblank_count;
                            std::printf("hen: shown at: %llu presents, mean %.2f ms apart; under 6.3 ms: %llu, ~8.3: %llu, ~16.7: %llu, ~25: %llu, ~33.3: %llu, ~41.7: %llu, ~50: %llu, over 54: %llu; emulated vertical blanks %llu, frames handed over %llu (not presented yet or dropped: %lld; blanks without a frame: %lld)\n",
                                        hist[8], hist[8] ? hist[9] / 1e3 / hist[8] : 0.0, hist[0], hist[1], hist[2],
                                        hist[3], hist[4], hist[5], hist[6], hist[7], vblanks, hist[10],
                                        static_cast<long long>(hist[10]) - static_cast<long long>(hist[8]),
                                        static_cast<long long>(vblanks) - static_cast<long long>(hist[10]));
                        }
                        // The two measures of the GPU's time to spare and their verdicts.
                        {
                            unsigned long long stats[4];
                            for (int k = 0; k < 4; k++) {
                                stats[k] = __atomic_exchange_n(&hen_render_latency_stats[k], 0, __ATOMIC_RELAXED);
                            }
                            std::printf("hen: headroom: GPU done with a frame's drawing %.1f ms after its flush (this interval: %llu frames, mean %.1f, longest %.1f, %llu dropped or not done in 100 ms; running mean %.1f): time to spare %s (none from %.1f ms, again from %.1f); the present fence wait's running mean %.1f ms: time to spare %s; the rule uses the %s\n",
                                        stats[0] ? stats[1] / 1e6 / stats[0] : 0.0, stats[0],
                                        stats[0] ? stats[1] / 1e6 / stats[0] : 0.0, stats[2] / 1e6, stats[3],
                                        hen_render_latency_mean_ns / 1e6, hen_headroom_by_render ? "yes" : "no",
                                        hen_headroom_hi_us / 1e3, hen_headroom_lo_us / 1e3,
                                        hen_present_wait_mean_ns / 1e6, hen_headroom_by_fence ? "yes" : "no",
                                        hen_headroom_render ? "render time" : "present fence wait (--headroom-fence)");
                        }
#endif
                        double hen_thread_cpu_ms = 0;
                        {
                            // How much of the interval this thread had a core (it runs the
                            // emulated CPU): less than the interval's busy time means it waited.
                            double wall_ms = 0, cpu_ms = 0;
                            const unsigned rc = hen_jit_thread_times(&wall_ms, &cpu_ms);
#if HEN_DEV
                            // The host cores each thread was seen on in the interval, as
                            // core:samples, and how often two samples in a row differed.
                            if (core_log) {
                                char emu[64], gpu[64], audio[64];
                                const int emu_changes = hen_core_report(0, emu, sizeof(emu));
                                const int gpu_changes = hen_core_report(1, gpu, sizeof(gpu));
                                const int audio_changes = hen_core_report(2, audio, sizeof(audio));
                                std::printf("hen: thread: interval %.0f ms, emulation thread on a core %.0f ms (0x%x); "
                                            "host cores %s, %d changes; gpu thread cores %s, %d changes; audio "
                                            "thread cores %s, %d changes\n",
                                            wall_ms, cpu_ms, rc, emu_changes < 0 ? "-" : emu, std::max(emu_changes, 0),
                                            gpu_changes < 0 ? "-" : gpu, std::max(gpu_changes, 0),
                                            audio_changes < 0 ? "-" : audio, std::max(audio_changes, 0));
                                // Who may run where: at the first report, and (three times at
                                // most) when the number of threads has changed since.
                                const int threads = hen_thread_count();
                                if (threads != threads_told && thread_tables < 4) {
                                    threads_told = threads;
                                    hen_thread_table(thread_tables++ == 0 ? "at the title's first report" : "the threads changed");
                                }
                                // Every thread's CPU time in the interval; the cores'
                                // idle time (asked for here, printed by its own thread with
                                // the same number); where this thread's time went.
                                const unsigned seq = ++hen_core_log.seq;
                                hen_thread_cpu_report(seq);
                                hen_core_idle_ask(seq);
                                unsigned long long ns[12], count[12], dispatch[5], layout[3];
                                hen_emu_stats(ns, count, dispatch, layout);
                                const unsigned long long ticks_now = hen_jit_code_ticks_total();
                                const unsigned long long interpreted_now = hen_jit_interpreted_ns();
                                const double ticks = static_cast<double>(ticks_now - hen_core_log.ticks_before);
                                const double interpreted_ms = (interpreted_now - hen_core_log.interpreted_before) / 1e6;
                                hen_core_log.ticks_before = ticks_now;
                                hen_core_log.interpreted_before = interpreted_now;
#if HEN_DEV
                                // (emptied at every report, after this: ReportGfx)
                                const long long slept_us = hen_limiter_slept_us;
#else
                                const long long slept_us = hen_limiter_slept_us >= hen_core_log.slept_before
                                                               ? hen_limiter_slept_us - hen_core_log.slept_before
                                                               : hen_limiter_slept_us;
                                hen_core_log.slept_before = hen_limiter_slept_us;
#endif
                                const auto ms = [&](int kind) { return ns[kind] / 1e6; };
                                std::printf("hen: emu: #%u interval %.0f ms, on a core %.0f ms; run loop %.1f ms in %llu passes; "
                                            "emulated CPU %.1f ms in %llu slices (%.1f us each), %.2f M ticks; inside it: system "
                                            "calls %.1f ms (%llu), interpreter %.1f ms, dispatcher lookups in C++ %.2f ms (%llu), "
                                            "memory calls %llu reads %llu writes; thread switches %llu; GPU thread: %llu commands "
                                            "queued in %.2f ms, waited for %.1f ms (%llu times); limiter slept %.1f ms\n",
                                            seq, wall_ms, cpu_ms, ms(0), count[0], ms(7), count[7],
                                            count[7] ? ns[7] / 1e3 / count[7] : 0.0, ticks / 1e6, ms(1), count[1],
                                            interpreted_ms, ms(2), count[2], count[5], count[6], count[4], count[3],
                                            ms(3), ms(8), count[8], slept_us / 1e3);
                                {
                                    const char* names[6];
                                    unsigned long long event_count[6], event_ns[6], total[2];
                                    const int kinds = Core::HenEventStats(names, event_count, event_ns, 6, total);
                                    char events[640];
                                    int at = 0;
                                    events[0] = 0;
                                    for (int k = 0; k < kinds && at < static_cast<int>(sizeof(events)); k++) {
                                        at += std::snprintf(events + at, sizeof(events) - at, " %s %.2f (%llu),", names[k],
                                                            event_ns[k] / 1e6, event_count[k]);
                                    }
                                    std::printf("hen: emu events: #%u %.1f ms in %llu events between slices; most time, as name "
                                                "ms (count):%s\n",
                                                seq, total[1] / 1e6, total[0], events);
                                }
                                // The emulated cores, the emulated threads by host time, the
                                // system calls by number of calls.
                                Core::HenEmuCoresReport(seq, true, system.GetNumCores());
                                Kernel::HenSvcReport(seq, true);
                                std::printf("hen: dispatch: #%u lookups in C++ %llu: the table's entry was empty %llu, held "
                                            "another block %llu, held this block %llu, not compiled (to the interpreter) %llu; "
                                            "table emptied %llu times; layout: %llu blocks placed, set %016llx, order %016llx\n",
                                            seq, count[2], dispatch[0], dispatch[1], dispatch[2], dispatch[3], dispatch[4],
                                            layout[0], layout[1], layout[2]);
                                {
                                    // The blocks compiled; which pages of generated code were in
                                    // use, from a sample.
                                    static unsigned long long late_before[2];
                                    unsigned long long l[5];
                                    hen_layout_stats(l);
                                    std::printf("hen: layout: #%u %llu blocks %llu KiB "
                                                "(+%llu blocks +%llu KiB in the interval); "
                                                "pages of block entries, a sample of %llu dispatcher "
                                                "lookups: %llu of 4 KiB, %llu of 2 MiB\n",
                                                seq, l[0],
                                                l[1] >> 10, l[0] - late_before[0], (l[1] - late_before[1]) >> 10,
                                                l[2], l[3], l[4]);
                                    late_before[0] = l[0];
                                    late_before[1] = l[1];
                                }
                                {
                                    // The background re-sort: what lies in sorted order
                                    // and what was compiled since, over all JITs.
                                    long long r[8];
                                    hen_resort_stats(r);
                                    const long long blocks = r[0] + r[2], bytes = r[1] + r[3];
                                    char mode[48];
                                    if (!hen_resort) {
                                        std::snprintf(mode, sizeof(mode), "off");
                                    } else if (hen_resort_at_ms >= 0) {
                                        std::snprintf(mode, sizeof(mode), "on (once, at %.1f s)", hen_resort_at_ms / 1000.0);
                                    } else {
                                        std::snprintf(mode, sizeof(mode), "on");
                                    }
                                    std::printf("hen: layout: #%u re-sort %s: %lld blocks %lld KiB in sorted order (%.1f%% of the "
                                                "blocks, %.1f%% of the bytes), %lld blocks %lld KiB appended since in compile "
                                                "order; %lld re-sorts so far, %lld abandoned, %lld failed checks; longest switch "
                                                "pause %lld us\n",
                                                seq, mode, r[0], r[1] >> 10, blocks > 0 ? 100.0 * r[0] / blocks : 0.0,
                                                bytes > 0 ? 100.0 * r[1] / bytes : 0.0, r[2], r[3] >> 10, r[4], r[5], r[6], r[7]);
                                }
                                if (double probe[3]; hen_probe_run(probe)) {
                                    std::printf("hen: probe: #%u memory %.1f ns a load (fastest of four parts %.1f), arithmetic "
                                                "%.3f ns a step\n",
                                                seq, probe[0], probe[1], probe[2]);
                                }
                            }
#endif
                            (void)rc;
                            hen_thread_cpu_ms = cpu_ms;
                        }
                        const auto perf = system.GetAndResetPerfStats();
                        // The interval's numbers, read once: for the session report (every
                        // build) and for the log's lines below (development builds).
                        unsigned long long gfx[3 * HEN_GFX_KINDS]{};
                        hen_gfx_stats(gfx);
                        unsigned long long boost[13]{};
                        hen_boost_stats(boost);
                        {
                            SessionReport::Interval interval;
                            interval.speed = perf.emulation_speed;
                            interval.game_fps = perf.game_fps;
                            interval.shown = hen_present_gaps[0];
                            interval.gaps40 = hen_present_gaps[1];
                            interval.gaps70 = hen_present_gaps[2];
                            interval.gaps120 = hen_present_gaps[3];
                            interval.longest_gap_ms = hen_present_gaps[4] / 1e3;
                            interval.pipelines = gfx[3 * HEN_GFX_PIPELINE_COMPILE];
                            interval.blocks_total = hen_blocks_compiled();
                            interval.boost_on_ms = boost[0];
                            SessionReport::Add(interval);
#if !HEN_DEV
                            // (development builds: ReportGfx, after it has printed them)
                            for (auto& value : hen_present_gaps) {
                                value = 0;
                            }
#endif
                        }
                        // The rare boot hang (about three in a hundred boots: the program stops
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
                        if (clock_info) {
                            sw_clock_report("interval"); // (--clock-info: rates and temperature, every interval)
                        }
                        {
                            // Where showing the frames went (the frame swap's time above is
                            // mostly waiting on this).
                            unsigned long long w[8];
                            for (int k = 0; k < 8; k++) {
                                w[k] = hen_swap_ns[k];
                                hen_swap_ns[k] = 0;
                            }
                            if (w[0] != 0 || w[3] != 0) {
                                std::printf("hen: show: %llu frames taken: waiting for a free one %.1f ms each, for the "
                                            "GPU to be done with its last use %.1f, drawing the screens %.1f; %llu "
                                            "shown: the display's next image %.1f ms each, commands to the GPU %.1f, "
                                            "image to the display %.1f\n",
                                            w[0], w[0] ? w[1] / 1e6 / w[0] : 0.0, w[0] ? w[2] / 1e6 / w[0] : 0.0,
                                            w[0] ? w[7] / 1e6 / w[0] : 0.0, w[3], w[3] ? w[4] / 1e6 / w[3] : 0.0,
                                            w[3] ? w[5] / 1e6 / w[3] : 0.0, w[3] ? w[6] / 1e6 / w[3] : 0.0);
                            }
                            // The present thread's own waits for the display to let
                            // go of an acquired image.
                            if (hen_present_cpu_acquire) {
                                const unsigned long long n = __atomic_exchange_n(&hen_present_acquire_wait[0], 0, __ATOMIC_RELAXED);
                                const unsigned long long ns = __atomic_exchange_n(&hen_present_acquire_wait[1], 0, __ATOMIC_RELAXED);
                                const unsigned long long longest = __atomic_exchange_n(&hen_present_acquire_wait[2], 0, __ATOMIC_RELAXED);
                                const unsigned long long ahead = __atomic_exchange_n(&hen_present_acquire_wait[3], 0, __ATOMIC_RELAXED);
                                std::printf("hen: present: cpu acquire on: the present thread waited for the display to "
                                            "let go of an image %llu times, %.1f ms in all (%.2f ms each), longest %.1f ms; "
                                            "%llu of the waits began ahead of their frame\n",
                                            n, ns / 1e6, n ? ns / 1e6 / n : 0.0, longest / 1e6, ahead);
                            }
                        }
                        ReportGfx(gfx, boost);
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
                    if (exit_after_s > 0 && exit_mid_call && mid_exit == 0 &&
                        now - start >= static_cast<uint64_t>(exit_after_s) * 1000) {
                        std::printf("hen: --hen-exit-after %d: --exit-mid-call: running on until a thread is inside a service call\n",
                                    exit_after_s);
                        mid_exit = 2;
                        mid_since = sw_time_ms();
                    } else if (exit_after_s > 0 && mid_exit == 0 &&
                        now - start >= static_cast<uint64_t>(exit_after_s) * 1000) {
                        std::printf("hen: --hen-exit-after %d: stopping\n", exit_after_s);
                        RoomLink::Step("run loop left: --hen-exit-after");
                        ended_how = "a timed test run";
                        closing = true;
                        timed_stop = true;
                        break;
                    } else if (step.secs > 0 && mid_exit == 0 &&
                               now - start >= static_cast<uint64_t>(step.secs) * 1000) {
                        std::printf("hen: --sequence: %d s: back to the titles\n", step.secs);
                        RoomLink::Step("run loop left: --sequence");
                        ended_how = "left by --sequence";
                        break;
                    }
#endif
                }
                combo_armed = false; // (every way out of the run loop comes through here)
                combo_request = false;
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
            } catch (const std::exception& e) {
                std::printf("hen: exception: %s\n", e.what());
                RoomLink::Step("exception");
                failure = std::strcmp(e.what(), "std::bad_alloc") == 0 ? "out of memory" : e.what();
                rc = 1;
                threw = true;
                // (the emulator was left in the middle of something: a time limit on its end)
                exit_deadline_ms = sw_time_ms() + 10000;
            }
#if HEN_DEV
            // What every thread of the emulated console was doing when the title ended: for a
            // title that sits and shows nothing, the thing its threads wait on names the cause.
            if (!threw && !asserted && system.IsPoweredOn()) {
                ReportEmulatedThreads(system);
            }
#endif
            combo_armed = false; // (also after an exception, which skips the run loop's end)
            combo_request = false;
            RoomLink::Step("shutting the emulated system down");
            system.Shutdown();
            Kernel::HenIpc::Reset(); // (the next title's threads get the same numbers)
            RoomLink::Step("emulated system down");
            if (threw && !asserted) {
                exit_deadline_ms = 0;
            }
            GuestDns::Enable(false);
            ToolScan::Drop(); // (a connection the search held and the setup didn't take)
        } catch (const std::exception& e) {
            GuestDns::Enable(false);
            ToolScan::Drop();
            // (Vulkan-Hpp throws on errors: a title that can't start shouldn't take the app down.)
            std::printf("hen: exception: %s\n", e.what());
            RoomLink::Step("exception");
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
        // (after the title is gone: its wireless service has let go of the room client. Back to
        // the title list, the console stays in its room.)
        RoomLink::TitleEnded(picker && !closing);
        {
            // The online record's counts: outcomes only, nothing that names a network.
            char online[600];
            if (hen_online_summary(online, sizeof(online)) != 0) {
                SessionReport::Note(online);
            }
        }
        {
            // Sound shortages of this title.
            static unsigned before;
            unsigned events[4 * 32], entered = 0;
            hen_audio_events(events, &entered);
            SessionReport::Note(fmt::format("audio: stretcher entered {} times", entered - before));
            before = entered;
        }
        SessionReport::End(failure.empty() ? ended_how : failure.c_str());
        sw_crash_title(0);
        // (what a click of the left stick changed for the title that ran)
        Settings::values.resolution_factor = static_cast<u32>(std::clamp(resolution, 0, 10));
        Settings::values.swap_screen = false;
        Settings::values.audio_muted = false;
        hen_boost_off();
        if (keep_awake || unattended() || setup) {
            sw_keep_awake(false);
        }
        std::printf("hen: title ended\n");
        RoomLink::Step("title ended");
        const std::string program_error = HLE::Applets::ErrEula::TakeLastMessage();
        if (!program_error.empty()) {
            std::printf("hen: the program reported: %s\n", program_error.c_str());
        }
        if (asserted) {
            // The title is down in time: the limit waits while the person reads, and counts
            // again for the way out.
            exit_deadline_ms = 0;
            std::printf("hen: assertion: the title was shut down; closing the app\n");
            if (picker && !closing) {
                TitleMenu::ShowMessage(name + " stopped", "An internal error. Kaizhar closes now.", "");
            }
            exit_deadline_ms = sw_time_ms() + 10000;
            rc = 1;
            break;
        }
#if HEN_DEV
        if (!picker && timed_stop && repeat_left > 0) {
            repeat_left--;
            std::printf("hen: --repeat: starting the title again\n");
            continue;
        }
#endif
        if (setup) {
            // The first setup of a console whose region the emulated console's country doesn't
            // fit stops after the console's files are copied: once more with the country set.
            if (!closing && !setup_again && Link3ds::FixCountry(system)) {
                std::printf("hen: link: the country was set to fit the linked console: connecting again\n");
                setup_again = true;
                continue;
            }
            setup_again = false;
            std::string text, error;
            const Link3ds::Set set = Link3ds::SetOf(program);
            const bool complete = Link3ds::End(set, last_status, last_details, error,
                                               [&text](const std::string& line) { text += line + "\n"; });
            // One "Link a 3DS" does both sets: after the Old 3DS set, the New 3DS set from the same
            // 3DS at once. (The tool ends a session when ours does and listens again.)
            if (complete && picker && !closing && set == Link3ds::Set::Old3ds &&
                !Link3ds::Read().new3ds_installed) {
                const std::string where = program.substr(program.find("://") + 3);
                std::string next, why;
                if (Link3ds::Begin("", Link3ds::Set::New3ds, false, next, why)) {
                    std::printf("hen: link: the Old 3DS set is done: the New 3DS set follows\n");
                    program = next + where;
                    setup_next = true;
                    continue;
                }
                std::printf("hen: link: the New 3DS set can't follow: %s\n", why.c_str());
            }
            program.clear();
            if (closing) {
                break;
            }
            if (!complete) {
                text = error;
            } else if (set == Link3ds::Set::Old3ds && !Link3ds::Read().new3ds_installed) {
                text = "Set 1 of 2 done.\nNext: Link a 3DS again for set 2.";
            } else {
                text = "Done. The 3DS is no longer needed.\nNext: turn the online system modules on.";
            }
            if (!picker) {
                break; // (a setup given on the command line: the log has it all)
            }
            TitleMenu::ShowPage(complete ? "Link a 3DS: done" : "Link a 3DS: not done", text);
            rc = 0;
            continue;
        }
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
        } else if (last_status == Core::System::ResultStatus::ShutdownRequested) {
            // (so that it doesn't read as a crash)
            TitleMenu::ShowMessage(name, "It turned the 3DS off.", "");
        }
        rc = 0;
    }
#if HEN_DEV
    hen_jit_profile_stop();
    ReportJit("at exit");
#endif
    sw_menu_shutdown(); // (the instance and the device the list kept for the session go here)
    RoomLink::Step("exiting");
    RoomLink::Stop();
    SessionReport::Close();
    SwitchInput::Shutdown();
    InputCommon::Shutdown();
    sw_mic_shutdown();
    Common::Log::Stop();
    hen_file_defer_finish();
    hen_boost_off();
    sw_cpu_boost_end();
#if HEN_DEV
    sw_power_log_finish();
#endif
    std::printf("hen: exiting\n");
#if HEN_CHAIN
    sw_chain_next(); // (the optional test library's last call, while the log still works)
#endif
    return rc;
}

// The main thread's stack is 1 MiB on this runtime, too small for the emulator (shader
// generation and compilation alone need more): everything runs on a thread with a 64 MiB stack.
int main(int argc, char** argv) {
    std::set_terminate(&OnTerminate);
    hen_thread_name("first"); // (for "hen: threads:")
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
            hen_thread_name("emulation");
            sw_watch_register();
            sw_crash_prepare(); // (the card's file system, for the fault handler)
            assert_thread = pthread_self();
            Common::hen_assert_hook = &OnAssertion;
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
    uint64_t combo_since = 0, combo_asked = 0;
    while (!args.done) {
        sw_watch_poll();
#if HEN_DEV
        sw_power_log_poll(); // (--power-log: nothing without it)
#endif
        // Both sticks, read here (combo_request, above).
        if (!combo_armed.load()) {
            combo_since = combo_asked = 0;
        } else {
            const uint64_t now = sw_time_ms();
            if (!sw_menu_combo_now()) {
                combo_since = 0;
            } else if (combo_since == 0) {
                combo_since = now;
            } else if (now - combo_since >= 200 && !combo_request.exchange(true)) {
                combo_asked = now;
            }
            if (!combo_request.load()) {
                combo_asked = 0;
            } else if (combo_asked != 0) {
                Core::System::GetInstance().HenKick();
                if (now - combo_asked > 8000) {
                    std::printf("hen: both sticks: the emulator thread did not take the request in 8 s: ending the program without it\n");
                    std::fflush(stdout);
                    return 1;
                }
            }
        }
        // After a failed assertion (above): the emulator thread did not get to its end in time.
        const uint64_t deadline = exit_deadline_ms;
        if (deadline != 0 && sw_time_ms() > deadline) {
            std::printf("hen: assertion: the emulator thread did not end in time: ending the program without it\n");
            std::fflush(stdout);
            return 1;
        }
    }
    pthread_join(thread, nullptr);
    return args.rc;
}
