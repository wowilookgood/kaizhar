// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "audio_core/audio_types.h"
#include "audio_core/switch_sink.h"
#include "common/logging/log.h"

// citra_switch/switch_audio.c (libnx's headers can't be included here: they clash with the
// core's names).
extern "C" {
unsigned sw_audio_sample_rate(void);
/// Starts playback: fill is called from the audio thread for every buffer (frames of two s16).
bool sw_audio_start(void (*fill)(void* user, int16_t* frames, size_t count), void* user);
void sw_audio_stop(void);
/// The sink takes samples at the 3DS's rate, as the other sinks do, and converts them to the
/// device's 48 kHz itself. (Reporting 48 kHz and leaving the conversion to the time stretcher
/// was the first sound path: it then stretches by 1.47 all the time, audibly.)
float hen_audio_rate_adjust(void); // audio_core/dsp_interface.cpp
}

namespace AudioCore {

namespace {

/// Sample rate conversion with a windowed-sinc filter (32 taps, 256 phases, interpolated).
class Resampler {
public:
    Resampler(double in_rate, double out_rate) : nominal_step(in_rate / out_rate) {
        // Converting upwards: the filter only has to remove the images above the input's
        // band, so its cutoff sits just under the input's Nyquist frequency.
        const double cutoff = std::min(1.0, out_rate / in_rate) * 0.97;
        const double half = Taps / 2.0;
        const double beta = 9.0;
        for (int p = 0; p <= Phases; p++) {
            const double frac = static_cast<double>(p) / Phases;
            double sum = 0.0;
            std::array<double, Taps> row;
            for (int k = 0; k < Taps; k++) {
                const double x = (k - (Taps / 2 - 1)) - frac;
                const double t = x / half;
                const double window =
                    std::abs(t) >= 1.0 ? 0.0 : Bessel0(beta * std::sqrt(1.0 - t * t)) / Bessel0(beta);
                const double arg = M_PI * x * cutoff;
                const double sinc = std::abs(arg) < 1e-9 ? 1.0 : std::sin(arg) / arg;
                row[k] = sinc * window;
                sum += row[k];
            }
            for (int k = 0; k < Taps; k++) {
                table[p][k] = static_cast<float>(row[k] / sum);
            }
        }
    }

    /// Writes count output frames; pull(dst, n) must supply exactly n input frames.
    /// @param adjust  fraction by which to take input faster (+) or slower (-) than nominal
    template <typename Pull>
    void Process(s16* out, std::size_t count, double adjust, Pull&& pull) {
        if (count == 0) {
            return;
        }
        const double step = nominal_step * (1.0 + adjust);
        const std::size_t have = in.size() / 2;
        const std::size_t need = static_cast<std::size_t>(pos + (count - 1) * step) + Taps;
        if (need > have) {
            in.resize(need * 2);
            pull(in.data() + have * 2, need - have);
        }
        for (std::size_t i = 0; i < count; i++) {
            const double p = pos + i * step;
            const std::size_t first = static_cast<std::size_t>(p);
            const float phase = static_cast<float>((p - first) * Phases);
            // (a fraction just under 1 can round up to Phases as a float)
            const int row = std::min(static_cast<int>(phase), Phases - 1);
            const float blend = phase - row;
            const float* a = table[row].data();
            const float* b = table[row + 1].data();
            const s16* src = in.data() + first * 2;
            float left = 0.0f, right = 0.0f;
            for (int k = 0; k < Taps; k++) {
                const float h = a[k] + (b[k] - a[k]) * blend;
                left += h * src[2 * k];
                right += h * src[2 * k + 1];
            }
            out[2 * i] = static_cast<s16>(std::clamp(std::lrintf(left), -32768l, 32767l));
            out[2 * i + 1] = static_cast<s16>(std::clamp(std::lrintf(right), -32768l, 32767l));
        }
        pos += count * step;
        const std::size_t done = std::min(static_cast<std::size_t>(pos), in.size() / 2);
        in.erase(in.begin(), in.begin() + done * 2);
        pos -= done;
    }

private:
    static constexpr int Taps = 32;
    static constexpr int Phases = 256;

    static double Bessel0(double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 32; k++) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    std::array<std::array<float, Taps>, Phases + 1> table;
    std::vector<s16> in; ///< input frames not fully used yet
    double pos = 0.0;    ///< of the next output frame's first tap, in input frames from in[0]
    const double nominal_step;
};

} // namespace

struct SwitchSink::Impl {
    std::mutex mutex;
    std::function<void(s16*, std::size_t)> cb;
    bool started = false;
    std::unique_ptr<Resampler> resampler;

    void Pull(s16* frames, std::size_t count) {
        if (cb) {
            cb(frames, count);
        } else {
            std::memset(frames, 0, count * 2 * sizeof(s16));
        }
    }

    static void Fill(void* user, int16_t* frames, size_t count) {
        auto* impl = static_cast<Impl*>(user);
        std::scoped_lock lock{impl->mutex};
        if (impl->resampler) {
            impl->resampler->Process(frames, count, hen_audio_rate_adjust(),
                                     [impl](s16* dst, std::size_t n) { impl->Pull(dst, n); });
        } else {
            impl->Pull(frames, count);
        }
    }
};

SwitchSink::SwitchSink(std::string) : impl(std::make_unique<Impl>()) {
    impl->resampler = std::make_unique<Resampler>(native_sample_rate, sw_audio_sample_rate());
    impl->started = sw_audio_start(&Impl::Fill, impl.get());
    if (!impl->started) {
        LOG_CRITICAL(Audio_Sink, "Couldn't start audio output (audout); running silent");
    }
}

SwitchSink::~SwitchSink() {
    if (impl->started) {
        sw_audio_stop();
    }
}

unsigned int SwitchSink::GetNativeSampleRate() const {
    return impl->resampler ? native_sample_rate : sw_audio_sample_rate();
}

void SwitchSink::SetCallback(std::function<void(s16*, std::size_t)> cb) {
    std::scoped_lock lock{impl->mutex};
    impl->cb = std::move(cb);
}

std::vector<std::string> ListSwitchSinkDevices() {
    return {"audout"};
}

} // namespace AudioCore
