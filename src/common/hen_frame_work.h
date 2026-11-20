// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Switch 2 homebrew port: what the renderer made and recorded since the frontend's last report
// (citra_switch/main.cpp: the lines "hen: gfx: image memory:" and "hen: gfx: frame work:").
// Plain counters: each is written by one thread, and the report reads and empties them from
// another without a lock (a count may be off by a few; hen/jit/hen_jit.c holds them).
// Development builds only: a release build has no report that reads them.
#pragma once

/// Surfaces and images made, and the surface pool (--recycle-surfaces).
enum HenSurfaceStat {
    // Surfaces asked for, by who asked (rasterizer_cache.h, CreateSurface)
    HEN_SURFACE_FRAMEBUFFER, ///< a draw's colour or depth target
    HEN_SURFACE_TEXTURE,     ///< a texture a draw samples
    HEN_SURFACE_MIP,         ///< the temporary surface for a texture whose size is not a multiple of 8
    HEN_SURFACE_TRANSFER,    ///< a display transfer's or a texture copy's source or destination
    HEN_SURFACE_OTHER,       ///< anything else (a cube map, a surface asked for from outside)
    // Images made outside a surface's construction (vk_texture_runtime.cpp)
    HEN_IMAGE_COPY,     ///< the copy a draw samples when it draws to the surface it samples
    HEN_IMAGE_SCALE_UP, ///< a surface's scaled image made anew at a larger factor
    // Images made, by the size of their memory (vk_instance.cpp, HenNoteImageMemory)
    HEN_IMAGE_UNDER_4, ///< under 4 MiB
    HEN_IMAGE_4_TO_8,  ///< 4 MiB to 8 MiB
    HEN_IMAGE_OVER_8,  ///< over 8 MiB
    // Surfaces asked for: with images made anew, or handed back
    HEN_SURFACE_NEW,
    HEN_SURFACE_RECYCLED,      ///< from the pool
    HEN_SURFACE_RECYCLED_SAME, ///< from the deletion queue, same address (as without the pool)
    // The pool
    HEN_POOL_KEPT,         ///< surfaces that entered it
    HEN_POOL_EVICTED_AGE,  ///< destroyed: not asked for in time
    HEN_POOL_EVICTED_SIZE, ///< destroyed: the pool was over its cap
    HEN_POOL_BYTES,        ///< held now (not emptied by the report)
    HEN_POOL_BYTES_PEAK,   ///< the most held since the last report
    HEN_POOL_SURFACES,     ///< held now (not emptied by the report)
    HEN_SURFACE_STATS
};

/// Commands recorded.
enum HenFrameWork {
    HEN_WORK_PASSES,             ///< render passes begun for draws (vk_render_manager.cpp)
    HEN_WORK_CLEARS_WHOLE,       ///< whole-surface clears
    HEN_WORK_CLEARS_PART,        ///< clears of a part of a surface (a pass of their own)
    HEN_WORK_FEEDBACK_COPIES,    ///< copies of a whole surface for a draw that samples its target
    HEN_WORK_FEEDBACK_BYTES,     ///< their bytes
    HEN_WORK_SCALE_UP,           ///< blits from a surface's 1x image to its scaled one
    HEN_WORK_SCALE_DOWN,         ///< and back
    HEN_WORK_DISPLAY_TRANSFERS,  ///< display transfers done on the GPU
    HEN_WORK_UPLOADS,            ///< texture uploads
    HEN_WORK_SUBMITS,            ///< submissions to the queue
    HEN_WORK_COPIES,             ///< copies between two surfaces
    HEN_WORK_BLITS,              ///< blits between two surfaces (display transfers included)
    HEN_WORK_DOWNLOADS,          ///< surface downloads
    // Draws with the generic fragment shader (vk_rasterizer.cpp, HenDrawGeneric): read by the gap line only
    HEN_WORK_GENERIC_WITH,       ///< with the depth write (no early depth test)
    HEN_WORK_GENERIC_WITHOUT,    ///< without it
    HEN_WORK_GENERIC_AREA,       ///< their viewports' pixels, summed: the most they can shade without overdraw
    HEN_WORK_GENERIC_AREA_WITH,  ///< the part of that from draws with the depth write
    HEN_WORK_GENERIC_VERTICES,   ///< their vertices
    HEN_WORK_GENERIC_NOT_EXACT,  ///< of those with the depth write: the depth range could not carry the draw's depth
    // Why a draw began a pass (vk_render_manager.cpp), against the pass the draw before it was in
    HEN_WORK_DRAWS,              ///< draws
    HEN_WORK_PASS_AREA,          ///< same framebuffer, another render area (the viewport moved)
    HEN_WORK_PASS_DEPTH,         ///< same colour image, the depth attachment differs (on, off or another)
    HEN_WORK_PASS_COLOUR,        ///< same depth image, the colour attachment differs
    HEN_WORK_PASS_TARGET,        ///< another target altogether
    HEN_WORK_PASS_RESUMED,       ///< same framebuffer and area: something between the two draws ended the pass
    HEN_FRAME_WORK
};

/// (the arrays' size in hen/jit/hen_jit.c, which is C and does not see this file)
#define HEN_FRAME_WORK_SLOTS 32
static_assert(HEN_SURFACE_STATS <= HEN_FRAME_WORK_SLOTS && HEN_FRAME_WORK <= HEN_FRAME_WORK_SLOTS);

#if defined(__SWITCH__) && HEN_DEV
extern "C" unsigned long long hen_surface_stats[HEN_FRAME_WORK_SLOTS];
extern "C" unsigned long long hen_frame_work[HEN_FRAME_WORK_SLOTS];
extern "C" unsigned long long hen_surface_stats_total[HEN_FRAME_WORK_SLOTS];
extern "C" unsigned long long hen_frame_work_total[HEN_FRAME_WORK_SLOTS];
#define HEN_SURFACE_COUNT(stat, n) (hen_surface_stats[stat] += (n), hen_surface_stats_total[stat] += (n))
#define HEN_WORK_COUNT(stat, n) (hen_frame_work[stat] += (n), hen_frame_work_total[stat] += (n))
#else
#define HEN_SURFACE_COUNT(stat, n) ((void)0)
#define HEN_WORK_COUNT(stat, n) ((void)0)
#endif
