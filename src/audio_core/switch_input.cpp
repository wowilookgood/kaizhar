// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdio>
#include <mutex>
#include "audio_core/switch_input.h"

// citra_switch/switch_mic.c (libnx's headers can't be included here: they clash with the
// core's names).
extern "C" {
/// Starts sampling: deliver is called from the microphone's thread with every buffer of the
/// device (interleaved s16 frames). Returns at once; false if there is no microphone to ask.
bool sw_mic_start(void (*deliver)(void* user, const int16_t* frames, size_t count,
                                  unsigned channels, unsigned rate),
                  void* user);
/// Stops sampling: deliver is not called again once this returns. Doesn't wait for the device.
void sw_mic_stop(void);
}

namespace AudioCore {

struct SwitchInput::Impl {
    std::mutex mutex;
    InputParameters params{};
    bool sampling = false;
    Samples queue; ///< converted samples the title hasn't read yet

    // The rate conversion: the mean of the device's samples over each output sample's time.
    // (The device's rate is three to six times the title's, so every output sample covers
    // several of the device's; their mean keeps what is above the title's band out of it.)
    s64 sum = 0;
    u32 summed = 0;
    u32 phase = 0; ///< output samples due, in units of 1 / (device rate * output rate) s

#if HEN_DEV
    u64 reads = 0, bytes_read = 0;
#endif

    void Emit(s16 sample) {
        // (A title that doesn't read keeps at most a second.)
        if (queue.size() >= std::size_t{params.sample_rate} * 2) {
            return;
        }
        u16 value = static_cast<u16>(sample);
        if (params.sign == Signedness::Unsigned) {
            value ^= 0x8000;
        }
        if (params.sample_size == 8) {
            queue.push_back(static_cast<u8>(value >> 8));
        } else {
            queue.push_back(static_cast<u8>(value));
            queue.push_back(static_cast<u8>(value >> 8));
        }
    }

    static void Deliver(void* user, const int16_t* frames, size_t count, unsigned channels,
                        unsigned rate) {
        auto* impl = static_cast<Impl*>(user);
        std::scoped_lock lock{impl->mutex};
        const u32 out_rate = impl->params.sample_rate;
        if (!impl->sampling || out_rate == 0) {
            return;
        }
        for (size_t i = 0; i < count; i++) {
            s32 mono = 0;
            for (unsigned c = 0; c < channels; c++) {
                mono += frames[i * channels + c];
            }
            impl->sum += mono / static_cast<s32>(channels);
            impl->summed++;
            impl->phase += out_rate;
            if (impl->phase >= rate) {
                const s16 sample = static_cast<s16>(impl->sum / impl->summed);
                do {
                    impl->Emit(sample);
                    impl->phase -= rate;
                } while (impl->phase >= rate);
                impl->sum = 0;
                impl->summed = 0;
            }
        }
    }
};

SwitchInput::SwitchInput() : impl(std::make_unique<Impl>()) {}

SwitchInput::~SwitchInput() {
    StopSampling();
}

void SwitchInput::StartSampling(const InputParameters& params) {
    if (IsSampling()) {
        return;
    }
    {
        std::scoped_lock lock{impl->mutex};
        parameters = params;
        impl->params = params;
        impl->queue.clear();
        impl->sum = 0;
        impl->summed = 0;
        impl->phase = 0;
        impl->sampling = true;
    }
    // (Without a device the title samples all the same and reads nothing, as with NullInput.)
    [[maybe_unused]] const bool device = sw_mic_start(&Impl::Deliver, impl.get());
#if HEN_DEV
    impl->reads = impl->bytes_read = 0;
    std::printf("hen: mic: title starts sampling: %u Hz, %u-bit %s, %s\n", params.sample_rate,
                params.sample_size, params.sign == Signedness::Signed ? "signed" : "unsigned",
                device ? "device asked" : "no device: silence");
#endif
}

void SwitchInput::StopSampling() {
    if (!IsSampling()) {
        return;
    }
    sw_mic_stop();
    std::scoped_lock lock{impl->mutex};
    impl->sampling = false;
    impl->queue.clear();
#if HEN_DEV
    std::printf("hen: mic: title stops sampling: it read %llu bytes in %llu reads\n",
                static_cast<unsigned long long>(impl->bytes_read),
                static_cast<unsigned long long>(impl->reads));
#endif
}

bool SwitchInput::IsSampling() {
    return impl->sampling;
}

void SwitchInput::AdjustSampleRate(u32 sample_rate) {
    std::scoped_lock lock{impl->mutex};
    parameters.sample_rate = sample_rate;
    impl->params.sample_rate = sample_rate;
}

Samples SwitchInput::Read() {
    Samples samples;
    std::scoped_lock lock{impl->mutex};
    if (!impl->sampling) {
        return samples;
    }
    samples.swap(impl->queue);
#if HEN_DEV
    if (!samples.empty()) {
        if (impl->bytes_read == 0) {
            std::printf("hen: mic: title reads its first samples (%zu bytes)\n", samples.size());
        }
        impl->bytes_read += samples.size();
        impl->reads++;
    }
#endif
    return samples;
}

std::vector<std::string> ListSwitchInputDevices() {
    return {"audin"};
}

} // namespace AudioCore
