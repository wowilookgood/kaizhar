// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/perf_stats.h"
#include "video_core/gpu.h"

using namespace std::chrono_literals;
using DoubleSecs = std::chrono::duration<double, std::chrono::seconds::period>;
using std::chrono::duration_cast;
using std::chrono::microseconds;

constexpr double FRAME_LENGTH = 1.0 / SCREEN_REFRESH_RATE;
// Purposefully ignore the first five frames, as there's a significant amount of overhead in
// booting that we shouldn't account for
constexpr std::size_t IgnoreFrames = 5;

#ifdef __SWITCH__
extern "C" long long hen_cpu_shade_debt_ns; // hen/jit/hen_jit.c
extern "C" int hen_max_lag_ms;
extern "C" long long hen_limiter_given_up_us, hen_limiter_slept_us; // (for the report)
namespace Core::HenBlockList {
void UpgradeLinks(); // arm/dynarmic/hen_block_list.cpp
}
extern "C" void hen_boost_note_frame(void); // hen/jit/hen_jit.c: the loading boost
extern "C" void hen_boost_tick(long long limiter_slept_total_us);
extern "C" long long hen_limiter_slept_total_us;
namespace Core {
void HenClockControl(Timing& timing, s64 work_us, s64 budget_us); // core_timing.cpp
}
static long long hen_spare_ring[10] = {};
static long long hen_spare_slot = 0;
extern "C" {
long long hen_recent_spare_ns = 0; // (sleep in the last half second; vk_rasterizer.cpp)
long long hen_limiter_slept_total_us = 0; // (never reset: read by the benchmark marks in svc.cpp)
}
#endif

namespace Core {

bool PerfStats::game_frames_updated = true;

PerfStats::PerfStats(u64 title_id) : title_id(title_id) {}

PerfStats::~PerfStats() {
    if (!Settings::values.record_frame_times || title_id == 0) {
        return;
    }

    const std::time_t t = std::time(nullptr);
    std::ostringstream stream;
    std::copy(perf_history.begin() + IgnoreFrames, perf_history.begin() + current_index,
              std::ostream_iterator<double>(stream, "\n"));
    const std::string& path = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
    // %F Date format expanded is "%Y-%m-%d"
    const std::string filename =
        fmt::format("{}/{:%F-%H-%M}_{:016X}.csv", path, *std::localtime(&t), title_id);
    FileUtil::IOFile file(filename, "w");
    file.WriteString(stream.str());
}

void PerfStats::BeginSVCProcessing() {
    start_svc_time = Clock::now();
}

void PerfStats::EndSVCProcessing() {
    accumulated_svc_time += (Clock::now() - start_svc_time);
}

void PerfStats::BeginIPCProcessing() {
    start_ipc_time = Clock::now();
}

void PerfStats::EndIPCProcessing() {
    accumulated_ipc_time += (Clock::now() - start_ipc_time);
}

void PerfStats::BeginGPUProcessing() {
    start_gpu_time = Clock::now();
}

void PerfStats::EndGPUProcessing() {
    accumulated_gpu_time += (Clock::now() - start_gpu_time);
}

void PerfStats::StartSwap() {
    start_swap_time = Clock::now();
}

void PerfStats::EndSwap() {
    accumulated_swap_time += (Clock::now() - start_swap_time);
}

void PerfStats::BeginSystemFrame() {
    std::scoped_lock lock{object_mutex};

    frame_begin = Clock::now();
}

void PerfStats::EndSystemFrame() {
    std::scoped_lock lock{object_mutex};

    auto frame_end = Clock::now();
    const auto frame_time = frame_end - frame_begin;
    if (current_index < perf_history.size()) {
        perf_history[current_index++] =
            std::chrono::duration<double, std::milli>(frame_time).count();
    }
    accumulated_frametime += frame_time;
    system_frames += 1;

    // TODO: Track previous frame times in a less stupid way. -OS
    previous_previous_frame_length = previous_frame_length;

    previous_frame_length = frame_end - previous_frame_end;
    previous_frame_end = frame_end;
}

void PerfStats::EndGameFrame() {
    std::scoped_lock lock{object_mutex};

    game_frames += 1;
    PerfStats::game_frames_updated = true;
#ifdef __SWITCH__
    hen_boost_note_frame();
#endif
}

double PerfStats::GetMeanFrametime() const {
    std::scoped_lock lock{object_mutex};

    if (current_index <= IgnoreFrames) {
        return 0;
    }

    const double sum = std::accumulate(perf_history.begin() + IgnoreFrames,
                                       perf_history.begin() + current_index, 0.0);
    return sum / static_cast<double>(current_index - IgnoreFrames);
}

PerfStats::Results PerfStats::GetAndResetStats(microseconds current_system_time_us) {
    std::scoped_lock lock{object_mutex};

    const auto now = Clock::now();
    // Walltime elapsed since stats were reset
    const auto interval = duration_cast<DoubleSecs>(now - reset_point).count();

    const auto system_us_per_second = (current_system_time_us - reset_point_system_us) / interval;

    last_stats.system_fps = static_cast<double>(system_frames) / interval;
    last_stats.game_fps = static_cast<double>(game_frames) / interval;
    last_stats.time_vblank_interval =
        system_frames ? (duration_cast<DoubleSecs>(accumulated_frametime).count() /
                         static_cast<double>(system_frames))
                      : 0;
    last_stats.time_hle_svc =
        system_frames
            ? (duration_cast<DoubleSecs>(accumulated_svc_time - accumulated_ipc_time).count() /
               static_cast<double>(system_frames))
            : 0;
    last_stats.time_hle_ipc =
        system_frames
            ? (duration_cast<DoubleSecs>(accumulated_ipc_time - accumulated_gpu_time).count() /
               static_cast<double>(system_frames))
            : 0;
    last_stats.time_gpu = system_frames ? (duration_cast<DoubleSecs>(accumulated_gpu_time).count() /
                                           static_cast<double>(system_frames))
                                        : 0;
    last_stats.time_swap = system_frames
                               ? (duration_cast<DoubleSecs>(accumulated_swap_time).count() /
                                  static_cast<double>(system_frames))
                               : 0;

    last_stats.time_remaining =
        system_frames ? (duration_cast<DoubleSecs>((accumulated_frametime - accumulated_svc_time) -
                                                   accumulated_swap_time)
                             .count() /
                         static_cast<double>(system_frames))
                      : 0;
    last_stats.emulation_speed = system_us_per_second.count() / 1'000'000.0;
    last_stats.artic_transmitted = static_cast<double>(artic_transmitted) / interval;
    last_stats.artic_events.raw = artic_events.raw | prev_artic_event.raw;

    // Reset counters
    reset_point = now;
    reset_point_system_us = current_system_time_us;
    accumulated_frametime = Clock::duration::zero();
    system_frames = 0;
    accumulated_svc_time = Clock::duration::zero();
    accumulated_ipc_time = Clock::duration::zero();
    accumulated_gpu_time = Clock::duration::zero();
    accumulated_swap_time = Clock::duration::zero();
    game_frames = 0;
    artic_transmitted = 0;
    prev_artic_event.raw &= artic_events.raw;

    return last_stats;
}

PerfStats::Results PerfStats::GetLastStats() {
    std::scoped_lock lock{object_mutex};

    return last_stats;
}

double PerfStats::GetLastFrameTimeScale() const {
    std::scoped_lock lock{object_mutex};

    return duration_cast<DoubleSecs>(previous_frame_length).count() / FRAME_LENGTH;
}

double PerfStats::GetStableFrameTimeScale() const {
    std::scoped_lock lock{object_mutex};

    const double stable_previous_frame_length =
        (duration_cast<DoubleSecs>(previous_frame_length).count() +
         duration_cast<DoubleSecs>(previous_previous_frame_length).count()) /
        2;
    return stable_previous_frame_length / FRAME_LENGTH;
}

void FrameLimiter::WaitForFrameAdvance() {
    if (wait_callback) {
        // Let the callback wait in short steps, checking for the frame advance in between
        constexpr auto check_interval = std::chrono::milliseconds(2);
        while (!frame_advance_event.IsSet()) {
            wait_callback(std::chrono::steady_clock::now() + check_interval);
        }
    }
    frame_advance_event.Wait();
    frame_advance_event.Reset();
}

void FrameLimiter::SetWaitCallback(WaitCallback callback) {
    wait_callback = std::move(callback);
}

void FrameLimiter::WaitOnce() {
    if (frame_advancing_enabled) {
        // Frame advancing is enabled: wait on event instead of doing framelimiting
        WaitForFrameAdvance();
    }
}

void FrameLimiter::DoFrameLimiting(microseconds current_system_time_us) {
#ifdef __SWITCH__
    Core::HenBlockList::UpgradeLinks();
    hen_boost_tick(hen_limiter_slept_total_us);
#endif
    if (frame_advancing_enabled) {
        // Frame advancing is enabled: wait on event instead of doing framelimiting
        WaitForFrameAdvance();
        return;
    }

    auto now = Clock::now();
    double sleep_scale = Settings::GetFrameLimit() / 100.0;

    if (Settings::GetFrameLimit() == 0) {
        return;
    }

    // Max lag caused by slow frames. Shouldn't be more than the length of a frame at the current
    // speed percent or it will clamp too much and prevent this from properly limiting to that
    // percent. High values means it'll take longer after a slow frame to recover and start limiting
    const microseconds max_lag_time_us = duration_cast<microseconds>(
#ifdef __SWITCH__
        // (Switch 2 port) hen_max_lag_ms, 60 by default: time lost to a slow frame beyond this
        // is given up for good. Upstream's 25 ms gave up part of every frame while vertex
        // shaders ran on the CPU for pipelines still compiling: a GPU command list then takes
        // about 24 ms of wall time and none of the emulated clock's, and with the rest of the
        // frame's work the lag passed 25 ms. The emulation thread slept half the time and
        // still ran at 85% (Pokemon Omega Ruby's title scene, first launch).
        std::chrono::duration<double, std::chrono::microseconds::period>(
            std::chrono::milliseconds(hen_max_lag_ms) / sleep_scale));
#else
        std::chrono::duration<double, std::chrono::microseconds::period>(25ms / sleep_scale));
#endif
    frame_limiting_delta_err += duration_cast<microseconds>(
        std::chrono::duration<double, std::chrono::microseconds::period>(
            (current_system_time_us - previous_system_time_us) / sleep_scale));
    frame_limiting_delta_err -= duration_cast<microseconds>(now - previous_walltime);
#ifdef __SWITCH__
    // (the automatic emulated clock of New 3DS titles: what this blank's work took of its time)
    Core::HenClockControl(
        Core::System::GetInstance().CoreTiming(),
        duration_cast<microseconds>(now - previous_walltime).count(),
        static_cast<s64>((current_system_time_us - previous_system_time_us).count() / sleep_scale));
    // (not counted: more than 5 s behind, which is time in the title list or the app suspended)
    if (frame_limiting_delta_err < -max_lag_time_us &&
        frame_limiting_delta_err > -microseconds(5'000'000)) {
        hen_limiter_given_up_us += (-max_lag_time_us - frame_limiting_delta_err).count();
    }
#endif
    frame_limiting_delta_err =
        std::clamp(frame_limiting_delta_err, -max_lag_time_us, max_lag_time_us);

    if (frame_limiting_delta_err > microseconds::zero()) {
        if (wait_callback) {
            wait_callback(std::chrono::steady_clock::now() + frame_limiting_delta_err);
        } else {
            std::this_thread::sleep_for(frame_limiting_delta_err);
        }
        auto now_after_sleep = Clock::now();
        frame_limiting_delta_err -= duration_cast<microseconds>(now_after_sleep - now);
#ifdef __SWITCH__
        hen_limiter_slept_us += duration_cast<microseconds>(now_after_sleep - now).count();
        hen_limiter_slept_total_us += duration_cast<microseconds>(now_after_sleep - now).count();
        // (the sleeps of the last half second, by 50 ms slot: what the emulation has had to
        // spare lately. The rasterizer's allowance for draws shaded on the CPU goes by it.)
        {
            const long long slot_now =
                duration_cast<std::chrono::milliseconds>(now_after_sleep.time_since_epoch()).count() / 50;
            if (slot_now != hen_spare_slot) {
                const long long step = std::min<long long>(slot_now - hen_spare_slot, 10);
                for (long long i = 1; i <= step; i++) {
                    hen_spare_ring[(hen_spare_slot + i) % 10] = 0;
                }
                hen_spare_slot = slot_now;
            }
            hen_spare_ring[slot_now % 10] +=
                duration_cast<std::chrono::nanoseconds>(now_after_sleep - now).count();
            long long sum = 0;
            for (const long long v : hen_spare_ring) {
                sum += v;
            }
            hen_recent_spare_ns = sum;
        }
        // (time to spare pays off what vertex shaders run on the CPU have cost: vk_rasterizer.cpp)
        hen_cpu_shade_debt_ns = std::max(
            0ll, hen_cpu_shade_debt_ns -
                     static_cast<long long>(
                         duration_cast<std::chrono::nanoseconds>(now_after_sleep - now).count()));
#endif
        now = now_after_sleep;
    }

    previous_system_time_us = current_system_time_us;
    previous_walltime = now;
}

bool FrameLimiter::IsFrameAdvancing() const {
    return frame_advancing_enabled;
}

void FrameLimiter::SetFrameAdvancing(bool value) {
    const bool was_enabled = frame_advancing_enabled.exchange(value);
    if (was_enabled && !value) {
        // Set the event to let emulation continue
        frame_advance_event.Set();
    }
}

void FrameLimiter::AdvanceFrame() {
    frame_advance_event.Set();
}

} // namespace Core
