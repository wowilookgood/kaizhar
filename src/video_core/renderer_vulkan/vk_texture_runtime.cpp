// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/hen_frame_work.h"
#include "common/hen_gap.h"
#include "common/hen_stats.h"
#include "video_core/renderer_vulkan/vk_texture_runtime.h"

#include <limits>
#include <span>
#include <string>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <vulkan/vulkan.hpp>
#include "video_core/custom_textures/custom_tex_manager.h"
#include "video_core/rasterizer_cache/pixel_format.h"
#include "video_core/rasterizer_cache/surface_params.h"
#include "video_core/renderer_vulkan/vk_blit_helper.h"
#include "video_core/renderer_vulkan/vk_descriptor_update_queue.h"
#include "video_core/renderer_vulkan/vk_stream_buffer.h"

#include "common/literals.h"
#include "common/microprofile.h"
#include "common/scope_exit.h"
#include "video_core/custom_textures/material.h"
#include "video_core/rasterizer_cache/texture_codec.h"
#include "video_core/rasterizer_cache/utils.h"
#include "video_core/renderer_vulkan/pica_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_render_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_texture_runtime.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_format_traits.hpp>

MICROPROFILE_DEFINE(Vulkan_ImageAlloc, "Vulkan", "Texture Allocation", MP_RGB(192, 52, 235));

#ifdef __SWITCH__
extern "C" int hen_depth_scale_blit; // --no-depth-scale-blit
#else
static constexpr int hen_depth_scale_blit = 1;
#endif

#ifdef __SWITCH__
extern "C" void hen_backtrace(const char* what); // hen/jit/hen_jit.c
extern "C" int hen_depth_lag;                     // --sync-depth-reads turns it off
#endif

#ifdef __SWITCH__
namespace Vulkan {
void HenNoteImageMemory(VmaAllocator allocator, VmaAllocation allocation); // vk_instance.cpp
}
#endif

#if defined(__SWITCH__) && HEN_DEV
#include <atomic>
extern "C" unsigned long long hen_jit_now_ns(void);
extern std::atomic<unsigned long long> hen_last_submit_ns; // vk_master_semaphore.cpp
#endif
namespace {
/// A surface download's wait: everything queued is recorded and submitted by the scheduler's
/// worker, then the GPU finishes it. Development builds say how a slow one divides between
/// the two and print the three threads' samples (first ones take 24 to 60 ms on cold routes
/// where later ones take 0.4 to 13).
void HenDownloadFinish(Vulkan::Scheduler& scheduler) {
#if defined(__SWITCH__) && HEN_DEV
    const unsigned long long start = hen_jit_now_ns();
    scheduler.Finish();
    const unsigned long long end = hen_jit_now_ns();
    if (end - start >= 20'000'000ull) {
        const unsigned long long submitted = hen_last_submit_ns.load(std::memory_order_relaxed);
        if (submitted >= start && submitted <= end) {
            std::printf("hen: slow: %llu ms in a surface download's wait: %llu until the scheduler's "
                        "worker had submitted, %llu for the GPU after that\n",
                        (end - start) / 1'000'000, (submitted - start) / 1'000'000,
                        (end - submitted) / 1'000'000);
        } else {
            std::printf("hen: slow: %llu ms in a surface download's wait (nothing was submitted in it)\n",
                        (end - start) / 1'000'000);
        }
        hen_blackbox_dump(static_cast<unsigned>((end - start) / 1'000'000));
    }
#else
    scheduler.Finish();
#endif
}
} // namespace

namespace Vulkan {

namespace {

using VideoCore::MapType;
using VideoCore::PixelFormat;
using VideoCore::SurfaceType;
using VideoCore::TextureType;
using namespace Common::Literals;

struct RecordParams {
    vk::ImageAspectFlags aspect;
    vk::Filter filter;
    vk::PipelineStageFlags pipeline_flags;
    vk::AccessFlags src_access;
    vk::AccessFlags dst_access;
    vk::Image src_image;
    vk::Image dst_image;
};

vk::Filter MakeFilter(VideoCore::PixelFormat pixel_format) {
    switch (pixel_format) {
    case VideoCore::PixelFormat::D16:
    case VideoCore::PixelFormat::D24:
    case VideoCore::PixelFormat::D24S8:
        return vk::Filter::eNearest;
    default:
        return vk::Filter::eLinear;
    }
}

[[nodiscard]] vk::ClearValue MakeClearValue(VideoCore::ClearValue clear) {
    static_assert(sizeof(VideoCore::ClearValue) == sizeof(vk::ClearValue));

    vk::ClearValue value{};
    std::memcpy(&value, &clear, sizeof(vk::ClearValue));
    return value;
}

[[nodiscard]] vk::ClearColorValue MakeClearColorValue(Common::Vec4f color) {
    return vk::ClearColorValue{
        .float32 = std::array{color[0], color[1], color[2], color[3]},
    };
}

[[nodiscard]] vk::ClearDepthStencilValue MakeClearDepthStencilValue(VideoCore::ClearValue clear) {
    return vk::ClearDepthStencilValue{
        .depth = clear.depth,
        .stencil = clear.stencil,
    };
}

u32 UnpackDepthStencil(const VideoCore::StagingData& data, vk::Format dest) {
    u32 depth_offset = 0;
    u32 stencil_offset = 4 * data.size / 5;
    const auto& mapped = data.mapped;

    switch (dest) {
    case vk::Format::eD24UnormS8Uint: {
        for (; stencil_offset < data.size; depth_offset += 4) {
            u8* ptr = mapped.data() + depth_offset;
            const u32 d24s8 = VideoCore::MakeInt<u32>(ptr);
            const u32 d24 = d24s8 >> 8;
            mapped[stencil_offset] = d24s8 & 0xFF;
            std::memcpy(ptr, &d24, 4);
            stencil_offset++;
        }
        break;
    }
    case vk::Format::eD32SfloatS8Uint: {
        for (; stencil_offset < data.size; depth_offset += 4) {
            u8* ptr = mapped.data() + depth_offset;
            const u32 d24s8 = VideoCore::MakeInt<u32>(ptr);
            const float d32 = (d24s8 >> 8) / 16777215.f;
            mapped[stencil_offset] = d24s8 & 0xFF;
            std::memcpy(ptr, &d32, 4);
            stencil_offset++;
        }
        break;
    }
    default:
        LOG_ERROR(Render_Vulkan, "Unimplemented convertion for depth format {}",
                  vk::to_string(dest));
        UNREACHABLE();
    }

    ASSERT(depth_offset == 4 * data.size / 5);
    return depth_offset;
}

void MakeInitBarriers(vk::ImageAspectFlags aspect, u32 num_images,
                      std::span<const vk::Image> images,
                      std::span<vk::ImageMemoryBarrier> out_barriers) {
    for (u32 i = 0; i < num_images; i++) {
        out_barriers[i] = vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i],
            .subresourceRange{
                .aspectMask = aspect,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
    }
}

vk::ImageSubresourceRange MakeSubresourceRange(vk::ImageAspectFlags aspect, u32 level = 0,
                                               u32 levels = 1, u32 layer = 0) {
    return vk::ImageSubresourceRange{
        .aspectMask = aspect,
        .baseMipLevel = level,
        .levelCount = levels,
        .baseArrayLayer = layer,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };
}

constexpr u64 UPLOAD_BUFFER_SIZE = 512_MiB;
constexpr u64 DOWNLOAD_BUFFER_SIZE = 16_MiB;

} // Anonymous namespace

void Handle::Create(u32 width, u32 height, u32 levels, TextureType type, vk::Format format,
                    vk::ImageUsageFlags usage, vk::ImageCreateFlags flags,
                    vk::ImageAspectFlags aspect, bool need_format_list,
                    std::string_view debug_name) {

    Destroy();

    const bool is_cube_map = type == TextureType::CubeMap && instance.IsLayeredRenderingSupported();
    if (!is_cube_map) {
        flags &= ~vk::ImageCreateFlagBits::eCubeCompatible;
    }

    this->width = width;
    this->height = height;
    this->levels = levels;
    this->layers = is_cube_map ? 6 : 1;

    const std::array format_list = {
        vk::Format::eR8G8B8A8Unorm,
        vk::Format::eR32Uint,
    };
    const vk::ImageFormatListCreateInfo image_format_list = {
        .viewFormatCount = static_cast<u32>(format_list.size()),
        .pViewFormats = format_list.data(),
    };

    const vk::ImageCreateInfo image_info = {
        .pNext = need_format_list ? &image_format_list : nullptr,
        .flags = flags,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = levels,
        .arrayLayers = layers,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = usage,
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
                                     &unsafe_image, &allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating image with error {}", result);
        UNREACHABLE();
    }
#ifdef __SWITCH__
    HenNoteImageMemory(instance.GetAllocator(), allocation); // (vk_instance.cpp: the report)
#endif

    image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = image,
        .viewType = is_cube_map ? vk::ImageViewType::eCube : vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };
    image_views[ViewType::Sample] = instance.GetDevice().createImageView(view_info);
    if (levels == 1) {
        image_views[ViewType::Mip0] = image_views[ViewType::Mip0];
    }

    if (!debug_name.empty() && instance.HasDebuggingToolAttached()) {
        SetObjectName(instance.GetDevice(), image, debug_name);
        SetObjectName(instance.GetDevice(), image_views[ViewType::Sample], "{} View({})",
                      debug_name, vk::to_string(aspect));
    }
}

void Handle::Destroy() {
    const auto device = instance.GetDevice();

    // Image views
    if (auto view = image_views[ViewType::Sample]) {
        device.destroyImageView(view);
    }
    if (auto view = image_views[ViewType::Mip0]; view && view != image_views[ViewType::Sample]) {
        device.destroyImageView(view);
    }
    if (auto view = image_views[ViewType::Storage]) {
        device.destroyImageView(view);
    }
    if (auto view = image_views[ViewType::Depth]) {
        device.destroyImageView(view);
    }
    if (auto view = image_views[ViewType::Stencil]) {
        device.destroyImageView(view);
    }

    image_views = {};

    if (framebuffer) {
        device.destroyFramebuffer(framebuffer);
        framebuffer = VK_NULL_HANDLE;
    }

    if (allocation) {
        vmaDestroyImage(instance.GetAllocator(), image, allocation);
    }

    image = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
}

TextureRuntime::TextureRuntime(const Instance& instance, Scheduler& scheduler,
                               RenderManager& renderpass_cache, DescriptorUpdateQueue& update_queue,
                               u32 num_swapchain_images_)
    : instance{instance}, scheduler{scheduler}, renderpass_cache{renderpass_cache},
      blit_helper{instance, scheduler, renderpass_cache, update_queue},
      upload_buffer{instance, scheduler, vk::BufferUsageFlagBits::eTransferSrc, UPLOAD_BUFFER_SIZE,
                    BufferType::Upload},
      download_buffer{instance, scheduler,
                      vk::BufferUsageFlagBits::eTransferDst |
                          vk::BufferUsageFlagBits::eStorageBuffer,
                      DOWNLOAD_BUFFER_SIZE, BufferType::Download},
      depth_lag_buffer{instance, scheduler,
                       vk::BufferUsageFlagBits::eTransferDst |
                           vk::BufferUsageFlagBits::eStorageBuffer,
                       16 * 1024 * 1024, BufferType::Download},
      num_swapchain_images{num_swapchain_images_} {}

TextureRuntime::~TextureRuntime() = default;

VideoCore::StagingData TextureRuntime::FindStaging(u32 size, bool upload) {
    StreamBuffer& buffer = upload ? upload_buffer : download_buffer;
    const auto [data, offset, invalidate] = buffer.Map(size, 16);
    return VideoCore::StagingData{
        .size = size,
        .offset = offset,
        .mapped = std::span{data, size},
    };
}

u64 TextureRuntime::GetResourceTick() {
    return scheduler.GetMasterSemaphore()->CurrentTick();
}

u64 TextureRuntime::GetResourceFreeTick() {
    // Ensure we are getting the latest GpuTick value to reduce garbage-collection latency and
    // allows the deletion of resources immediately when they are done being used.
    scheduler.GetMasterSemaphore()->Refresh();
    return scheduler.GetMasterSemaphore()->KnownGpuTick();
}

void TextureRuntime::Finish() {
    scheduler.Flush();
}

bool TextureRuntime::Reinterpret(Surface& source, Surface& dest,
                                 const VideoCore::TextureCopy& copy) {
    const PixelFormat src_format = source.pixel_format;
    const PixelFormat dst_format = dest.pixel_format;
    ASSERT_MSG(src_format != dst_format, "Reinterpretation with the same format is invalid");

    if (!source.traits.needs_conversion && !dest.traits.needs_conversion &&
        source.type == dest.type) {
        CopyTextures(source, dest, copy);
        return true;
    }

    if (src_format == PixelFormat::D24S8 && dst_format == PixelFormat::RGBA8) {
        blit_helper.ConvertDS24S8ToRGBA8(source, dest, copy);
    } else {
        LOG_WARNING(Render_Vulkan, "Unimplemented reinterpretation {} -> {}",
                    VideoCore::PixelFormatAsString(src_format),
                    VideoCore::PixelFormatAsString(dst_format));
        return false;
    }
    return true;
}

bool TextureRuntime::ClearTexture(Surface& surface, const VideoCore::TextureClear& clear) {
    renderpass_cache.EndRendering();

    const RecordParams params = {
        .aspect = surface.Aspect(),
        .pipeline_flags = surface.PipelineStageFlags(),
        .src_access = surface.AccessFlags(),
        .src_image = surface.Image(),
    };

    if (clear.texture_rect == surface.GetScaledRect()) {
        HEN_WORK_COUNT(HEN_WORK_CLEARS_WHOLE, 1);
        scheduler.Record([params, clear](vk::CommandBuffer cmdbuf) {
            const vk::ImageSubresourceRange range = {
                .aspectMask = params.aspect,
                .baseMipLevel = clear.texture_level,
                .levelCount = 1,
                .baseArrayLayer = clear.texture_layer,
                .layerCount = 1,
            };

            const vk::ImageMemoryBarrier pre_barrier = {
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = range,
            };

            const vk::ImageMemoryBarrier post_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.src_access,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = range,
            };

            cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

            const bool is_color =
                static_cast<bool>(params.aspect & vk::ImageAspectFlagBits::eColor);
            if (is_color) {
                cmdbuf.clearColorImage(params.src_image, vk::ImageLayout::eTransferDstOptimal,
                                       MakeClearColorValue(clear.value.color), range);
            } else {
                cmdbuf.clearDepthStencilImage(params.src_image,
                                              vk::ImageLayout::eTransferDstOptimal,
                                              MakeClearDepthStencilValue(clear.value), range);
            }

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);
        });
        return true;
    }

    HEN_WORK_COUNT(HEN_WORK_CLEARS_PART, 1);
    ClearTextureWithRenderpass(surface, clear);
    return true;
}

void TextureRuntime::ClearTextureWithRenderpass(Surface& surface,
                                                const VideoCore::TextureClear& clear) {
    const bool is_color = surface.type != VideoCore::SurfaceType::Depth &&
                          surface.type != VideoCore::SurfaceType::DepthStencil;

    const auto color_format = is_color ? surface.pixel_format : PixelFormat::Invalid;
    const auto depth_format = is_color ? PixelFormat::Invalid : surface.pixel_format;
    const auto render_pass = renderpass_cache.GetRenderpass(color_format, depth_format, true);

    const RecordParams params = {
        .aspect = surface.Aspect(),
        .pipeline_flags = surface.PipelineStageFlags(),
        .src_access = surface.AccessFlags(),
        .src_image = surface.Image(),
    };

    scheduler.Record([params, is_color, clear, render_pass,
                      framebuffer = surface.Framebuffer()](vk::CommandBuffer cmdbuf) {
        const vk::AccessFlags access_flag =
            is_color ? vk::AccessFlagBits::eColorAttachmentRead |
                           vk::AccessFlagBits::eColorAttachmentWrite
                     : vk::AccessFlagBits::eDepthStencilAttachmentRead |
                           vk::AccessFlagBits::eDepthStencilAttachmentWrite;

        const vk::PipelineStageFlags pipeline_flags =
            is_color ? vk::PipelineStageFlagBits::eColorAttachmentOutput
                     : vk::PipelineStageFlagBits::eEarlyFragmentTests;

        const vk::ImageMemoryBarrier pre_barrier = {
            .srcAccessMask = params.src_access,
            .dstAccessMask = access_flag,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = params.src_image,
            .subresourceRange =
                MakeSubresourceRange(params.aspect, clear.texture_level, 1, clear.texture_layer),
        };

        const vk::ImageMemoryBarrier post_barrier = {
            .srcAccessMask = access_flag,
            .dstAccessMask = params.src_access,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = params.src_image,
            .subresourceRange = MakeSubresourceRange(params.aspect, clear.texture_level),
        };

        const vk::Rect2D render_area = {
            .offset{
                .x = static_cast<s32>(clear.texture_rect.left),
                .y = static_cast<s32>(clear.texture_rect.bottom),
            },
            .extent{
                .width = clear.texture_rect.GetWidth(),
                .height = clear.texture_rect.GetHeight(),
            },
        };

        const auto clear_value = MakeClearValue(clear.value);

        const vk::RenderPassBeginInfo renderpass_begin_info = {
            .renderPass = render_pass,
            .framebuffer = framebuffer,
            .renderArea = render_area,
            .clearValueCount = 1,
            .pClearValues = &clear_value,
        };

        cmdbuf.pipelineBarrier(params.pipeline_flags, pipeline_flags,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

        cmdbuf.beginRenderPass(renderpass_begin_info, vk::SubpassContents::eInline);
        cmdbuf.endRenderPass();

        cmdbuf.pipelineBarrier(pipeline_flags, params.pipeline_flags,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);
    });
}

bool TextureRuntime::CopyTextures(Surface& source, Surface& dest,
                                  std::span<const VideoCore::TextureCopy> copies) {
    HEN_WORK_COUNT(HEN_WORK_COPIES, 1);
    renderpass_cache.EndRendering();

    const RecordParams params = {
        .aspect = source.Aspect(),
        .filter = MakeFilter(source.pixel_format),
        .pipeline_flags = source.PipelineStageFlags() | dest.PipelineStageFlags(),
        .src_access = source.AccessFlags(),
        .dst_access = dest.AccessFlags(),
        .src_image = source.Image(),
        .dst_image = dest.Image(),
    };

    boost::container::small_vector<vk::ImageCopy, 2> vk_copies;
    std::ranges::transform(copies, std::back_inserter(vk_copies), [&](const auto& copy) {
        return vk::ImageCopy{
            .srcSubresource{
                .aspectMask = params.aspect,
                .mipLevel = copy.src_level,
                .baseArrayLayer = copy.src_layer,
                .layerCount = 1,
            },
            .srcOffset = {static_cast<s32>(copy.src_offset.x), static_cast<s32>(copy.src_offset.y),
                          0},
            .dstSubresource{
                .aspectMask = params.aspect,
                .mipLevel = copy.dst_level,
                .baseArrayLayer = copy.dst_layer,
                .layerCount = 1,
            },
            .dstOffset = {static_cast<s32>(copy.dst_offset.x), static_cast<s32>(copy.dst_offset.y),
                          0},
            .extent = {copy.extent.width, copy.extent.height, 1},
        };
    });

    scheduler.Record([params, copies = std::move(vk_copies)](vk::CommandBuffer cmdbuf) {
        const bool self_copy = params.src_image == params.dst_image;
        const vk::ImageLayout new_src_layout =
            self_copy ? vk::ImageLayout::eGeneral : vk::ImageLayout::eTransferSrcOptimal;
        const vk::ImageLayout new_dst_layout =
            self_copy ? vk::ImageLayout::eGeneral : vk::ImageLayout::eTransferDstOptimal;

        const std::array pre_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = new_src_layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, 0, VK_REMAINING_MIP_LEVELS),
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.dst_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = new_dst_layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, 0, VK_REMAINING_MIP_LEVELS),
            },
        };
        const std::array post_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = new_src_layout,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, 0, VK_REMAINING_MIP_LEVELS),
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.dst_access,
                .oldLayout = new_dst_layout,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, 0, VK_REMAINING_MIP_LEVELS),
            },
        };

        cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

        cmdbuf.copyImage(params.src_image, new_src_layout, params.dst_image, new_dst_layout,
                         copies);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
    });

    return true;
}

bool TextureRuntime::BlitTextures(Surface& source, Surface& dest,
                                  const VideoCore::TextureBlit& blit) {
    HEN_WORK_COUNT(HEN_WORK_BLITS, 1);
    const bool is_depth_stencil = source.type == VideoCore::SurfaceType::DepthStencil;
    const auto& depth_traits = instance.GetTraits(source.pixel_format);
    if (is_depth_stencil && !depth_traits.blit_support) {
        return blit_helper.BlitDepthStencil(source, dest, blit);
    }

    renderpass_cache.EndRendering();

    const RecordParams params = {
        .aspect = source.Aspect(),
        .filter = MakeFilter(source.pixel_format),
        .pipeline_flags = source.PipelineStageFlags() | dest.PipelineStageFlags(),
        .src_access = source.AccessFlags(),
        .dst_access = dest.AccessFlags(),
        .src_image = source.Image(),
        .dst_image = dest.Image(),
    };

    scheduler.Record([params, blit](vk::CommandBuffer cmdbuf) {
        const std::array source_offsets = {
            vk::Offset3D{static_cast<s32>(blit.src_rect.left),
                         static_cast<s32>(blit.src_rect.bottom), 0},
            vk::Offset3D{static_cast<s32>(blit.src_rect.right), static_cast<s32>(blit.src_rect.top),
                         1},
        };

        const std::array dest_offsets = {
            vk::Offset3D{static_cast<s32>(blit.dst_rect.left),
                         static_cast<s32>(blit.dst_rect.bottom), 0},
            vk::Offset3D{static_cast<s32>(blit.dst_rect.right), static_cast<s32>(blit.dst_rect.top),
                         1},
        };

        const vk::ImageBlit blit_area = {
            .srcSubresource{
                .aspectMask = params.aspect,
                .mipLevel = blit.src_level,
                .baseArrayLayer = blit.src_layer,
                .layerCount = 1,
            },
            .srcOffsets = source_offsets,
            .dstSubresource{
                .aspectMask = params.aspect,
                .mipLevel = blit.dst_level,
                .baseArrayLayer = blit.dst_layer,
                .layerCount = 1,
            },
            .dstOffsets = dest_offsets,
        };

        const std::array read_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, blit.src_level),
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.dst_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, blit.dst_level),
            },
        };
        const std::array write_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = params.src_access,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, blit.src_level),
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.dst_access,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, blit.dst_level),
            },
        };

        cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, read_barriers);

        cmdbuf.blitImage(params.src_image, vk::ImageLayout::eTransferSrcOptimal, params.dst_image,
                         vk::ImageLayout::eTransferDstOptimal, blit_area, params.filter);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                               vk::DependencyFlagBits::eByRegion, {}, {}, write_barriers);
    });

    return true;
}

void TextureRuntime::GenerateMipmaps(Surface& surface) {
    if (VideoCore::IsCustomFormatCompressed(surface.custom_format)) {
        LOG_ERROR(Render_Vulkan, "Generating mipmaps for compressed formats unsupported!");
        return;
    }

    renderpass_cache.EndRendering();

    auto [width, height] = surface.RealExtent();
    const u32 levels = surface.levels;
    for (u32 i = 1; i < levels; i++) {
        const Common::Rectangle<u32> src_rect{0, height, width, 0};
        width = width > 1 ? width >> 1 : 1;
        height = height > 1 ? height >> 1 : 1;
        const Common::Rectangle<u32> dst_rect{0, height, width, 0};

        const VideoCore::TextureBlit blit = {
            .src_level = i - 1,
            .dst_level = i,
            .src_rect = src_rect,
            .dst_rect = dst_rect,
        };
        BlitTextures(surface, surface, blit);
    }
}

bool TextureRuntime::NeedsConversion(const Surface& surface) const {
    const FormatTraits& traits = surface.traits;
    return traits.needs_conversion &&
           // DepthStencil formats are handled elsewhere due to de-interleaving.
           traits.aspect != (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil);
}

Surface::Surface(TextureRuntime& runtime_, const VideoCore::SurfaceParams& params,
                 const VideoCore::SurfaceFlagBits& initial_flag_bits)
    : SurfaceBase{params, initial_flag_bits}, runtime{runtime_}, instance{runtime_.GetInstance()},
      scheduler{runtime_.GetScheduler()}, traits{instance.GetTraits(pixel_format)},
      handles{Handle(instance), Handle(instance), Handle(instance), Handle(instance)} {

    if (pixel_format == VideoCore::PixelFormat::Invalid || !traits.transfer_support) {
        return;
    }

    bool is_mutable = traits.native == vk::Format::eR8G8B8A8Unorm;

    if (True(flags & VideoCore::SurfaceFlagBits::ShadowSource) &&
        traits.native != vk::Format::eR8G8B8A8Unorm) {
        // If the surface is a shadow source, it needs conversion
        // to be forced as it always has to be RGBA8
        traits = instance.GetTraits(VideoCore::PixelFormat::RGBA8);
        traits.needs_conversion = true;
        is_mutable = true;
    }

    const vk::Format format = traits.native;

    ASSERT_MSG(format != vk::Format::eUndefined && levels >= 1,
               "Image allocation parameters are invalid");

    u32 num_images{};
    std::array<vk::Image, 2> raw_images;

    vk::ImageCreateFlags flags{};
    if (texture_type == VideoCore::TextureType::CubeMap) {
        flags |= vk::ImageCreateFlagBits::eCubeCompatible;
    }
    if (is_mutable) {
        flags |= vk::ImageCreateFlagBits::eMutableFormat;
    }

    // Ensure color formats have the color attachment bit set for framebuffers
    auto usage = traits.usage;
    const bool is_color =
        (traits.aspect & vk::ImageAspectFlagBits::eColor) != vk::ImageAspectFlags{};
    if (is_color) {
        usage |= vk::ImageUsageFlagBits::eColorAttachment;
    }
    if (traits.native == vk::Format::eR8G8B8A8Unorm && traits.storage_support) {
        // Add Storage-usage support when available in case it is found out later that this is a
        // shadow-source texture that will get used in the utility descriptor-set
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    const bool need_format_list = is_mutable && instance.IsImageFormatListSupported();
    handles[Type::Base].Create(width, height, levels, texture_type, format, usage, flags,
                               traits.aspect, need_format_list, DebugName(false));
    raw_images[num_images++] = handles[Type::Base].image;

    if (res_scale != 1) {
        handles[Type::Scaled].Create(GetScaledWidth(), GetScaledHeight(), levels, texture_type,
                                     format, usage, flags, traits.aspect, need_format_list,
                                     DebugName(true));
        raw_images[num_images++] = handles[Type::Scaled].image;
    }

    current = res_scale != 1 ? Type::Scaled : Type::Base;

    runtime.renderpass_cache.EndRendering();
    scheduler.Record([raw_images, num_images, aspect = traits.aspect](vk::CommandBuffer cmdbuf) {
        std::array<vk::ImageMemoryBarrier, 3> barriers;
        MakeInitBarriers(aspect, num_images, raw_images, barriers);
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTopOfPipe,
            vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, num_images, barriers.data());
    });
}

Surface::Surface(TextureRuntime& runtime_, const VideoCore::SurfaceBase& surface,
                 const VideoCore::Material* mat)
    : SurfaceBase{surface}, runtime{runtime_}, instance{runtime_.GetInstance()},
      scheduler{runtime_.GetScheduler()}, traits{instance.GetTraits(mat->format)},
      handles{Handle(instance), Handle(instance), Handle(instance), Handle(instance)} {
    if (!traits.transfer_support) {
        return;
    }

    const bool has_normal = mat && mat->Map(MapType::Normal);
    const vk::Format format = traits.native;

    u32 num_images{};
    std::array<vk::Image, 3> raw_images;

    vk::ImageCreateFlags flags{};
    if (texture_type == VideoCore::TextureType::CubeMap) {
        flags |= vk::ImageCreateFlagBits::eCubeCompatible;
    }

    const std::string debug_name = DebugName(false, true);
    handles[Type::Base].Create(mat->width, mat->height, levels, texture_type, format, traits.usage,
                               flags, traits.aspect, false, debug_name);
    raw_images[num_images++] = handles[Type::Base].image;

    if (res_scale != 1) {
        handles[Type::Scaled].Create(mat->width, mat->height, levels, texture_type,
                                     vk::Format::eR8G8B8A8Unorm, traits.usage, flags, traits.aspect,
                                     false, debug_name);
        raw_images[num_images++] = handles[Type::Scaled].image;
    }
    if (has_normal) {
        handles[Type::Custom].Create(mat->width, mat->height, levels, texture_type, format,
                                     traits.usage, flags, traits.aspect, false, debug_name);
        raw_images[num_images++] = handles[Type::Custom].image;
    }

    current = res_scale != 1 ? Type::Scaled : Type::Base;

    runtime.renderpass_cache.EndRendering();
    scheduler.Record([raw_images, num_images, aspect = traits.aspect](vk::CommandBuffer cmdbuf) {
        std::array<vk::ImageMemoryBarrier, 3> barriers;
        MakeInitBarriers(aspect, num_images, raw_images, barriers);
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTopOfPipe,
            vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, num_images, barriers.data());
    });

    custom_format = mat->format;
    material = mat;
}

void Surface::Upload(const VideoCore::BufferTextureCopy& upload,
                     const VideoCore::StagingData& staging) {
#if defined(__SWITCH__) && HEN_DEV
    // (for the line about a gap between two frames: common/hen_gap.h)
    struct UploadTime {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~UploadTime() {
            hen_gap_add(HEN_GAP_UPLOADS, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count());
        }
    } upload_time;
#endif
    HEN_WORK_COUNT(HEN_WORK_UPLOADS, 1);
    runtime.renderpass_cache.EndRendering();

    // (Switch 2 port) Depth and stencil the program wrote, for a surface drawn at a multiple
    // of its size, where the device can neither blit the format (BlitScale) nor write stencil
    // from a shader (BlitHelper::BlitDepthStencil): NVK here. Without this the scaled image
    // kept its old contents and "Depth scale unsupported by hardware" was all that happened.
    // The pixels are repeated here and uploaded to the scaled image as well. (Read before the
    // upload below is recorded: that rewrites the staging data in place, on another thread.)
    std::vector<u32> hen_source;
    const bool hen_scale_here =
        hen_depth_scale_blit && res_scale != 1 && pixel_format == PixelFormat::D24S8 &&
        !traits.blit_support && (Aspect() & vk::ImageAspectFlagBits::eStencil) &&
        (traits.native == vk::Format::eD24UnormS8Uint ||
         traits.native == vk::Format::eD32SfloatS8Uint);
    const u32 hen_w = upload.texture_rect.GetWidth(), hen_h = upload.texture_rect.GetHeight();
    if (hen_scale_here && staging.size >= hen_w * hen_h * 5) {
        hen_source.resize(static_cast<std::size_t>(hen_w) * hen_h);
        std::memcpy(hen_source.data(), staging.mapped.data(), hen_source.size() * 4);
    }
    // (Switch 2 port) Depth and stencil are separated here, in ordinary memory, not by
    // UnpackDepthStencil in the recorded command below: that reads and rewrites the upload
    // buffer a pixel at a time, and reads of that memory are slow on this device (not cached):
    // 40 ms for a 512x400 depth buffer, on the thread the emulation thread waits for at every
    // frame. A final smash in Super Smash Bros. that writes the depth buffer every frame ran
    // at 27 to 38%.
    bool hen_unpacked = false;
    if (pixel_format == PixelFormat::D24S8 && (Aspect() & vk::ImageAspectFlagBits::eStencil) &&
        traits.native == vk::Format::eD24UnormS8Uint && staging.size >= 5) {
        const std::size_t pixels = staging.size / 5;
        std::vector<u32> words(pixels);
        // (one pass over the buffer at most: the copy made above for scaling serves too)
        if (hen_source.size() == pixels) {
            std::memcpy(words.data(), hen_source.data(), pixels * 4);
        } else {
            std::memcpy(words.data(), staging.mapped.data(), pixels * 4);
        }
        std::vector<u8> stencil(pixels);
        for (std::size_t i = 0; i < pixels; i++) {
            stencil[i] = static_cast<u8>(words[i]);
            words[i] >>= 8;
        }
        std::memcpy(staging.mapped.data(), words.data(), pixels * 4);
        std::memcpy(staging.mapped.data() + 4 * staging.size / 5, stencil.data(), pixels);
        hen_unpacked = true;
    }
    // Writes the scaled depth words, then the scaled stencil bytes, at out.
    const auto hen_scale_into = [&](u8* out) {
        const u32 s = res_scale, sw = hen_w * s, sh = hen_h * s;
        u8* const depth_out = out;
        u8* const stencil_out = out + static_cast<std::size_t>(sw) * sh * 4;
        const bool as_float = traits.native == vk::Format::eD32SfloatS8Uint;
        // One scaled row built in ordinary memory, then written s times: the buffer may be
        // write-combined memory, which must not be read back.
        std::vector<u32> depth_row(sw);
        std::vector<u8> stencil_row(sw);
        for (u32 y = 0; y < hen_h; y++) {
            const u32* const src = hen_source.data() + static_cast<std::size_t>(y) * hen_w;
            for (u32 x = 0; x < hen_w; x++) {
                const u32 d24s8 = src[x];
                u32 word = d24s8 >> 8;
                if (as_float) {
                    const float d32 = (d24s8 >> 8) / 16777215.f;
                    std::memcpy(&word, &d32, 4);
                }
                for (u32 i = 0; i < s; i++) {
                    depth_row[x * s + i] = word;
                    stencil_row[x * s + i] = static_cast<u8>(d24s8);
                }
            }
            for (u32 i = 0; i < s; i++) {
                const std::size_t row = static_cast<std::size_t>(y) * s + i;
                std::memcpy(depth_out + row * sw * 4, depth_row.data(), static_cast<std::size_t>(sw) * 4);
                std::memcpy(stencil_out + row * sw, stencil_row.data(), sw);
            }
        }
    };

    const RecordParams params = {
        .aspect = Aspect(),
        .pipeline_flags = PipelineStageFlags(),
        .src_access = AccessFlags(),
        .src_image = Image(Type::Base),
    };

    scheduler.Record([buffer = runtime.upload_buffer.Handle(), format = traits.native, params,
                      staging, upload, hen_unpacked](vk::CommandBuffer cmdbuf) {
        boost::container::static_vector<vk::BufferImageCopy, 2> buffer_image_copies;

        const auto rect = upload.texture_rect;
        buffer_image_copies.emplace_back(vk::BufferImageCopy{
            .bufferOffset = upload.buffer_offset,
            .bufferRowLength = rect.GetWidth(),
            .bufferImageHeight = rect.GetHeight(),
            .imageSubresource{
                .aspectMask = params.aspect,
                .mipLevel = upload.texture_level,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = {static_cast<s32>(rect.left), static_cast<s32>(rect.bottom), 0},
            .imageExtent = {rect.GetWidth(), rect.GetHeight(), 1},
        });

        if (params.aspect & vk::ImageAspectFlagBits::eStencil) {
            buffer_image_copies[0].imageSubresource.aspectMask = vk::ImageAspectFlagBits::eDepth;

            vk::BufferImageCopy& stencil_copy =
                buffer_image_copies.emplace_back(buffer_image_copies[0]);
            stencil_copy = buffer_image_copies[0];
            stencil_copy.bufferOffset +=
                hen_unpacked ? 4 * staging.size / 5 : UnpackDepthStencil(staging, format);
            stencil_copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eStencil;
        }

        const vk::ImageMemoryBarrier read_barrier = {
            .srcAccessMask = params.src_access,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = params.src_image,
            .subresourceRange = MakeSubresourceRange(params.aspect, upload.texture_level),
        };
        const vk::ImageMemoryBarrier write_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = params.src_access,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = params.src_image,
            .subresourceRange = MakeSubresourceRange(params.aspect, upload.texture_level),
        };

        cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);

        cmdbuf.copyBufferToImage(buffer, params.src_image, vk::ImageLayout::eTransferDstOptimal,
                                 buffer_image_copies);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                               vk::DependencyFlagBits::eByRegion, {}, {}, write_barrier);
    });

    runtime.upload_buffer.Commit(staging.size);

    if (res_scale != 1) {
        ASSERT_MSG(handles[Type::Scaled], "Scaled allocation missing during upload");

        const VideoCore::TextureBlit blit = {
            .src_level = upload.texture_level,
            .dst_level = upload.texture_level,
            .src_rect = upload.texture_rect,
            .dst_rect = upload.texture_rect * res_scale,
        };
        if (!hen_source.empty()) {
            const u32 size = hen_w * res_scale * hen_h * res_scale * 5;
            const auto [data, offset, invalidate] = runtime.upload_buffer.Map(size, 16);
            hen_scale_into(data);
            const RecordParams scaled_params = {
                .aspect = Aspect(),
                .pipeline_flags = PipelineStageFlags(),
                .src_access = AccessFlags(),
                .src_image = Image(Type::Scaled),
            };
            scheduler.Record([buffer = runtime.upload_buffer.Handle(), scaled_params,
                              rect = blit.dst_rect, level = upload.texture_level,
                              offset](vk::CommandBuffer cmdbuf) {
                std::array<vk::BufferImageCopy, 2> copies;
                copies[0] = vk::BufferImageCopy{
                    .bufferOffset = offset,
                    .bufferRowLength = rect.GetWidth(),
                    .bufferImageHeight = rect.GetHeight(),
                    .imageSubresource{
                        .aspectMask = vk::ImageAspectFlagBits::eDepth,
                        .mipLevel = level,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                    .imageOffset = {static_cast<s32>(rect.left), static_cast<s32>(rect.bottom), 0},
                    .imageExtent = {rect.GetWidth(), rect.GetHeight(), 1},
                };
                copies[1] = copies[0];
                copies[1].bufferOffset += static_cast<u64>(rect.GetWidth()) * rect.GetHeight() * 4;
                copies[1].imageSubresource.aspectMask = vk::ImageAspectFlagBits::eStencil;
                const vk::ImageMemoryBarrier read_barrier = {
                    .srcAccessMask = scaled_params.src_access,
                    .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .oldLayout = vk::ImageLayout::eGeneral,
                    .newLayout = vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = scaled_params.src_image,
                    .subresourceRange = MakeSubresourceRange(scaled_params.aspect, level),
                };
                const vk::ImageMemoryBarrier write_barrier = {
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = scaled_params.src_access,
                    .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                    .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = scaled_params.src_image,
                    .subresourceRange = MakeSubresourceRange(scaled_params.aspect, level),
                };
                cmdbuf.pipelineBarrier(scaled_params.pipeline_flags,
                                       vk::PipelineStageFlagBits::eTransfer,
                                       vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);
                cmdbuf.copyBufferToImage(buffer, scaled_params.src_image,
                                         vk::ImageLayout::eTransferDstOptimal, copies);
                cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                       scaled_params.pipeline_flags,
                                       vk::DependencyFlagBits::eByRegion, {}, {}, write_barrier);
            });
            runtime.upload_buffer.Commit(size);
        } else if ((type != SurfaceType::Color && type != SurfaceType::Texture) ||
                   !runtime.blit_helper.Filter(*this, blit)) {
            BlitScale(blit, true);
        }
    }
}

void Surface::UploadCustom(const VideoCore::Material* material, u32 level) {
    const u32 width = material->width;
    const u32 height = material->height;
    const auto color = material->textures[0];
    const Common::Rectangle rect{0U, height, width, 0U};

    const auto upload = [&](Type type, VideoCore::CustomTexture* texture) {
        const u32 custom_size = static_cast<u32>(texture->data.size());
        const RecordParams params = {
            .aspect = vk::ImageAspectFlagBits::eColor,
            .pipeline_flags = PipelineStageFlags(),
            .src_access = AccessFlags(),
            .src_image = Image(type),
        };

        const auto [data, offset, invalidate] = runtime.upload_buffer.Map(custom_size, 0);
        std::memcpy(data, texture->data.data(), custom_size);
        runtime.upload_buffer.Commit(custom_size);

        scheduler.Record([buffer = runtime.upload_buffer.Handle(), level, params, rect,
                          offset = offset](vk::CommandBuffer cmdbuf) {
            const vk::BufferImageCopy buffer_image_copy = {
                .bufferOffset = offset,
                .bufferRowLength = 0,
                .bufferImageHeight = rect.GetHeight(),
                .imageSubresource{
                    .aspectMask = params.aspect,
                    .mipLevel = level,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .imageOffset = {static_cast<s32>(rect.left), static_cast<s32>(rect.bottom), 0},
                .imageExtent = {rect.GetWidth(), rect.GetHeight(), 1},
            };

            const vk::ImageMemoryBarrier read_barrier = {
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, level),
            };
            const vk::ImageMemoryBarrier write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.src_access,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, level),
            };

            cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);

            cmdbuf.copyBufferToImage(buffer, params.src_image, vk::ImageLayout::eTransferDstOptimal,
                                     buffer_image_copy);

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, write_barrier);
        });
    };

    upload(Type::Base, color);
    if (auto* texture = material->textures[u32(MapType::Normal)]) {
        upload(Type::Custom, texture);
    }
}

void Surface::Download(const VideoCore::BufferTextureCopy& download,
                       const VideoCore::StagingData& staging) {
    HEN_WORK_COUNT(HEN_WORK_DOWNLOADS, 1);
    bool hen_finished = false; // (the depth path below has done its own waiting)
    SCOPE_EXIT({
        if (!hen_finished) {
            HenGfxTimer hen_timer{HEN_GFX_DOWNLOAD_FINISH};
            HenGfxTimer hen_depth_timer{pixel_format == PixelFormat::D24S8 ? HEN_GFX_DOWNLOAD_DEPTH
                                                                           : HEN_GFX_KINDS};
            HenDownloadFinish(scheduler);
        }
        runtime.download_buffer.Commit(staging.size);
    });

    runtime.renderpass_cache.EndRendering();

    if (pixel_format == PixelFormat::D24S8) {
#ifdef __SWITCH__
        // (who asks for it: the 2nd, 100th and 1000th of a run)
        static u32 depth_downloads;
        if (++depth_downloads == 2 || depth_downloads == 100 || depth_downloads == 1000) {
            hen_backtrace("a depth download");
        }
        static bool told;
        if (!told) {
            told = true;
            std::printf("hen: gfx: first depth download: %ux%u of a %ux%u surface, scale %u, host format %s\n",
                        download.texture_rect.GetWidth(), download.texture_rect.GetHeight(), width,
                        height, static_cast<unsigned>(res_scale), vk::to_string(traits.native).c_str());
        }
#endif
#ifdef __SWITCH__
        // (Switch 2 port) A program that reads the depth buffer with its CPU (Ocarina of Time's
        // lens flare, Super Smash Bros. in some matches: once a frame) made every such read
        // wait for the GPU to finish everything drawn so far: 5 to 6 ms each, a quarter of the
        // emulation thread's time and 73 to 76% speed in a Smash match. When the same part of
        // the same surface was read less than 100 ms ago, the program is given what was copied
        // out then (one frame old), and a copy of the present contents is recorded for the next
        // read, without waiting. --sync-depth-reads: wait every time, as upstream does.
        if (hen_depth_lag) {
            const auto now = std::chrono::steady_clock::now();
            const auto& rect = download.texture_rect;
            // (the slot for this part of the surface, or the one unused for the longest)
            using HenDepthLag = TextureRuntime::HenDepthLag;
            auto& hen_lags = runtime.hen_lags;
            HenDepthLag* slot = &hen_lags[0];
            bool same = false;
            for (HenDepthLag& candidate : hen_lags) {
                if (candidate.data != nullptr && candidate.addr == addr &&
                    candidate.size == staging.size &&
                    candidate.level == download.texture_level && candidate.left == rect.left &&
                    candidate.top == rect.top && candidate.right == rect.right &&
                    candidate.bottom == rect.bottom) {
                    slot = &candidate;
                    same = true;
                    break;
                }
                if (candidate.time < slot->time) {
                    slot = &candidate;
                }
            }
            HenDepthLag& hen_lag = *slot;
            const bool recent = same && now - hen_lag.time < std::chrono::milliseconds(100);
            HenGfxTimer hen_timer{HEN_GFX_DOWNLOAD_FINISH};
            HenGfxTimer hen_depth_timer{HEN_GFX_DOWNLOAD_DEPTH};
            if (recent) {
                scheduler.Wait(hen_lag.tick); // (submitted a frame ago: normally done already)
                std::memcpy(staging.mapped.data(), hen_lag.data, staging.size);
                hen_gfx_note(HEN_GFX_DOWNLOAD_LAGGED, 0);
            }
            const auto [data, offset, invalidate] = runtime.depth_lag_buffer.Map(staging.size, 16);
            VideoCore::BufferTextureCopy copy = download;
            copy.buffer_offset = offset;
            runtime.blit_helper.DepthToBuffer(*this, runtime.depth_lag_buffer.Handle(), copy);
            runtime.depth_lag_buffer.Commit(staging.size);
            hen_lag = {data, addr, staging.size, download.texture_level, rect.left, rect.top, rect.right,
                       rect.bottom, scheduler.CurrentTick(), now};
            if (!recent) {
                HenDownloadFinish(scheduler);
                std::memcpy(staging.mapped.data(), data, staging.size);
            }
            hen_finished = true;
            return;
        }
#endif
        runtime.blit_helper.DepthToBuffer(*this, runtime.download_buffer.Handle(), download);
        return;
    }

    if (res_scale != 1) {
        const VideoCore::TextureBlit blit = {
            .src_level = download.texture_level,
            .dst_level = download.texture_level,
            .src_rect = download.texture_rect * res_scale,
            .dst_rect = download.texture_rect,
        };

        BlitScale(blit, false);
    }

    const RecordParams params = {
        .aspect = Aspect(),
        .pipeline_flags = PipelineStageFlags(),
        .src_access = AccessFlags(),
        .src_image = Image(Type::Base),
    };

    scheduler.Record(
        [buffer = runtime.download_buffer.Handle(), params, download](vk::CommandBuffer cmdbuf) {
            const auto rect = download.texture_rect;
            const vk::BufferImageCopy buffer_image_copy = {
                .bufferOffset = download.buffer_offset,
                .bufferRowLength = rect.GetWidth(),
                .bufferImageHeight = rect.GetHeight(),
                .imageSubresource{
                    .aspectMask = params.aspect,
                    .mipLevel = download.texture_level,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .imageOffset = {static_cast<s32>(rect.left), static_cast<s32>(rect.bottom), 0},
                .imageExtent = {rect.GetWidth(), rect.GetHeight(), 1},
            };

            const vk::ImageMemoryBarrier read_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, download.texture_level),
            };
            const vk::ImageMemoryBarrier image_write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, download.texture_level),
            };
            const vk::MemoryBarrier memory_write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            };

            cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);

            cmdbuf.copyImageToBuffer(params.src_image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                     buffer_image_copy);

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                                   vk::DependencyFlagBits::eByRegion, memory_write_barrier, {},
                                   image_write_barrier);
        });
}

void Surface::ScaleUp(u32 new_scale) {
    if (res_scale == new_scale || new_scale == 1) {
        return;
    }

    res_scale = new_scale;

    const bool is_mutable = pixel_format == VideoCore::PixelFormat::RGBA8;

    vk::ImageCreateFlags flags{};
    if (texture_type == VideoCore::TextureType::CubeMap) {
        flags |= vk::ImageCreateFlagBits::eCubeCompatible;
    }
    if (is_mutable) {
        flags |= vk::ImageCreateFlagBits::eMutableFormat;
    }

    handles[Type::Scaled].Create(GetScaledWidth(), GetScaledHeight(), levels, texture_type,
                                 traits.native, traits.usage, flags, traits.aspect, false,
                                 DebugName(true));
    HEN_SURFACE_COUNT(HEN_IMAGE_SCALE_UP, 1);
    hen_scaled_up = true; // (other usage flags than the constructor's: not for the pool)
    current = Type::Scaled;

    handles[Type::Copy].Destroy();

    runtime.renderpass_cache.EndRendering();
    scheduler.Record(
        [raw_images = std::array{Image()}, aspect = traits.aspect](vk::CommandBuffer cmdbuf) {
            std::array<vk::ImageMemoryBarrier, 1> barriers;
            MakeInitBarriers(aspect, 1, raw_images, barriers);
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                   vk::PipelineStageFlagBits::eTopOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, barriers);
        });

    for (u32 level = 0; level < levels; level++) {
        const VideoCore::TextureBlit blit = {
            .src_level = level,
            .dst_level = level,
            .src_rect = GetRect(level),
            .dst_rect = GetScaledRect(level),
        };
        BlitScale(blit, true);
    }
}

bool Surface::HenPoolable() const noexcept {
    return texture_type == VideoCore::TextureType::Texture2D && material == nullptr &&
           custom_format == VideoCore::CustomPixelFormat::Invalid && !hen_scaled_up &&
           handles[Type::Base] && !handles[Type::Custom] &&
           static_cast<bool>(handles[Type::Scaled]) == (res_scale != 1);
}

u64 Surface::HenImageBytes() const {
    u64 bytes = 0;
    for (const Handle& handle : handles) {
        if (handle) {
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(instance.GetAllocator(), handle.allocation, &info);
            bytes += info.size;
        }
    }
    return bytes;
}

bool Surface::HenFits(const VideoCore::SurfaceParams& params,
                      const VideoCore::SurfaceFlagBits& initial_flags) const {
    if (params.width != width || params.height != height || params.levels != levels ||
        params.res_scale != res_scale || params.pixel_format != pixel_format ||
        params.texture_type != texture_type ||
        params.custom_format != VideoCore::CustomPixelFormat::Invalid) {
        return false;
    }
    // The format, usage and aspect the constructor would choose (the flags the surface has
    // now do not say what it chose: the rasterizer sets ShadowSource on live surfaces).
    FormatTraits wanted = instance.GetTraits(params.pixel_format);
    if (True(initial_flags & VideoCore::SurfaceFlagBits::ShadowSource) &&
        wanted.native != vk::Format::eR8G8B8A8Unorm) {
        wanted = instance.GetTraits(VideoCore::PixelFormat::RGBA8);
        wanted.needs_conversion = true;
    }
    return wanted == traits;
}

void Surface::HenRecycle(const VideoCore::SurfaceParams& params,
                         const VideoCore::SurfaceFlagBits& initial_flags) {
    // SurfaceBase, as its constructor leaves it. The modification tick goes on counting: a
    // tick someone remembers for this id must not come round again.
    static_cast<VideoCore::SurfaceParams&>(*this) = params;
    flags = initial_flags;
    material = nullptr;
    invalid_regions.clear();
    fill_size = 0;
    fill_data = {};
    modification_tick++;
    // The images, their views and framebuffers stay, the copy image too if there is one
    // (CopyImageView writes it before a draw reads it). is_framebuffer and is_storage stay
    // set: they only widen the access masks of barriers, and the images were used that way.
    if (instance.HasDebuggingToolAttached()) {
        SetObjectName(instance.GetDevice(), handles[Type::Base].image, DebugName(false));
        if (handles[Type::Scaled]) {
            SetObjectName(instance.GetDevice(), handles[Type::Scaled].image, DebugName(true));
        }
    }
}

u32 Surface::GetInternalBytesPerPixel() const {
    // Request 5 bytes for D24S8 as well because we can use the
    // extra space when deinterleaving the data during upload
    if (traits.native == vk::Format::eD24UnormS8Uint) {
        return 5;
    }

    return vk::blockSize(traits.native);
}

vk::AccessFlags Surface::AccessFlags() const noexcept {
    const bool is_color = static_cast<bool>(Aspect() & vk::ImageAspectFlagBits::eColor);
    const vk::AccessFlags attachment_flags =
        is_color
            ? vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
            : vk::AccessFlagBits::eDepthStencilAttachmentRead |
                  vk::AccessFlagBits::eDepthStencilAttachmentWrite;

    return vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead |
           vk::AccessFlagBits::eTransferWrite |
           (is_framebuffer ? attachment_flags : vk::AccessFlagBits::eNone) |
           (is_storage ? vk::AccessFlagBits::eShaderWrite : vk::AccessFlagBits::eNone);
}

vk::PipelineStageFlags Surface::PipelineStageFlags() const noexcept {
    const bool is_color = static_cast<bool>(Aspect() & vk::ImageAspectFlagBits::eColor);
    const vk::PipelineStageFlags attachment_flags =
        is_color ? vk::PipelineStageFlagBits::eColorAttachmentOutput
                 : vk::PipelineStageFlagBits::eEarlyFragmentTests |
                       vk::PipelineStageFlagBits::eLateFragmentTests;

    return vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eFragmentShader |
           (is_framebuffer ? attachment_flags : vk::PipelineStageFlagBits::eNone) |
           (is_storage ? vk::PipelineStageFlagBits::eComputeShader
                       : vk::PipelineStageFlagBits::eNone);
}

vk::ImageView Surface::CopyImageView() noexcept {
    auto& copy_handle = handles[Type::Copy];
    vk::ImageLayout copy_layout = vk::ImageLayout::eGeneral;
    if (!copy_handle) {
        vk::ImageCreateFlags flags{};
        if (texture_type == VideoCore::TextureType::CubeMap) {
            flags |= vk::ImageCreateFlagBits::eCubeCompatible;
        }
        copy_handle.Create(GetScaledWidth(), GetScaledHeight(), levels, texture_type, traits.native,
                           traits.usage, flags, traits.aspect, false);
        copy_layout = vk::ImageLayout::eUndefined;
        HEN_SURFACE_COUNT(HEN_IMAGE_COPY, 1);
    }
    {
        unsigned long long hen_bytes = 0;
        for (u32 level = 0; level < levels; level++) {
            hen_bytes += static_cast<unsigned long long>(GetScaledWidth() >> level) *
                         (GetScaledHeight() >> level) * vk::blockSize(traits.native);
        }
        HEN_WORK_COUNT(HEN_WORK_FEEDBACK_COPIES, 1);
        HEN_WORK_COUNT(HEN_WORK_FEEDBACK_BYTES, hen_bytes);
        (void)hen_bytes;
    }

    runtime.renderpass_cache.EndRendering();

    const RecordParams params = {
        .aspect = Aspect(),
        .pipeline_flags = PipelineStageFlags(),
        .src_access = AccessFlags(),
        .src_image = Image(),
        .dst_image = copy_handle.image,
    };

    scheduler.Record([params, copy_layout, levels = this->levels, width = GetScaledWidth(),
                      height = GetScaledHeight()](vk::CommandBuffer cmdbuf) {
        std::array pre_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, 0, levels),
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eShaderRead,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = copy_layout,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, 0, levels),
            },
        };
        std::array post_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, 0, levels),
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange = MakeSubresourceRange(params.aspect, 0, levels),
            },
        };

        boost::container::small_vector<vk::ImageCopy, 3> image_copies;
        for (u32 level = 0; level < levels; level++) {
            image_copies.push_back(vk::ImageCopy{
                .srcSubresource{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = level,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .srcOffset = {0, 0, 0},
                .dstSubresource{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = level,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .dstOffset = {0, 0, 0},
                .extent = {width >> level, height >> level, 1},
            });
        }

        cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

        cmdbuf.copyImage(params.src_image, vk::ImageLayout::eTransferSrcOptimal, params.dst_image,
                         vk::ImageLayout::eTransferDstOptimal, image_copies);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
    });

    return copy_handle.image_views[ViewType::Sample];
}

vk::ImageView Surface::ImageView(ViewType view_type, Type type) noexcept {
    auto& handle = handles[type == Type::Current ? current : type];
    if (auto image_view = handle.image_views[view_type]) {
        return image_view;
    }

    auto aspect = traits.aspect;

    if (view_type == ViewType::Storage) {
        ASSERT(traits.native == vk::Format::eR8G8B8A8Unorm);
        is_storage = true;
    }
    if (view_type == ViewType::Depth || view_type == ViewType::Stencil) {
        ASSERT(this->type == SurfaceType::DepthStencil);
        aspect = view_type == ViewType::Depth ? vk::ImageAspectFlagBits::eDepth
                                              : vk::ImageAspectFlagBits::eStencil;
    }

    const vk::ImageViewCreateInfo view_info = {
        .image = handle.image,
        .viewType = vk::ImageViewType::e2D,
        .format = view_type == ViewType::Storage ? vk::Format::eR32Uint : traits.native,
        .subresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = (view_type == ViewType::Mip0 || view_type == ViewType::Storage)
                              ? 1u
                              : VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };
    handle.image_views[view_type] = instance.GetDevice().createImageView(view_info);
    return handle.image_views[view_type];
}

vk::Framebuffer Surface::Framebuffer(Type type) noexcept {
    auto& handle = handles[type == Type::Current ? current : type];
    if (handle.framebuffer) {
        return handle.framebuffer;
    }

    const bool is_depth =
        this->type == SurfaceType::Depth || this->type == SurfaceType::DepthStencil;
    const auto color_format = is_depth ? PixelFormat::Invalid : pixel_format;
    const auto depth_format = is_depth ? pixel_format : PixelFormat::Invalid;

    const auto image_view = ImageView(ViewType::Mip0, type);
    const vk::FramebufferCreateInfo framebuffer_info = {
        .renderPass = runtime.renderpass_cache.GetRenderpass(color_format, depth_format, false),
        .attachmentCount = 1u,
        .pAttachments = &image_view,
        .width = handle.width,
        .height = handle.height,
        .layers = handle.layers,
    };
    handle.framebuffer = instance.GetDevice().createFramebuffer(framebuffer_info);
    return handle.framebuffer;
}

void Surface::BlitScale(const VideoCore::TextureBlit& blit, bool up_scale) {
    HEN_WORK_COUNT(up_scale ? HEN_WORK_SCALE_UP : HEN_WORK_SCALE_DOWN, 1);
    const bool is_depth_stencil = pixel_format == PixelFormat::D24S8;
    if (is_depth_stencil && !traits.blit_support) {
        // (Switch 2 port) The device can't blit this format (NVK here: D24S8), so a depth
        // surface the program wrote was never scaled up, and what was then drawn against it
        // used stale depth. Draw it across with the shader that BlitTextures uses.
        if (!hen_depth_scale_blit || !runtime.blit_helper.BlitDepthStencilScale(*this, blit, up_scale)) {
            LOG_WARNING(Render_Vulkan, "Depth scale unsupported by hardware");
        }
        return;
    }

    const auto src_type = up_scale ? Type::Base : Type::Scaled;
    const auto dst_type = up_scale ? Type::Scaled : Type::Base;

    scheduler.Record([src_image = Image(src_type), aspect = Aspect(),
                      filter = MakeFilter(pixel_format), dst_image = Image(dst_type),
                      blit](vk::CommandBuffer render_cmdbuf) {
        const std::array source_offsets = {
            vk::Offset3D{static_cast<s32>(blit.src_rect.left),
                         static_cast<s32>(blit.src_rect.bottom), 0},
            vk::Offset3D{static_cast<s32>(blit.src_rect.right), static_cast<s32>(blit.src_rect.top),
                         1},
        };

        const std::array dest_offsets = {
            vk::Offset3D{static_cast<s32>(blit.dst_rect.left),
                         static_cast<s32>(blit.dst_rect.bottom), 0},
            vk::Offset3D{static_cast<s32>(blit.dst_rect.right), static_cast<s32>(blit.dst_rect.top),
                         1},
        };

        const vk::ImageBlit blit_area = {
            .srcSubresource{
                .aspectMask = aspect,
                .mipLevel = blit.src_level,
                .baseArrayLayer = blit.src_layer,
                .layerCount = 1,
            },
            .srcOffsets = source_offsets,
            .dstSubresource{
                .aspectMask = aspect,
                .mipLevel = blit.dst_level,
                .baseArrayLayer = blit.dst_layer,
                .layerCount = 1,
            },
            .dstOffsets = dest_offsets,
        };

        const std::array read_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = MakeSubresourceRange(aspect, blit.src_level),
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eShaderRead |
                                 vk::AccessFlagBits::eDepthStencilAttachmentRead |
                                 vk::AccessFlagBits::eColorAttachmentRead |
                                 vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = MakeSubresourceRange(aspect, blit.dst_level),
            },
        };
        const std::array write_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eMemoryRead,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = MakeSubresourceRange(aspect, blit.src_level),
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eMemoryRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = MakeSubresourceRange(aspect, blit.dst_level),
            },
        };

        render_cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                      vk::PipelineStageFlagBits::eTransfer,
                                      vk::DependencyFlagBits::eByRegion, {}, {}, read_barriers);

        render_cmdbuf.blitImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                                vk::ImageLayout::eTransferDstOptimal, blit_area, filter);

        render_cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                      vk::PipelineStageFlagBits::eAllCommands,
                                      vk::DependencyFlagBits::eByRegion, {}, {}, write_barriers);
    });
}

Framebuffer::Framebuffer(TextureRuntime& runtime, const VideoCore::FramebufferParams& params,
                         Surface* color, Surface* depth)
    : VideoCore::FramebufferParams{params}, instance{runtime.GetInstance()},
      res_scale{color ? color->res_scale : (depth ? depth->res_scale : 1u)} {
    auto& renderpass_cache = runtime.GetRenderpassCache();
    if (shadow_rendering && !color) {
        return;
    }

    width = height = std::numeric_limits<u32>::max();

    u32 num_attachments{};
    std::array<vk::ImageView, 2> attachments;

    const auto prepare = [&](u32 index, Surface* surface) {
        const auto extent = surface->RealExtent();
        width = std::min(width, extent.width);
        height = std::min(height, extent.height);
        formats[index] = surface->pixel_format;
        images[index] = surface->Image();
        aspects[index] = surface->Aspect();
        image_views[index] = surface->FramebufferView();
    };

    if (shadow_rendering) {
        const auto extent = color->RealExtent();
        width = extent.width;
        height = extent.height;
        render_pass =
            renderpass_cache.GetRenderpass(PixelFormat::Invalid, PixelFormat::Invalid, false);
        images[0] = color->Image();
        image_views[0] = color->StorageView();
        aspects[0] = vk::ImageAspectFlagBits::eColor;
    } else {
        if (color) {
            prepare(0, color);
            attachments[num_attachments++] = image_views[0];
        }
        if (depth) {
            prepare(1, depth);
            attachments[num_attachments++] = image_views[1];
        }
        render_pass = renderpass_cache.GetRenderpass(formats[0], formats[1], false);
    }

    const vk::FramebufferCreateInfo framebuffer_info = {
        .renderPass = render_pass,
        .attachmentCount = num_attachments,
        .pAttachments = attachments.data(),
        .width = width,
        .height = height,
        .layers = 1,
    };
    framebuffer = instance.GetDevice().createFramebuffer(framebuffer_info);
}

Framebuffer::~Framebuffer() {
    if (framebuffer) {
        instance.GetDevice().destroyFramebuffer(framebuffer);
    }
}

Sampler::Sampler(TextureRuntime& runtime, const VideoCore::SamplerParams& params) {
    using TextureConfig = VideoCore::SamplerParams::TextureConfig;

    const Instance& instance = runtime.GetInstance();
    const vk::PhysicalDeviceProperties properties = instance.GetPhysicalDevice().getProperties();
    const bool use_border_color =
        instance.IsCustomBorderColorSupported() && (params.wrap_s == TextureConfig::ClampToBorder ||
                                                    params.wrap_t == TextureConfig::ClampToBorder);

    const auto color = PicaToVK::ColorRGBA8(params.border_color);
    const vk::SamplerCustomBorderColorCreateInfoEXT border_color_info = {
        .customBorderColor = MakeClearColorValue(color),
        .format = vk::Format::eUndefined,
    };

    const vk::Filter mag_filter = PicaToVK::TextureFilterMode(params.mag_filter);
    const vk::Filter min_filter = PicaToVK::TextureFilterMode(params.min_filter);
    const vk::SamplerMipmapMode mipmap_mode = PicaToVK::TextureMipFilterMode(params.mip_filter);
    const vk::SamplerAddressMode wrap_u = PicaToVK::WrapMode(params.wrap_s);
    const vk::SamplerAddressMode wrap_v = PicaToVK::WrapMode(params.wrap_t);
    const float lod_min = static_cast<float>(params.lod_min);
    const float lod_max = static_cast<float>(params.lod_max);

    // Do not apply anisotropic filtering if nearest filtering is used at all, as drivers
    // are only recommended (not enforced) to follow the mag/min filter in such cases.
    // Adreno drivers are an example of this, as they force linear filtering when using
    // anisotropic filtering.
    const bool use_anisotropy = instance.IsAnisotropicFilteringSupported() &&
                                mag_filter == vk::Filter::eLinear &&
                                min_filter == vk::Filter::eLinear;

    const vk::SamplerCreateInfo sampler_info = {
        .pNext = use_border_color ? &border_color_info : nullptr,
        .magFilter = mag_filter,
        .minFilter = min_filter,
        .mipmapMode = mipmap_mode,
        .addressModeU = wrap_u,
        .addressModeV = wrap_v,
        .mipLodBias = 0,
        .anisotropyEnable = use_anisotropy,
        .maxAnisotropy = use_anisotropy ? properties.limits.maxSamplerAnisotropy : 1.0f,
        .compareEnable = false,
        .compareOp = vk::CompareOp::eAlways,
        .minLod = lod_min,
        .maxLod = lod_max,
        .borderColor =
            use_border_color ? vk::BorderColor::eFloatCustomEXT : vk::BorderColor::eIntOpaqueBlack,
        .unnormalizedCoordinates = false,
    };
    sampler = instance.GetDevice().createSamplerUnique(sampler_info);
}

Sampler::~Sampler() = default;

DebugScope::DebugScope(TextureRuntime& runtime, Common::Vec4f color, std::string_view label)
    : scheduler{runtime.GetScheduler()},
      has_debug_tool{runtime.GetInstance().HasDebuggingToolAttached()} {
    if (!has_debug_tool) {
        return;
    }
    scheduler.Record([color, label = std::string(label)](vk::CommandBuffer cmdbuf) {
        const vk::DebugUtilsLabelEXT debug_label = {
            .pLabelName = label.data(),
            .color = std::array{color[0], color[1], color[2], color[3]},
        };
        cmdbuf.beginDebugUtilsLabelEXT(debug_label);
    });
}

DebugScope::~DebugScope() {
    if (!has_debug_tool) {
        return;
    }
    scheduler.Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endDebugUtilsLabelEXT(); });
}

} // namespace Vulkan
