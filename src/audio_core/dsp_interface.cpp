// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstddef>
#include <vector>
#include "audio_core/dsp_interface.h"
#include "audio_core/sink.h"
#include "audio_core/sink_details.h"
#include "common/assert.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/dumping/backend.h"

#ifdef __SWITCH__
#include <atomic>
// Output statistics for the Switch 2 frontend's periodic report (citra_switch/main.cpp).
namespace {
std::atomic<u32> hen_calls, hen_short_calls, hen_short_frames, hen_stretched_calls, hen_skips;
std::atomic<u32> hen_fifo_min{~0u}, hen_fifo_max, hen_ratio_min{~0u}, hen_ratio_max;
std::atomic<u64> hen_fifo_sum;
std::atomic<u32> hen_mode_switches;
std::atomic<float> hen_rate_adjust{0.0f};

// Output modes. Stretched: through the time stretcher, for when emulation runs slow. Direct:
// straight from the queue, with the sink's resampler making corrections of under 1% to hold the
// queue level; clean, but only possible at full speed. Used only from the audio thread.
bool hen_direct = false;
bool hen_draining = false; // direct, still playing what the stretcher had left
u32 hen_good_calls = 0;
double hen_level = 0.0;
constexpr std::size_t hen_target_level = 1640; // frames: 50 ms
// (A deeper queue while the program warms up, waiting for a refill instead of going to the
// stretcher, and playing a surplus off were tried with the catch-up experiment and judged worse
// on average across titles.) A 50 ms queue, the stretcher as soon as the queue
// runs dry, 0.8% of rate adjustment, the queue cut back past 6000 frames, as on 2026-11-10.
constexpr u32 hen_calls_to_direct = 94;        // 1.5 s at full speed
void HenMin(std::atomic<u32>& a, u32 v) {
    if (v < a.load(std::memory_order_relaxed))
        a.store(v, std::memory_order_relaxed);
}
void HenMax(std::atomic<u32>& a, u32 v) {
    if (v > a.load(std::memory_order_relaxed))
        a.store(v, std::memory_order_relaxed);
}
} // namespace
/// For the sink's resampler (switch_sink.cpp): how much faster (+) or slower (-) than nominal
/// to take samples, as a fraction, to keep the queue at its target level.
extern "C" float hen_audio_rate_adjust() {
    return hen_rate_adjust.load(std::memory_order_relaxed);
}
extern "C" void hen_audio_dsp_stats(unsigned* out) {
    out[0] = hen_calls.exchange(0);
    out[1] = hen_short_calls.exchange(0);
    out[2] = hen_short_frames.exchange(0);
    out[3] = hen_stretched_calls.exchange(0);
    out[4] = hen_fifo_min.exchange(~0u);
    out[5] = hen_fifo_max.exchange(0);
    out[6] = out[0] ? static_cast<unsigned>(hen_fifo_sum.exchange(0) / out[0]) : 0;
    out[7] = hen_ratio_min.exchange(~0u);
    out[8] = hen_ratio_max.exchange(0);
    out[9] = hen_skips.exchange(0);
    out[10] = hen_mode_switches.exchange(0);
}
#endif

namespace AudioCore {

DspInterface::DspInterface(Core::System& system_) : system(system_) {}

DspInterface::~DspInterface() = default;

void DspInterface::SetSink(AudioCore::SinkType sink_type, std::string_view audio_device) {
    // Dispose of the current sink first to avoid contention.
    sink.reset();

    sink = AudioCore::GetSinkDetails(sink_type).create_sink(audio_device);
    sink->SetCallback(
        [this](s16* buffer, std::size_t num_frames) { OutputCallback(buffer, num_frames); });
    time_stretcher.SetOutputSampleRate(sink->GetNativeSampleRate());
}

Sink& DspInterface::GetSink() {
    ASSERT(sink);
    return *sink.get();
}

void DspInterface::EnableStretching(bool enable) {
    enable_time_stretching = enable;
}

void DspInterface::OutputFrame(StereoFrame16 frame) {
    if (!sink) {
        return;
    }

    if (sink->ImmediateSubmission()) {
        sink->PushSamples(frame.data(), frame.size());
    } else {
        fifo.Push(frame.data(), frame.size());
    }

    auto video_dumper = system.GetVideoDumper();
    if (video_dumper && video_dumper->IsDumping()) {
        video_dumper->AddAudioFrame(std::move(frame));
    }
}

void DspInterface::OutputSample(std::array<s16, 2> sample) {
    if (!sink) {
        return;
    }

    if (sink->ImmediateSubmission()) {
        sink->PushSamples(&sample, 1);
    } else {
        fifo.Push(&sample, 1);
    }

    auto video_dumper = system.GetVideoDumper();
    if (video_dumper && video_dumper->IsDumping()) {
        video_dumper->AddAudioSample(std::move(sample));
    }
}

void DspInterface::OutputCallback(s16* buffer, std::size_t num_frames) {
    // Determine if we should stretch based on the current emulation speed.
    // TODO: Only activate audio stretching when emulation speed goes below 95% threshold
    //       (see #2487) -OS
    if (performing_time_stretching && !enable_time_stretching) {
        // If we just stopped stretching, flush the stretcher before returning to normal output.
        flushing_time_stretcher = true;
    }
    performing_time_stretching = enable_time_stretching.load();

#ifdef __SWITCH__
    const u32 hen_fifo = static_cast<u32>(fifo.Size());
    hen_calls++;
    hen_fifo_sum += hen_fifo;
    HenMin(hen_fifo_min, hen_fifo);
    HenMax(hen_fifo_max, hen_fifo);
#endif

    std::size_t frames_written = 0;
#ifdef __SWITCH__
    if (performing_time_stretching && hen_direct) {
        if (hen_draining) {
            frames_written = time_stretcher.Drain(buffer, num_frames);
            if (frames_written < num_frames) {
                hen_draining = false;
                time_stretcher.Clear();
            }
        }
        if (!hen_draining && fifo.Size() > 6000) {
            // Far too much queued (emulation ran fast for a moment): skip down to the target.
            std::vector<s16> skipped((fifo.Size() - hen_target_level) * 2);
            fifo.Pop(skipped.data(), skipped.size() / 2);
            hen_skips++;
        }
        if (hen_direct) {
            frames_written += fifo.Pop(buffer + 2 * frames_written, num_frames - frames_written);
        }
        hen_level += (static_cast<double>(fifo.Size()) - hen_level) * 0.02;
        const double hen_target = static_cast<double>(hen_target_level);
        const double hen_rate_limit = 0.008;
        hen_rate_adjust = static_cast<float>(
            std::clamp((hen_level - hen_target) / hen_target_level * 0.01, -hen_rate_limit,
                       hen_rate_limit));
        if (frames_written < num_frames && hen_direct && !hen_draining) {
            // Ran dry: emulation is slow. Back to stretching.
            hen_direct = false;
            hen_good_calls = 0;
            hen_rate_adjust = 0.0f;
            hen_mode_switches++;
        }
    } else
#endif
    if (performing_time_stretching) {
        const std::vector<s16> in{fifo.Pop()};
        const std::size_t num_in{in.size() / 2};
        frames_written = time_stretcher.Process(in.data(), num_in, buffer, num_frames);
#ifdef __SWITCH__
        hen_stretched_calls++;
        hen_skips += time_stretcher.TakeSkips();
        const u32 ratio = static_cast<u32>(time_stretcher.GetStretchRatio() * 10000.0);
        HenMin(hen_ratio_min, ratio);
        HenMax(hen_ratio_max, ratio);
        hen_good_calls = ratio >= 9800 && frames_written == num_frames ? hen_good_calls + 1 : 0;
        if (hen_good_calls >= hen_calls_to_direct) {
            // Full speed again: play out what the stretcher holds, then straight from the queue
            // (which fills meanwhile).
            hen_direct = true;
            hen_draining = true;
            hen_level = hen_target_level;
        }
#endif
    } else {
        if (flushing_time_stretcher) {
            time_stretcher.Flush();
            frames_written = time_stretcher.Process(nullptr, 0, buffer, num_frames);
            flushing_time_stretcher = false;

            // Make sure any frames that did not fit are cleared from the time stretcher,
            // so that they do not bleed into the next time the stretcher is enabled.
            time_stretcher.Clear();
        }
        frames_written += fifo.Pop(buffer, num_frames - frames_written);
    }

#ifdef __SWITCH__
    if (frames_written < num_frames) {
        hen_short_calls++;
        hen_short_frames += static_cast<u32>(num_frames - frames_written);
    }
#endif

    if (frames_written > 0) {
        std::memcpy(&last_frame[0], buffer + 2 * (frames_written - 1), 2 * sizeof(s16));
    }

    // Hold last emitted frame; this prevents popping.
    for (std::size_t i = frames_written; i < num_frames; i++) {
        std::memcpy(buffer + 2 * i, &last_frame[0], 2 * sizeof(s16));
    }

    // Implementation of the hardware volume slider
    // A cubic curve is used to approximate a linear change in human-perceived loudness
    const float linear_volume = std::clamp(Settings::Volume(), 0.0f, 1.0f);
    if (linear_volume != 1.0) {
        const float volume_scale_factor = linear_volume * linear_volume * linear_volume;
        for (std::size_t i = 0; i < num_frames; i++) {
            buffer[i * 2 + 0] = static_cast<s16>(buffer[i * 2 + 0] * volume_scale_factor);
            buffer[i * 2 + 1] = static_cast<s16>(buffer[i * 2 + 1] * volume_scale_factor);
        }
    }
}

} // namespace AudioCore
