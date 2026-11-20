// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <queue>
#include "common/polyfill_thread.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace Frontend {
class EmuWindow;
}

namespace Vulkan {

class Instance;
class Swapchain;
class Scheduler;
class RenderManager;

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Framebuffer framebuffer;
    vk::Image image;
    vk::ImageView image_view;
    vk::Semaphore render_ready;
    vk::Fence present_done;
    vk::CommandBuffer cmdbuf;
};

class PresentWindow final {
public:
#ifdef __SWITCH__
    /// (Kaizhar) A frame's drawing was flushed just now with this scheduler tick: the
    /// watcher thread times how long the GPU takes to reach it. Never waits.
    void HenWatchFrame(u64 tick, unsigned long long flush_ns);
#endif
    explicit PresentWindow(Frontend::EmuWindow& emu_window, const Instance& instance,
                           Scheduler& scheduler, bool low_refresh_rate);
    ~PresentWindow();

    /// Waits for all queued frames to finish presenting.
    void WaitPresent();

    /// Returns the last used render frame.
    Frame* GetRenderFrame();

    /// Recreates the render frame to match provided parameters.
    void RecreateFrame(Frame* frame, u32 width, u32 height);

    /// Queues the provided frame for presentation.
    void Present(Frame* frame);

    /// This is called to notify the rendering backend of a surface change
    void NotifySurfaceChanged();

    [[nodiscard]] vk::RenderPass Renderpass() const noexcept {
        return present_renderpass;
    }

    u32 ImageCount() const noexcept {
        return swapchain.GetImageCount();
    }

    vk::Format GetSurfaceFormat() const noexcept {
        return swapchain.GetSurfaceFormat().format;
    }

private:
    void PresentThread(std::stop_token token);
#ifdef __SWITCH__
    void HenWatchThread(std::stop_token token);
#endif

    /// acquired: AcquireImage(frame) has been called for this frame already.
    void CopyToSwapchain(Frame* frame, bool acquired = false);
    /// (Kaizhar) The first part of CopyToSwapchain: the swapchain brought to the frame's
    /// size and its next image acquired. With --present-cpu-acquire it also waits, on this
    /// thread, until the display has let go of that image.
    void AcquireImage(Frame* frame);
    /// (Kaizhar, --present-cpu-acquire) Right after a present: the next frame's image is
    /// acquired and the display's release of it waited for now, while that frame is still being
    /// made. Present thread only, no lock held.
    void PreAcquire();
    /// The acquire and the timed wait of AcquireImage and PreAcquire.
    void HenWaitAcquired();

    vk::RenderPass CreateRenderpass();

private:
    Frontend::EmuWindow& emu_window;
    const Instance& instance;
    Scheduler& scheduler;
    bool low_refresh_rate;
    vk::SurfaceKHR surface;
    vk::SurfaceKHR next_surface{};
    Swapchain swapchain;
    vk::CommandPool command_pool;
    vk::Queue graphics_queue;
    vk::RenderPass present_renderpass;
    std::vector<Frame> swap_chain;
    std::queue<Frame*> free_queue;
    std::queue<Frame*> present_queue;
    std::condition_variable free_cv;
    std::condition_variable recreate_surface_cv;
    std::condition_variable_any frame_cv;
    std::mutex swapchain_mutex;
    std::mutex recreate_surface_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::jthread present_thread;
    /// (Kaizhar, under queue_mutex) The present thread has taken a frame from the queue
    /// and has not finished presenting it: what WaitPresent waits for when the thread does not
    /// hold swapchain_mutex all the way (--present-cpu-acquire).
    bool presenting{false};
    /// (present thread only) PreAcquire's image is waiting for its frame.
    bool pre_acquired{false};
#ifdef __SWITCH__
    // Frames whose drawing the watcher has yet to see finished: tick and flush time.
    std::mutex hen_watch_mutex;
    std::condition_variable_any hen_watch_cv;
    std::deque<std::pair<u64, unsigned long long>> hen_watch_queue;
    std::jthread hen_watch_thread; // (last: it ends first)
#endif
    bool vsync_enabled{};
    bool blit_supported;
    bool use_present_thread{true};
    void* last_render_surface{};
};

} // namespace Vulkan
