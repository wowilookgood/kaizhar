// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include "common/common_types.h"

namespace soundtouch {
class SoundTouch;
}

namespace AudioCore {

class TimeStretcher {
public:
    TimeStretcher();
    ~TimeStretcher();

    void SetOutputSampleRate(unsigned int sample_rate);

    /// @param in       Input sample buffer
    /// @param num_in   Number of input frames in `in`
    /// @param out      Output sample buffer
    /// @param num_out  Desired number of output frames in `out`
    /// @returns Actual number of frames written to `out`
    std::size_t Process(const s16* in, std::size_t num_in, s16* out, std::size_t num_out);

    /// Takes samples already stretched, without putting more in.
    /// @returns Actual number of frames written to `out`
    std::size_t Drain(s16* out, std::size_t num_out);

    void Clear();

    void Flush();

    /// For statistics: the tempo last given to the stretcher (input frames per output frame).
    double GetStretchRatio() const {
        return stretch_ratio;
    }

private:
    std::unique_ptr<soundtouch::SoundTouch> sound_touch;
    double stretch_ratio = 1.0;
    std::array<u32, 12> window_in{}, window_out{};
    std::size_t window_at = 0;
    unsigned skips = 0; ///< times the backlog was cut (Switch 2 port)

public:
    unsigned TakeSkips() {
        const unsigned n = skips;
        skips = 0;
        return n;
    }
};

} // namespace AudioCore
