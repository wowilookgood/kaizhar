// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Host check for the ETC1 decode shared with worker threads (rasterizer_cache/texture_codec.h,
// HEN_ETC1_WORKERS): every texture is decoded twice from the same random bytes, once by the
// plain loop and once split over real threads, and the two results must be the same bytes.
// Build and run: ./run.sh (Mac, clang; no emulator code is linked, only the headers).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#define HEN_ETC1_WORKERS 1
#include "video_core/rasterizer_cache/texture_codec.h"

int hen_etc1_workers = 1;
unsigned long long hen_etc1_split[2];
static unsigned long long calls_from_other_threads;

// (the emulator's version lends the renderer's shader workers; this one starts three threads
// per call and makes them take the indices from the top, so the order differs from the loop's)
namespace VideoCore::HenWorkers {
bool ParallelFor(u32 count, const std::function<void(u32)>& work) {
    std::atomic<u32> next{0};
    std::atomic<unsigned long long> others{0};
    const auto run = [&](bool other) {
        for (;;) {
            const u32 index = next.fetch_add(1);
            if (index >= count) {
                return;
            }
            work(count - 1 - index);
            if (other) {
                others++;
            }
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; i++) {
        threads.emplace_back(run, true);
    }
    run(false);
    for (auto& thread : threads) {
        thread.join();
    }
    calls_from_other_threads += others;
    return true;
}
} // namespace VideoCore::HenWorkers

// (what common/assert.h and the log macros call; the emulator's logging is not linked here)
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char* filename, unsigned int line_num,
                       const char* function, fmt::string_view format, const fmt::format_args&) {
    std::printf("log: %s:%u %s: %.*s\n", filename, line_num, function,
                static_cast<int>(format.size()), format.data());
}
void Stop() {}
} // namespace Common::Log

int main() {
    using VideoCore::PixelFormat;
    std::mt19937 random{20261116};
    unsigned long long textures = 0, split = 0, bytes = 0;
    const u32 sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024};
    for (int round = 0; round < 6; round++) {
        for (const u32 width : sizes) {
            for (const u32 height : sizes) {
                for (int kind = 0; kind < 4; kind++) {
                    const bool alpha = kind & 1;
                    const bool converted = kind & 2;
                    const PixelFormat format = alpha ? PixelFormat::ETC1A4 : PixelFormat::ETC1;
                    const u32 tile_size = alpha ? 64 : 32;
                    const u32 tiles = (width / 8) * (height / 8);
                    // (a part of the texture from a tile boundary to a tile boundary, as an
                    // upload of some of its rows is; the whole of it every other round)
                    u32 first = 0, last = tiles;
                    if (round & 1) {
                        first = random() % tiles;
                        last = first + 1 + random() % (tiles - first);
                    }
                    std::vector<u8> source((last - first) * tile_size);
                    for (u8& byte : source) {
                        byte = static_cast<u8>(random());
                    }
                    const std::size_t linear_size = static_cast<std::size_t>(width) * height * 4 + 64;
                    std::vector<u8> plain(linear_size, 0xAB), shared(linear_size, 0xAB);
                    const auto func = (converted ? VideoCore::UNSWIZZLE_TABLE_CONVERTED
                                                 : VideoCore::UNSWIZZLE_TABLE)[static_cast<u32>(format)];
                    if (func == nullptr) {
                        continue; // (the renderer has no such variant for this format)
                    }
                    const auto decode = [&](std::vector<u8>& out) {
                        func(width, height, first * tile_size, last * tile_size, out, source);
                    };
                    hen_etc1_workers = 0;
                    decode(plain);
                    hen_etc1_workers = 1;
                    const unsigned long long before = hen_etc1_split[0];
                    decode(shared);
                    split += hen_etc1_split[0] - before;
                    textures++;
                    bytes += linear_size;
                    if (plain != shared) {
                        std::printf("DIFFERENT: %ux%u %s%s tiles %u..%u\n", width, height,
                                    alpha ? "ETC1A4" : "ETC1", converted ? " converted" : "", first,
                                    last);
                        return 1;
                    }
                }
            }
        }
    }
    std::printf("%llu decodes compared (%llu MiB), %llu of them split over threads (%llu tiles, "
                "%llu runs done by another thread): all byte-identical\n",
                textures, bytes >> 20, split, hen_etc1_split[1], calls_from_other_threads);
    return split != 0 && calls_from_other_threads != 0 ? 0 : 3;
}
