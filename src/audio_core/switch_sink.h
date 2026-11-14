// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "audio_core/sink.h"

namespace AudioCore {

/// Audio output on Switch 2 homebrew: the system's audout service (48 kHz stereo), driven by a
/// thread in the frontend (citra_switch/switch_audio.c) that asks this sink for samples.
class SwitchSink final : public Sink {
public:
    explicit SwitchSink(std::string device_id);
    ~SwitchSink() override;

    unsigned int GetNativeSampleRate() const override;
    void SetCallback(std::function<void(s16*, std::size_t)> cb) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

std::vector<std::string> ListSwitchSinkDevices();

} // namespace AudioCore
