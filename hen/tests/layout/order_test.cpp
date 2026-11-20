// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The block order "pc" and the layout figure (hen_block_order.h), checked on the computer:
//   c++ -std=c++20 -O1 hen/tests/layout/order_test.cpp -o /tmp/order_test && /tmp/order_test [LIST.blocks]
// With a title's saved list (/switch/Kaizhar/jit/<title id>.blocks) it also orders that list.

#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <vector>
#include "hen_block_order.h"

using namespace Core::HenBlockOrder;

namespace {

struct Block {
    std::uint64_t descriptor;
    std::uint32_t hash;
    std::uint16_t words;
    std::uint16_t tag;
};

int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        failures++;
    }
}

/// A layout as the JIT would make it: each block after the one before, 16 bytes a word.
std::uint64_t Figure(const std::vector<Block>& blocks, const std::vector<std::uint32_t>& order, std::uint16_t tag) {
    std::uint64_t figure = LayoutStart(tag), offset = 0x800;
    for (const std::uint32_t i : order) {
        figure = LayoutAdd(figure, blocks[i].descriptor, offset);
        offset += 16ull * blocks[i].words;
    }
    return figure;
}

bool IsPermutation(const std::vector<std::uint32_t>& order, std::size_t count) {
    std::set<std::uint32_t> seen(order.begin(), order.end());
    return order.size() == count && seen.size() == count && (count == 0 || *seen.rbegin() == count - 1);
}

void Synthetic() {
    std::mt19937_64 rng(54);
    const std::uint16_t tag = 0x001;
    std::vector<Block> blocks;
    const std::size_t used = 5000, total = 6000;
    for (std::size_t i = 0; i < total; i++) {
        const std::uint64_t upper = (rng() % 3 == 0) ? 0x1ull : (rng() % 5 == 0 ? 0x03000000ull : 0);
        blocks.push_back({upper << 32 | (0x00100000u + static_cast<std::uint32_t>(rng() % 200000) * 4), static_cast<std::uint32_t>(rng()),
                          static_cast<std::uint16_t>(1 + rng() % 30), tag});
    }
    blocks[17].descriptor = blocks[4000].descriptor; // (the same location twice: the code changed)
    const auto descriptor = [&](std::uint32_t i) { return blocks[i].descriptor; };
    const auto a = Make(total, used, descriptor);
    const auto b = Make(total, used, descriptor);
    Check(IsPermutation(a, total), "every index once");
    Check(a == b, "the same order twice");
    const std::uint64_t figure = Figure(blocks, a, tag);
    Check(figure == Figure(blocks, b, tag), "the same figure twice");
    std::printf("synthetic: order pc figure %016llx\n", static_cast<unsigned long long>(figure));
    {
        bool sorted = true, grouped = true;
        for (std::size_t k = 0; k < total; k++) {
            grouped = grouped && ((a[k] < used) == (k < used));
            if (k != 0 && k != used) {
                sorted = sorted && blocks[a[k - 1]].descriptor <= blocks[a[k]].descriptor;
            }
        }
        Check(grouped, "pc: the blocks the program ran first, speculation after");
        Check(sorted, "pc: by location inside each group");
    }
    // The figure tells one moved block.
    {
        auto moved = a;
        std::swap(moved[100], moved[101]);
        Check(Figure(blocks, a, tag) != Figure(blocks, moved, tag), "two blocks swapped change the figure");
        Check(Figure(blocks, a, tag) != Figure(blocks, a, tag + 0x200), "another JIT's tag changes the figure");
    }
}

void RealList(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::printf("cannot open %s\n", path);
        failures++;
        return;
    }
    std::vector<Block> all;
    Block b;
    while (std::fread(&b, sizeof(b), 1, f) == 1) {
        all.push_back(b);
    }
    std::fclose(f);
    std::set<std::uint16_t> tags;
    for (const Block& block : all) {
        tags.insert(block.tag & 0x7fff);
    }
    std::printf("%s: %zu records, %zu JITs\n", path, all.size(), tags.size());
    for (const std::uint16_t tag : tags) {
        // (as hen_block_list.cpp's Assign: the blocks the program ran, then speculation)
        std::vector<Block> blocks;
        for (const Block& block : all) {
            if (block.tag == tag) {
                blocks.push_back(block);
            }
        }
        const std::size_t used = blocks.size();
        for (const Block& block : all) {
            if (block.tag == (tag | 0x8000)) {
                blocks.push_back(block);
            }
        }
        const auto descriptor = [&](std::uint32_t i) { return blocks[i].descriptor; };
        std::set<std::uint64_t> locations;
        for (const Block& block : blocks) {
            locations.insert(block.descriptor);
        }
        std::printf("  JIT 0x%03x: %zu blocks (%zu ran, %zu on speculation), %zu locations", static_cast<unsigned>(tag), blocks.size(), used,
                    blocks.size() - used, locations.size());
        const auto a = Make(blocks.size(), used, descriptor);
        Check(IsPermutation(a, blocks.size()), "real list: every index once");
        Check(a == Make(blocks.size(), used, descriptor), "real list: the same order twice");
        std::printf("; pc %016llx", static_cast<unsigned long long>(Figure(blocks, a, tag)));
        std::printf("\n");
    }
}

} // namespace

int main(int argc, char** argv) {
    static_assert(sizeof(Block) == 16);
    Synthetic();
    for (int i = 1; i < argc; i++) {
        RealList(argv[i]);
    }
    std::printf(failures == 0 ? "all checks passed\n" : "%d check(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
