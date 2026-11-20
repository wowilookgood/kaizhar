// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The order "pc" of a list of blocks and the figure that names a layout: what
// the background re-sort's own order and figure (hen/jit/hen_resort.h) are compared with on the
// computer (hen/tests/layout/resort_test.cpp). Nothing here knows the emulator:
// hen/tests/layout/order_test.cpp checks it on the computer.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Core::HenBlockOrder {

/// The indices of count listed blocks by location (the descriptor's upper half, then the
/// address). descriptor(i) is block i's location; the first used_count blocks are those the
/// program ran, the rest were compiled on speculation, and each group is ordered by itself.
/// Every index appears once. The same list gives the same answer on every run (ties are settled
/// by the index).
template <typename DescriptorOf>
std::vector<std::uint32_t> Make(std::size_t count, std::size_t used_count, DescriptorOf&& descriptor) {
    std::vector<std::uint32_t> out(count);
    for (std::size_t i = 0; i < count; i++) {
        out[i] = static_cast<std::uint32_t>(i);
    }
    used_count = std::min(used_count, count);
    const auto by_location = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint64_t da = descriptor(a), db = descriptor(b);
        return da != db ? da < db : a < b;
    };
    std::sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(used_count), by_location);
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(used_count), out.end(), by_location);
    return out;
}

inline std::uint64_t Mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ x >> 33;
}

/// The figure of one code buffer's listed region: every block placed there, in the order
/// placed, with its offset from the start of the buffer. Equal figures: the same blocks at the
/// same offsets. Start from LayoutStart(tag).
inline std::uint64_t LayoutStart(std::uint16_t tag) {
    return Mix(0x6c61796f75740000ull | tag);
}
inline std::uint64_t LayoutAdd(std::uint64_t figure, std::uint64_t descriptor, std::uint64_t offset) {
    return Mix(figure ^ Mix(descriptor) ^ (offset + 1) * 0x9e3779b97f4a7c15ull);
}

} // namespace Core::HenBlockOrder
