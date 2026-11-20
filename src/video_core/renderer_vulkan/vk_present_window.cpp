// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/microprofile.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_present_window.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "vk_platform.h"

#include <vk_mem_alloc.h>

// (Kaizhar) Where the time of showing a frame goes, for the port's log (hen/jit/hen_jit.c).
#ifdef __SWITCH__
#include <chrono>
#include <thread>
extern "C" unsigned long long hen_swap_ns[8];
// (hen/jit/hen_jit.c) --present-cpu-acquire, and the present thread's waits for an
// acquired image to be let go by the display: count, ns in all, longest ns.
extern "C" int hen_present_cpu_acquire;
// [3]: of those, waits begun ahead of the frame (PreAcquire).
extern "C" unsigned long long hen_present_acquire_wait[4];
// 0..7: presents by the time since the one before (CopyToSwapchain); 8: presents counted;
// 9: the sum of those times, microseconds; 10: frames handed over by the renderer (Present).
#if HEN_DEV
extern "C" {
unsigned long long hen_shown_hist[11];
}
#endif
#endif
// (the timings and counts of showing a frame are the development builds': nothing otherwise)
#if defined(__SWITCH__) && HEN_DEV
#define HEN_SWAP_NOW()                                                                             \
    static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(          \
                                        std::chrono::steady_clock::now().time_since_epoch())       \
                                        .count())
#define HEN_SWAP_START() unsigned long long hen_swap_at = HEN_SWAP_NOW()
#define HEN_SWAP_ADD(i)                                                                            \
    do {                                                                                           \
        const unsigned long long hen_swap_now = HEN_SWAP_NOW();                                    \
        hen_swap_ns[i] += hen_swap_now - hen_swap_at;                                              \
        hen_swap_at = hen_swap_now;                                                                \
    } while (0)
#define HEN_SWAP_START2() unsigned long long hen_swap_at2 = HEN_SWAP_NOW()
#define HEN_SWAP_ADD2(i)                                                                           \
    do {                                                                                           \
        const unsigned long long hen_swap_now = HEN_SWAP_NOW();                                    \
        hen_swap_ns[i] += hen_swap_now - hen_swap_at2;                                             \
        hen_swap_at2 = hen_swap_now;                                                               \
    } while (0)
#define HEN_SWAP_COUNT(i) hen_swap_ns[i]++
#else
#define HEN_SWAP_START()
#define HEN_SWAP_ADD(i)
#define HEN_SWAP_START2()
#define HEN_SWAP_ADD2(i)
#define HEN_SWAP_COUNT(i)
#endif

MICROPROFILE_DEFINE(Vulkan_WaitPresent, "Vulkan", "Wait For Present", MP_RGB(128, 128, 128));

#ifdef __SWITCH__
#include <chrono>
// (hen/jit/hen_jit.c) the present wait's running mean and what follows from it: GetRenderFrame
extern "C" unsigned long long hen_present_wait_mean_ns;
extern "C" int hen_gpu_headroom;
extern "C" unsigned long long hen_gpu_headroom_flips, hen_gpu_headroom_frames_without;
// A second measure of whether the GPU has time to spare, valid at any
// refresh rate: how long the GPU takes to finish a frame's drawing, from the renderer flushing
// the frame's commands to the scheduler's tick for them being reached on the GPU.
// Why: the first measure (the wait for a frame object's present fence, in GetRenderFrame) is
// several ms a frame for handheld Xenoblade on a 60 Hz display and 0.0 to 0.7 on a 120 Hz one
// with the same drawing: the present's commands wait for the display to release an image, so
// that wait is mostly the display's pace. At 120 Hz it always said "time to spare".
// How, and why it cannot hold anything up: timed on the present thread, inside
// CopyToSwapchain, i.e. with the swapchain mutex held and before the frame object goes back
// to the free queue, the renderer waits for that queue (GetRenderFrame) and WaitPresent for
// that mutex, so every frame is held back by its own measurement: Xenoblade fell to 85%.
// Here the renderer only queues (tick, time) under a mutex that is never held across a wait
// or a driver call, and a thread of its own (HenWatchThread) asks the scheduler's tick every
// millisecond until it is reached. That thread holds no lock while it asks or sleeps, owns no
// frame object and is waited for by nobody; if it falls behind, entries are dropped.
// Thresholds: no time to spare from hen_headroom_hi_us of running mean (about sixteen frames),
// again from hen_headroom_lo_us. 9 and 7 ms are PROVISIONAL (no clean log of this quantity
// exists: the numbers at hand are of a pipeline the measurement was blocking). This measure
// rules by default (the first one reads the display's pace, which changes
// with the refresh rate), --headroom-fence brings the first one back, --headroom-ms HI,LO sets
// the thresholds. Both verdicts are kept and printed whichever rules.
extern "C" {
int hen_headroom_render = 1;
int hen_headroom_hi_us = 9000, hen_headroom_lo_us = 7000;
int hen_headroom_by_fence = 1, hen_headroom_by_render = 1;
unsigned long long hen_render_latency_mean_ns;
unsigned long long hen_render_latency_stats[4]; // frames, sum ns, max ns, dropped or over 100 ms
unsigned long long hen_render_latency_gap_max_ns; // the longest since the gap line last took it (renderer_vulkan.cpp)
// All the time the renderer has waited for a frame object and for the GPU to be done with
// it (GetRenderFrame), ns, never reset: the automatic resolution's sign that the GPU is the
// limit (citra_switch/main.cpp).
unsigned long long hen_render_wait_ns;
}
#endif

namespace Vulkan {

namespace {
/// (Kaizhar) --present-cpu-acquire
bool HenCpuAcquire() {
#ifdef __SWITCH__
    return hen_present_cpu_acquire != 0;
#else
    return false;
#endif
}


bool CanBlitToSwapchain(const vk::PhysicalDevice& physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                          s32 swapchain_height) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = swapchain_width,
                    .y = swapchain_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageCopy MakeImageCopy(u32 frame_width, u32 frame_height, u32 swapchain_width,
                                          u32 swapchain_height) {
    return vk::ImageCopy{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffset =
            vk::Offset3D{
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffset =
            vk::Offset3D{
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .extent =
            vk::Extent3D{
                .width = std::min(frame_width, swapchain_width),
                .height = std::min(frame_height, swapchain_height),
                .depth = 1,
            },
    };
}

} // Anonymous namespace

PresentWindow::PresentWindow(Frontend::EmuWindow& emu_window_, const Instance& instance_,
                             Scheduler& scheduler_, bool low_refresh_rate_)
    : emu_window{emu_window_}, instance{instance_}, scheduler{scheduler_},
      low_refresh_rate{low_refresh_rate_},
      surface{CreateSurface(instance.GetInstance(), emu_window)}, next_surface{surface},
      swapchain{instance, emu_window.GetFramebufferLayout().width,
                emu_window.GetFramebufferLayout().height, surface, low_refresh_rate_},
      graphics_queue{instance.GetGraphicsQueue()}, present_renderpass{CreateRenderpass()},
      vsync_enabled{Settings::values.use_vsync.GetValue()},
      blit_supported{
          CanBlitToSwapchain(instance.GetPhysicalDevice(), swapchain.GetSurfaceFormat().format)},
      use_present_thread{Settings::values.async_presentation.GetValue()},
      last_render_surface{emu_window.GetWindowInfo().render_surface} {

    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    const vk::CommandPoolCreateInfo pool_info = {
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer |
                 vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    };
    command_pool = device.createCommandPool(pool_info);

    const vk::CommandBufferAllocateInfo alloc_info = {
        .commandPool = command_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = num_images,
    };
    const std::vector command_buffers = device.allocateCommandBuffers(alloc_info);

    swap_chain.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = swap_chain[i];
        frame.cmdbuf = command_buffers[i];
        frame.render_ready = device.createSemaphore({});
        frame.present_done = device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled});
        free_queue.push(&frame);
    }

    if (instance.HasDebuggingToolAttached()) {
        for (u32 i = 0; i < num_images; ++i) {
            SetObjectName(device, swap_chain[i].cmdbuf, "Swapchain Command Buffer {}", i);
            SetObjectName(device, swap_chain[i].render_ready,
                          "Swapchain Semaphore: render_ready {}", i);
            SetObjectName(device, swap_chain[i].present_done, "Swapchain Fence: present_done {}",
                          i);
        }
    }

    if (use_present_thread) {
        present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
    }
#ifdef __SWITCH__
    hen_watch_thread = std::jthread([this](std::stop_token token) { HenWatchThread(token); });
#endif
}

#ifdef __SWITCH__
void PresentWindow::HenWatchFrame(u64 tick, unsigned long long flush_ns) {
    {
        std::scoped_lock lock{hen_watch_mutex};
        if (hen_watch_queue.size() >= 8) {
            // (the watcher is behind: the oldest goes, nobody waits)
            hen_watch_queue.pop_front();
            __atomic_fetch_add(&hen_render_latency_stats[3], 1, __ATOMIC_RELAXED);
        }
        hen_watch_queue.emplace_back(tick, flush_ns);
    }
    hen_watch_cv.notify_one();
}

void PresentWindow::HenWatchThread(std::stop_token token) {
    Common::SetCurrentThreadName("HenGpuWatch");
    const auto clock_ns = [] {
        return static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };
    MasterSemaphore* const master = scheduler.GetMasterSemaphore();
    while (!token.stop_requested()) {
        std::pair<u64, unsigned long long> entry;
        {
            std::unique_lock lock{hen_watch_mutex};
            Common::CondvarWait(hen_watch_cv, lock, token, [this] { return !hen_watch_queue.empty(); });
            if (token.stop_requested()) {
                return;
            }
            entry = hen_watch_queue.front();
            hen_watch_queue.pop_front();
        }
        // (no lock from here on)
        bool reached = true;
        while (!master->IsFree(entry.first)) {
            master->Refresh();
            if (master->IsFree(entry.first)) {
                break;
            }
            if (token.stop_requested()) {
                return;
            }
            if (clock_ns() - entry.second > 100'000'000ull) {
                reached = false; // (a tenth of a second: counted, and on to the next)
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const unsigned long long done_ns = clock_ns();
        const long long ns = done_ns > entry.second ? static_cast<long long>(done_ns - entry.second) : 0;
        long long mean = static_cast<long long>(__atomic_load_n(&hen_render_latency_mean_ns, __ATOMIC_RELAXED));
        mean += (ns - mean) / 16;
        __atomic_store_n(&hen_render_latency_mean_ns, static_cast<unsigned long long>(mean), __ATOMIC_RELAXED);
        const int by_render = __atomic_load_n(&hen_headroom_by_render, __ATOMIC_RELAXED);
        if (by_render && mean >= hen_headroom_hi_us * 1000ll) {
            __atomic_store_n(&hen_headroom_by_render, 0, __ATOMIC_RELAXED);
        } else if (!by_render && mean <= hen_headroom_lo_us * 1000ll) {
            __atomic_store_n(&hen_headroom_by_render, 1, __ATOMIC_RELAXED);
        }
        __atomic_fetch_add(&hen_render_latency_stats[0], 1, __ATOMIC_RELAXED);
        if (static_cast<unsigned long long>(ns) > __atomic_load_n(&hen_render_latency_gap_max_ns, __ATOMIC_RELAXED)) {
            __atomic_store_n(&hen_render_latency_gap_max_ns, static_cast<unsigned long long>(ns), __ATOMIC_RELAXED);
        }
        __atomic_fetch_add(&hen_render_latency_stats[1], static_cast<unsigned long long>(ns), __ATOMIC_RELAXED);
        if (static_cast<unsigned long long>(ns) > __atomic_load_n(&hen_render_latency_stats[2], __ATOMIC_RELAXED)) {
            __atomic_store_n(&hen_render_latency_stats[2], static_cast<unsigned long long>(ns), __ATOMIC_RELAXED);
        }
        if (!reached) {
            __atomic_fetch_add(&hen_render_latency_stats[3], 1, __ATOMIC_RELAXED);
        }
    }
}
#endif

PresentWindow::~PresentWindow() {
    scheduler.Finish();
    const vk::Device device = instance.GetDevice();
    // If the window is destroyed before the next_surface is
    // consumed, make sure to destroy it here to prevent a
    // resource leak.
    if (next_surface && next_surface != surface) {
        instance.GetInstance().destroySurfaceKHR(next_surface);
        next_surface = vk::SurfaceKHR{};
    }
    device.destroyCommandPool(command_pool);
    device.destroyRenderPass(present_renderpass);
    for (auto& frame : swap_chain) {
        device.destroyImageView(frame.image_view);
        device.destroyFramebuffer(frame.framebuffer);
        device.destroySemaphore(frame.render_ready);
        device.destroyFence(frame.present_done);
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
    }
}

void PresentWindow::RecreateFrame(Frame* frame, u32 width, u32 height) {
    vk::Device device = instance.GetDevice();
    if (frame->framebuffer) {
        device.destroyFramebuffer(frame->framebuffer);
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    frame->image_view = device.createImageView(view_info);

    const vk::FramebufferCreateInfo framebuffer_info = {
        .renderPass = present_renderpass,
        .attachmentCount = 1,
        .pAttachments = &frame->image_view,
        .width = width,
        .height = height,
        .layers = 1,
    };
    frame->framebuffer = instance.GetDevice().createFramebuffer(framebuffer_info);

    frame->width = width;
    frame->height = height;
}

Frame* PresentWindow::GetRenderFrame() {
    MICROPROFILE_SCOPE(Vulkan_WaitPresent);

    HEN_SWAP_START();
#ifdef __SWITCH__
    const auto hen_frame_start = std::chrono::steady_clock::now();
#endif
    // Wait for free presentation frames
    std::unique_lock lock{free_mutex};
    free_cv.wait(lock, [this] { return !free_queue.empty(); });
    HEN_SWAP_ADD(1);
    HEN_SWAP_COUNT(0);

    // Take the frame from the queue
    Frame* frame = free_queue.front();
    free_queue.pop();

    vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };
#ifdef __SWITCH__
    const auto hen_wait_start = std::chrono::steady_clock::now();
#endif

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }

        // eErrorInitializationFailed occurs on Mali GPU drivers due to them
        // using the ppoll() syscall which isn't correctly restarted after a signal,
        // we need to manually retry waiting in that case
        if (result == vk::Result::eErrorInitializationFailed) {
            continue;
        }
    }

    HEN_SWAP_ADD(2);
#ifdef __SWITCH__
    __atomic_fetch_add(&hen_render_wait_ns,
                       static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                           std::chrono::steady_clock::now() - hen_frame_start)
                                                           .count()),
                       __ATOMIC_RELAXED);
#endif
#ifdef __SWITCH__
    // Whether the GPU has time to spare, for the rasterizer's choice of
    // the generic fragment shader (vk_rasterizer.cpp, Draw). This wait is for the GPU to
    // finish what was queued before this frame object's last use: 0.0 ms a frame while the GPU
    // keeps up (docked: every interval of five titles, 2.8 at most), 4 to 14 ms a frame where
    // it is the bottleneck (handheld: Xenoblade 9-13 for a minute, Kid Icarus 3-11, Pokemon
    // OR 11-14 for 15 s; docked Xenoblade 6-10 in its heaviest ten seconds). A running mean
    // over about sixteen frames; no headroom from 4 ms, headroom again from 2 ms down.
    {
        const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - hen_wait_start)
                                 .count();
        long long mean = static_cast<long long>(__atomic_load_n(&hen_present_wait_mean_ns, __ATOMIC_RELAXED));
        mean += (ns - mean) / 16;
        __atomic_store_n(&hen_present_wait_mean_ns, static_cast<unsigned long long>(mean), __ATOMIC_RELAXED);
        // This measure's own verdict, and the one that rules (the render time's, kept by the
        // watcher thread, unless --headroom-fence: then this one).
        const int by_fence = __atomic_load_n(&hen_headroom_by_fence, __ATOMIC_RELAXED);
        if (by_fence && mean >= 4'000'000) {
            __atomic_store_n(&hen_headroom_by_fence, 0, __ATOMIC_RELAXED);
        } else if (!by_fence && mean <= 2'000'000) {
            __atomic_store_n(&hen_headroom_by_fence, 1, __ATOMIC_RELAXED);
        }
        const int ruling = hen_headroom_render ? __atomic_load_n(&hen_headroom_by_render, __ATOMIC_RELAXED)
                                               : __atomic_load_n(&hen_headroom_by_fence, __ATOMIC_RELAXED);
        if (ruling != __atomic_load_n(&hen_gpu_headroom, __ATOMIC_RELAXED)) {
            __atomic_store_n(&hen_gpu_headroom, ruling, __ATOMIC_RELAXED);
#if HEN_DEV
            hen_gpu_headroom_flips++;
#endif
        }
#if HEN_DEV
        if (!ruling) {
            hen_gpu_headroom_frames_without++;
        }
#endif
    }
#endif
    device.resetFences(frame->present_done);
    return frame;
}

void PresentWindow::Present(Frame* frame) {
#if defined(__SWITCH__) && HEN_DEV
    __atomic_fetch_add(&hen_shown_hist[10], 1, __ATOMIC_RELAXED); // (frames handed over for showing)
#endif
    if (!use_present_thread) {
        scheduler.WaitWorker();
        CopyToSwapchain(frame);
        free_queue.push(frame);
        return;
    }

    scheduler.Record([this, frame](vk::CommandBuffer) {
        std::unique_lock lock{queue_mutex};
        present_queue.push(frame);
        frame_cv.notify_one();
    });
}

void PresentWindow::WaitPresent() {
    if (!use_present_thread) {
        return;
    }

    // Wait for the present queue to be empty
    {
        std::unique_lock queue_lock{queue_mutex};
        frame_cv.wait(queue_lock, [this] { return present_queue.empty() && !presenting; });
    }

    // The above condition will be satisfied when the last frame is taken from the queue.
    // To ensure that frame has been presented as well take hold of the swapchain
    // mutex.
    std::scoped_lock swapchain_lock{swapchain_mutex};
}

void PresentWindow::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("VulkanPresent");
    while (!token.stop_requested()) {
        std::unique_lock lock{queue_mutex};

        // Wait for presentation frames
        Common::CondvarWait(frame_cv, lock, token, [this] { return !present_queue.empty(); });
        if (token.stop_requested()) {
            return;
        }

        // Take the frame and notify anyone waiting
        Frame* frame = present_queue.front();
        present_queue.pop();
        presenting = true; // (WaitPresent waits for this too)
        frame_cv.notify_one();

        if (HenCpuAcquire()) {
            // (Kaizhar, --present-cpu-acquire) The image is acquired, and the display's
            // release of it waited for, with no lock held: neither swapchain_mutex nor the
            // scheduler's submit_mutex (a wait under the former holds every frame
            // back). Only this thread uses the swapchain while a title runs; WaitPresent's
            // callers are kept out by `presenting` until this frame is on its way.
            lock.unlock();
            AcquireImage(frame);
            lock = std::unique_lock{swapchain_mutex};
            CopyToSwapchain(frame, true);
        } else {
            // By exchanging the lock ownership we take the swapchain lock
            // before the queue lock goes out of scope. This way the swapchain
            // lock in WaitPresent is guaranteed to occur after here.
            std::exchange(lock, std::unique_lock{swapchain_mutex});

            CopyToSwapchain(frame);
        }

        // Free the frame for reuse
        {
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
        lock.unlock(); // (the swapchain lock)
        {
            std::scoped_lock ql{queue_mutex};
            presenting = false;
        }
        frame_cv.notify_all();

        // (Kaizhar) Acquiring when the frame arrives and then waiting for the display
        // to let go of the image is, with three images at 60 frames a second, a whole
        // refresh, every frame (16.5 ms x 300 in each five-second report), during which
        // the frame object is held and the next frame queued behind it: the emulation thread
        // waits 10 to 12 ms a blank for a free frame object, the automatic clock reads that as
        // work, and Xenoblade loses 326 ms where it lost 30. The same wait begun here, as
        // soon as a frame is on its way, runs while the next frame is being made.
        if (HenCpuAcquire() && !token.stop_requested()) {
            PreAcquire();
        }
    }
}

void PresentWindow::PreAcquire() {
    // (a swapchain to be made anew, or a size about to change: AcquireImage sees to it when the
    // frame is here; nothing is acquired now)
    if (pre_acquired || !swapchain.AcquireNextImage(true)) {
        return;
    }
#if defined(__SWITCH__) && HEN_DEV
    __atomic_fetch_add(&hen_present_acquire_wait[3], 1, __ATOMIC_RELAXED);
#endif
    HenWaitAcquired();
    pre_acquired = true;
}

void PresentWindow::HenWaitAcquired() {
    // (the display's release of the image: otherwise the copy waits for it in the GPU's
    // queue, and with it everything submitted after the copy)
#if defined(__SWITCH__) && HEN_DEV
    const auto wait_start = std::chrono::steady_clock::now();
    swapchain.WaitAcquired();
    const unsigned long long ns = static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             wait_start)
            .count());
    __atomic_fetch_add(&hen_present_acquire_wait[0], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&hen_present_acquire_wait[1], ns, __ATOMIC_RELAXED);
    if (ns > __atomic_load_n(&hen_present_acquire_wait[2], __ATOMIC_RELAXED)) {
        __atomic_store_n(&hen_present_acquire_wait[2], ns, __ATOMIC_RELAXED);
    }
#else
    swapchain.WaitAcquired();
#endif
}

void PresentWindow::NotifySurfaceChanged() {
#ifdef ANDROID
    std::scoped_lock lock{recreate_surface_mutex};

    // surfaceChanged() may notify us that a surface has changed
    // for the same surface multiple times. If that is the case
    // skip creating the surface again as that would cause a
    // vulkan ErrorNativeWindowInUseKHR.
    void* const render_surface = emu_window.GetWindowInfo().render_surface;
    if (render_surface == last_render_surface) {
        return;
    }
    last_render_surface = render_surface;

    // If an earlier notification produced a surface that CopyToSwapchain() has not consumed yet,
    // release it rather than just overwritting its handle and causing a leak.
    if (next_surface && next_surface != surface) {
        instance.GetInstance().destroySurfaceKHR(next_surface);
        next_surface = vk::SurfaceKHR{};
    }

    next_surface = CreateSurface(instance.GetInstance(), emu_window);
    recreate_surface_cv.notify_one();
#endif
}

void PresentWindow::AcquireImage(Frame* frame) {
    const auto recreate_swapchain = [&] {
#ifdef ANDROID
        {
            std::unique_lock lock{recreate_surface_mutex};
            recreate_surface_cv.wait(lock, [this]() { return surface != next_surface; });
            surface = next_surface;
        }
#endif
        std::scoped_lock submit_lock{scheduler.submit_mutex};
        graphics_queue.waitIdle();
        swapchain.Create(frame->width, frame->height, surface, low_refresh_rate);
    };

#ifndef ANDROID
    const bool use_vsync = Settings::values.use_vsync.GetValue();
    const bool size_changed =
        swapchain.GetWidth() != frame->width || swapchain.GetHeight() != frame->height;
    const bool vsync_changed = vsync_enabled != use_vsync;
    if (vsync_changed || size_changed) [[unlikely]] {
        vsync_enabled = use_vsync;
        recreate_swapchain();
        pre_acquired = false; // (that image went with the old swapchain)
    }
#endif

    // The image PreAcquire took after the last present, its release waited for already.
    if (pre_acquired) {
        pre_acquired = false;
        HEN_SWAP_COUNT(3);
        return;
    }

    const bool cpu_acquire = HenCpuAcquire();
    HEN_SWAP_START();
    while (!swapchain.AcquireNextImage(cpu_acquire)) {
        recreate_swapchain();
    }
    HEN_SWAP_ADD(4);
    HEN_SWAP_COUNT(3);
    if (cpu_acquire) {
        HenWaitAcquired();
    }
}

void PresentWindow::CopyToSwapchain(Frame* frame, bool acquired) {
    if (!acquired) {
        AcquireImage(frame);
    }
    const bool cpu_acquire = HenCpuAcquire();

    const vk::Image swapchain_image = swapchain.Image();

    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    const vk::CommandBuffer cmdbuf = frame->cmdbuf;
    cmdbuf.begin(begin_info);

    const vk::Extent2D extent = swapchain.GetExtent();
    const std::array pre_barriers{
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = frame->image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
    };
    const vk::ImageMemoryBarrier post_barrier{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::ePresentSrcKHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchain_image,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };

    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                           {}, {}, pre_barriers);

    if (blit_supported) {
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlit(frame->width, frame->height, extent.width, extent.height),
                         Settings::values.filter_mode.GetValue() ? vk::Filter::eLinear
                                                                 : vk::Filter::eNearest);
    } else {
        cmdbuf.copyImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageCopy(frame->width, frame->height, extent.width, extent.height));
    }

    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                           vk::PipelineStageFlagBits::eAllCommands,
                           vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

    cmdbuf.end();

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eAllGraphics,
    };

    const vk::Semaphore present_ready = swapchain.GetPresentReadySemaphore();
    const vk::Semaphore image_acquired = swapchain.GetImageAcquiredSemaphore();
    const std::array wait_semaphores = {image_acquired, frame->render_ready};

    // (--present-cpu-acquire: the image's release was waited for in AcquireImage, and
    // image_acquired was not given to the acquire: the copy waits for the frame's drawing only)
    vk::SubmitInfo submit_info = {
        .waitSemaphoreCount = cpu_acquire ? 1u : static_cast<u32>(wait_semaphores.size()),
        .pWaitSemaphores = cpu_acquire ? &wait_semaphores[1] : wait_semaphores.data(),
        .pWaitDstStageMask = cpu_acquire ? &wait_stage_masks[1] : wait_stage_masks.data(),
        .commandBufferCount = 1u,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &present_ready,
    };

    std::scoped_lock submit_lock{scheduler.submit_mutex, recreate_surface_mutex};

    HEN_SWAP_START2();
    try {
        graphics_queue.submit(submit_info, frame->present_done);
    } catch (vk::DeviceLostError& err) {
        LOG_CRITICAL(Render_Vulkan, "Device lost during present submit: {}", err.what());
        UNREACHABLE();
    }

    HEN_SWAP_ADD2(5);
    swapchain.Present();
    HEN_SWAP_ADD2(6);
#if defined(__SWITCH__) && HEN_DEV
    // How far apart images are handed to the display: the time from one
    // present's return to the next, in buckets around the refresh periods at 120 and 60 Hz.
    // Two clock reads and two additions a frame.
    {
        static unsigned long long last_ns = 0;
        const unsigned long long now_ns = HEN_SWAP_NOW();
        if (last_ns != 0 && now_ns - last_ns < 5'000'000'000ull) {
            const unsigned long long us = (now_ns - last_ns) / 1000;
            // under 6.3 | 8.3 | 16.7 | 25 | 33.3 | 41.7 | 50 | over 54.2 ms
            static constexpr unsigned long long edges[7] = {6250, 12500, 20833, 29167, 37500, 45833, 54167};
            unsigned bucket = 0;
            while (bucket < 7 && us >= edges[bucket]) {
                bucket++;
            }
            __atomic_fetch_add(&hen_shown_hist[bucket], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&hen_shown_hist[8], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&hen_shown_hist[9], us, __ATOMIC_RELAXED);
        }
        last_ns = now_ns;
    }
#endif
}

vk::RenderPass PresentWindow::CreateRenderpass() {
    const vk::AttachmentReference color_ref = {
        .attachment = 0,
        .layout = vk::ImageLayout::eGeneral,
    };

    const vk::SubpassDescription subpass = {
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &color_ref,
        .pResolveAttachments = 0,
        .pDepthStencilAttachment = nullptr,
    };

    const vk::AttachmentDescription color_attachment = {
        .format = swapchain.GetSurfaceFormat().format,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::eTransferSrcOptimal,
    };

    const vk::RenderPassCreateInfo renderpass_info = {
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = nullptr,
    };

    return instance.GetDevice().createRenderPass(renderpass_info);
}

} // namespace Vulkan
