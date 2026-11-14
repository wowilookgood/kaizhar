// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <vector>
#include "audio_core/input.h"

namespace AudioCore {

/// The microphone on Switch 2 homebrew: the system's audin service (48 kHz, s16), read by a
/// thread in the frontend (citra_switch/switch_mic.c) and converted here to what the title
/// asked for. Without a device (any call failing) it behaves as NullInput: no samples.
class SwitchInput final : public Input {
public:
    SwitchInput();
    ~SwitchInput() override;

    void StartSampling(const InputParameters& params) override;
    void StopSampling() override;
    bool IsSampling() override;
    void AdjustSampleRate(u32 sample_rate) override;
    Samples Read() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

std::vector<std::string> ListSwitchInputDevices();

} // namespace AudioCore
