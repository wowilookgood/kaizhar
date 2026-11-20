// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdlib>
#include <boost/container/static_vector.hpp>

#include <algorithm>
#include <cstdio>
#include "common/hen_stats.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/loader/loader.h"
#include "video_core/hen_workers.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/renderer_vulkan/pica_to_vk.h"
#include "video_core/renderer_vulkan/vk_descriptor_update_queue.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_render_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/shader/generator/glsl_fs_shader_gen.h"
#include "video_core/shader/generator/glsl_shader_gen.h"
#include "video_core/shader/generator/spv_fs_shader_gen.h"

using namespace Pica::Shader::Generator;
using Pica::Shader::FSConfig;

MICROPROFILE_DEFINE(Vulkan_Bind, "Vulkan", "Pipeline Bind", MP_RGB(192, 32, 32));

#ifdef __SWITCH__
extern "C" int hen_generic_fs, hen_stale_profile, hen_generic_seed;
extern "C" int hen_early_depth; // hen/jit/hen_jit.c: --no-early-depth
extern "C" int hen_generic_early_depth; // hen/jit/hen_jit.c: --no-generic-early-depth
extern "C" int hen_generic_shading_rate; // hen/jit/hen_jit.c: --generic-shading-rate
extern "C" int hen_recycle_surfaces; // hen/jit/hen_jit.c: --recycle-surfaces
extern "C" unsigned hen_recycle_cap_mib, hen_recycle_age_frames;
// Since the last report: draws with the generic fragment shader, its pipelines linked on the
// renderer's thread, ns in that.
extern "C" unsigned long long hen_generic_stats[3];
extern "C" {
// The driver pipeline cache when the GPU right changes hands (SwitchCache below).
// Upstream writes the cache of the title losing the right to its file and reads the other
// title's file, every time: with the console's own keyboard run as a title, that is one write
// of about 0.5 MiB and one read at every open and at every close, on the emulation thread
// inside the service call (System Settings: 54 to 75 ms at open, 22 to 43 ms at close).
// 1: the cache losing the right stays in memory, by title, and comes back from there; a
// title's file is read once a run. Written: the current title's at the end as before, and
// then those kept in memory that have grown since they were read or last written.
// The trade: written each time a title lost the right, its file held after a crash what had
// been compiled up to the last applet opened; kept in memory, it holds what the last clean
// exit left. (A title that opens no applet is only ever written at the end.) What is
// lost then is the driver's part only: the shaders and pipeline descriptions are in the disk
// shader cache's own files, appended as they are made, and the driver has its own cache file.
// 0: --no-driver-cache-keep, as upstream, to compare.
int hen_driver_cache_keep = 1;
}
#else
static constexpr int hen_driver_cache_keep = 0;
#endif

namespace Vulkan {

u32 AttribBytes(Pica::PipelineRegs::VertexAttributeFormat format, u32 size) {
    switch (format) {
    case Pica::PipelineRegs::VertexAttributeFormat::FLOAT:
        return sizeof(float) * size;
    case Pica::PipelineRegs::VertexAttributeFormat::SHORT:
        return sizeof(u16) * size;
    case Pica::PipelineRegs::VertexAttributeFormat::BYTE:
    case Pica::PipelineRegs::VertexAttributeFormat::UBYTE:
        return sizeof(u8) * size;
    }
    return 0;
}

AttribLoadFlags MakeAttribLoadFlag(Pica::PipelineRegs::VertexAttributeFormat format) {
    switch (format) {
    case Pica::PipelineRegs::VertexAttributeFormat::BYTE:
    case Pica::PipelineRegs::VertexAttributeFormat::SHORT:
        return AttribLoadFlags::Sint;
    case Pica::PipelineRegs::VertexAttributeFormat::UBYTE:
        return AttribLoadFlags::Uint;
    default:
        return AttribLoadFlags::Float;
    }
}

constexpr std::array<vk::DescriptorSetLayoutBinding, 6> BUFFER_BINDINGS = {{
    {0, vk::DescriptorType::eUniformBufferDynamic, 1, vk::ShaderStageFlagBits::eVertex},
    {1, vk::DescriptorType::eUniformBufferDynamic, 1,
     vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eGeometry},
    {2, vk::DescriptorType::eUniformBufferDynamic, 1, vk::ShaderStageFlagBits::eFragment},
    {3, vk::DescriptorType::eUniformTexelBuffer, 1, vk::ShaderStageFlagBits::eFragment},
    {4, vk::DescriptorType::eUniformTexelBuffer, 1, vk::ShaderStageFlagBits::eFragment},
    {5, vk::DescriptorType::eUniformTexelBuffer, 1, vk::ShaderStageFlagBits::eFragment},
}};

template <u32 NumTex0>
constexpr std::array<vk::DescriptorSetLayoutBinding, 3> TEXTURE_BINDINGS = {{
    {0, vk::DescriptorType::eCombinedImageSampler, NumTex0,
     vk::ShaderStageFlagBits::eFragment},                                                  // tex0
    {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment}, // tex1
    {2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment}, // tex2
}};

constexpr std::array<vk::DescriptorSetLayoutBinding, 2> UTILITY_BINDINGS = {{
    {0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eFragment}, // shadow_buffer
    {1, vk::DescriptorType::eCombinedImageSampler, 1,
     vk::ShaderStageFlagBits::eFragment}, // tex_normal
}};

#ifdef __SWITCH__
extern "C" int hen_compile_workers; // hen/jit/hen_jit.c
#endif

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             RenderManager& renderpass_cache_, DescriptorUpdateQueue& update_queue_)
    : instance{instance_}, scheduler{scheduler_}, renderpass_cache{renderpass_cache_},
      update_queue{update_queue_},
#ifdef __SWITCH__
      // hardware_concurrency() is 0 here; threads can use six cores (hen_thread.c spreads them).
      // (--compile-workers N, for --nir-timing: one compile at a time)
      num_worker_threads{static_cast<u32>(std::clamp(hen_compile_workers, 1, 8))},
#else
      num_worker_threads{std::max(std::thread::hardware_concurrency(), 2U) / 2},
#endif
      pipeline_workers{num_worker_threads, "Pipeline workers"},
      shader_workers{num_worker_threads, "Shader workers"},
      descriptor_heaps{
          DescriptorHeap{instance, scheduler.GetMasterSemaphore(), BUFFER_BINDINGS, 32},
          DescriptorHeap{instance, scheduler.GetMasterSemaphore(), TEXTURE_BINDINGS<1>},
          DescriptorHeap{instance, scheduler.GetMasterSemaphore(), UTILITY_BINDINGS, 32}},
      trivial_vertex_shader{
          instance, vk::ShaderStageFlagBits::eVertex,
          GLSL::GenerateTrivialVertexShader(instance.IsShaderClipDistanceSupported(), true)},
      hen_generic_fragment_shader{instance}, hen_generic_nd_fragment_shader{instance} {
#ifdef __SWITCH__
    GraphicsPipeline::hen_fast_worker = &shader_workers; // (see GraphicsPipeline::TryBuild)
    // (work from outside the renderer: the PICA vertex JIT, texture decodes; hen_workers.h)
    VideoCore::HenWorkers::Set(
        [](void* owner, std::function<void()> work) {
            static_cast<PipelineCache*>(owner)->shader_workers.QueueWork(std::move(work));
        },
        this);
    std::printf("hen: gfx: %u pipeline worker(s), %u shader worker(s) (%u hardware threads)\n",
                num_worker_threads, num_worker_threads, std::thread::hardware_concurrency());
    std::printf("hen: gfx: early depth: %s; for the generic fragment shader's draws: %s\n",
                hen_early_depth ? "on" : "off",
                hen_early_depth && hen_generic_early_depth && hen_generic_fs ? "on" : "off");
    std::printf("hen: gfx: coarse shading of the generic fragment shader's draws: %dx%d asked, the device offers up to %ux%u\n",
                hen_generic_shading_rate, hen_generic_shading_rate, instance.HenCoarseShading(),
                instance.HenCoarseShading());
    // What the driver was told as this title's Vulkan instance was made: every build
    // says it, so a log shows whether the driver's compression is on (unset) or off.
    {
        const char* const nvk_debug = std::getenv("NVK_DEBUG");
        std::printf("hen: driver: NVK_DEBUG=%s\n", nvk_debug ? nvk_debug : "(unset: compression on)");
    }
    std::printf("hen: gfx: surface recycling: %s, cap %u MiB, age %u frames\n",
                hen_recycle_surfaces ? "on" : "off", hen_recycle_cap_mib, hen_recycle_age_frames);
#endif
    scheduler.RegisterOnDispatch([this] { update_queue.Flush(); });
    profile = Pica::Shader::Profile{
        .enable_accurate_mul = false,
        .has_separable_shaders = true,
        .has_clip_planes = instance.IsShaderClipDistanceSupported(),
        .has_geometry_shader = instance.UseGeometryShaders(),
        .has_custom_border_color = instance.IsCustomBorderColorSupported(),
        .has_fragment_shader_interlock = instance.IsFragmentShaderInterlockSupported(),
        .has_fragment_shader_barycentric = instance.IsFragmentShaderBarycentricSupported(),
        .has_blend_minmax_factor = false,
        .has_minus_one_to_one_range = false,
        .has_logic_op = !instance.NeedsLogicOpEmulation(),
        .vk_disable_spirv_optimizer = Settings::values.disable_spirv_optimizer.GetValue(),
        .vk_use_spirv_generator = Settings::values.spirv_shader_gen.GetValue(),
        .is_vulkan = true,
    };
#ifdef __SWITCH__
    // --stale-profile (a test): a field nothing on Vulkan reads is set, so that the shader
    // caches on the card were "made for other driver and user settings" and are regenerated
    // at the start, as after a driver update: to time that, and to see what the screen shows.
    if (hen_stale_profile) {
        profile.has_gl_arm_framebuffer_fetch = 1;
    }
    // (after the profile is set: queued before it, the worker generates the shader for an
    // empty profile, it does not compile, and the pipeline made from the empty module takes
    // the emulator down)
    // And its variant without the depth write, for draws whose depth the viewport's
    // range carries (early depth): a job of its own, queued with the first, so both modules
    // are there before the title draws. One that fails is never marked done and its draws
    // take the first variant under the range 0..1.
    for (const bool no_depth_write : {false, true}) {
        if (!hen_generic_fs || (no_depth_write && !(hen_early_depth && hen_generic_early_depth))) {
            continue;
        }
        shader_workers.QueueWork([this, generic_profile = profile, no_depth_write] {
            const auto before = std::chrono::steady_clock::now();
            const char* const which = no_depth_write ? " without the depth write" : "";
            Shader& shader =
                no_depth_write ? hen_generic_nd_fragment_shader : hen_generic_fragment_shader;
            const std::string code =
                GLSL::GenerateGenericFragmentShader(generic_profile, no_depth_write);
            const std::vector<u32> spirv = CompileGLSL(code, vk::ShaderStageFlagBits::eFragment);
            if (spirv.empty()) {
                // (never marked done: draws are left out as before)
                std::printf("hen: gfx: the generic fragment shader%s did not compile\n", which);
                return;
            }
            shader.module = CompileSPV(spirv, this->instance.GetDevice());
            if (!shader.module) {
                std::printf("hen: gfx: the generic fragment shader%s: its module was refused\n",
                            which);
                return;
            }
            shader.MarkDone();
            std::printf("hen: gfx: generic fragment shader%s: %zu words of SPIR-V in %.1f ms\n",
                        which, spirv.size(),
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - before)
                            .count());
        });
    }
#endif

    const auto& traits = instance.GetAllTraits();
    size_t i = 0;
    for (const auto& it : traits) {
        profile.vk_format_traits[i].transfer_support = it.transfer_support;
        profile.vk_format_traits[i].blit_support = it.blit_support;
        profile.vk_format_traits[i].attachment_support = it.attachment_support;
        profile.vk_format_traits[i].storage_support = it.storage_support;
        profile.vk_format_traits[i].needs_conversion = it.needs_conversion;
        profile.vk_format_traits[i].needs_emulation = it.needs_emulation;
        profile.vk_format_traits[i].usage_flags = static_cast<u32>(it.usage);
        profile.vk_format_traits[i].aspect_flags = static_cast<u32>(it.aspect);
        profile.vk_format_traits[i].native_format = static_cast<u32>(it.native);
        ++i;
    }

    BuildLayout();
}

void PipelineCache::BuildLayout() {
    std::array<vk::DescriptorSetLayout, NumRasterizerSets> descriptor_set_layouts;
    descriptor_set_layouts[0] = descriptor_heaps[0].Layout();
    descriptor_set_layouts[1] = descriptor_heaps[1].Layout();
    descriptor_set_layouts[2] = descriptor_heaps[2].Layout();

    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = NumRasterizerSets,
        .pSetLayouts = descriptor_set_layouts.data(),
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = nullptr,
    };
    pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(layout_info);
}

PipelineCache::~PipelineCache() {
    GraphicsPipeline::hen_fast_worker = nullptr;
#ifdef __SWITCH__
    // (before the wait below: nothing more is queued from outside once this returns)
    VideoCore::HenWorkers::Clear(this);
#endif
#ifdef __SWITCH__
    // (a shader cache still being regenerated by the shader workers: they finish the entry
    // they are on and stop, or the wait below would last until the whole cache is done)
    for (const auto& disk_cache : disk_caches) {
        disk_cache->HenRegenStop();
    }
#endif
    pipeline_workers.WaitForRequests();
    shader_workers.WaitForRequests();
    SaveDriverPipelineDiskCache();
    // (hen_driver_cache_keep: the titles whose caches were kept in memory when they lost the
    // GPU right; none without the switch)
    for (const auto& [program_id, cache] : hen_parked_driver_caches) {
        if (cache) {
            HenSaveDriverPipelineCache(program_id, *cache, true);
        }
    }
#if defined(__SWITCH__) && HEN_DEV
    if (hen_driver_cache_switches[0] + hen_driver_cache_switches[1] != 0) {
        std::printf("hen: driver cache: %u change(s) of title answered from memory, %u from the "
                    "card; %zu title(s) kept in memory at the end\n",
                    hen_driver_cache_switches[0], hen_driver_cache_switches[1],
                    hen_parked_driver_caches.size());
    }
#endif
#ifdef __SWITCH__
    HenDestroyPipelineParts(instance.GetDevice());
#endif
}

void PipelineCache::LoadCache(const std::atomic_bool& stop_loading,
                              const VideoCore::DiskResourceLoadCallback& callback) {
    LoadDriverPipelineDiskCache(stop_loading, callback);
    LoadDiskCache(stop_loading, callback);
}

void PipelineCache::SwitchCache(u64 title_id, const std::atomic_bool& stop_loading,
                                const VideoCore::DiskResourceLoadCallback& callback) {
    if (GetProgramID() == title_id) {
        LOG_DEBUG(Render_Vulkan,
                  "Skipping pipeline cache switch - already using cache for title_id={:016X}",
                  title_id);
        return;
    }

    // Make sure we have a valid pipeline cache before switching
    if (!driver_pipeline_cache) {
        vk::PipelineCacheCreateInfo cache_info{};
        try {
            driver_pipeline_cache = instance.GetDevice().createPipelineCacheUnique(cache_info);
        } catch (const vk::SystemError& err) {
            LOG_ERROR(Render_Vulkan, "Failed to create pipeline cache: {}", err.what());
            return;
        }
    }

    LOG_INFO(Render_Vulkan, "Switching pipeline cache to title_id={:016X}", title_id);

    // Save current driver cache, update program ID and load the new driver cache
    if (hen_driver_cache_keep) {
        // (kept in memory under its title instead; written at the end, ~PipelineCache. The
        // handle stays valid, so a pipeline of that title still being built keeps a cache.)
        hen_parked_driver_caches[GetProgramID()] = std::move(driver_pipeline_cache);
    } else {
        SaveDriverPipelineDiskCache();
    }
    SetProgramID(title_id);
#ifdef __SWITCH__
    // (HenDropUnbuiltGeneric waits for the shader workers to have nothing queued: a deferred
    // regeneration takes no new entry meanwhile)
    for (const auto& disk_cache : disk_caches) {
        disk_cache->HenRegenPause();
    }
    HenDropUnbuiltGeneric();
#endif
    const auto parked = hen_parked_driver_caches.find(title_id);
    if (hen_driver_cache_keep && parked != hen_parked_driver_caches.end() && parked->second) {
        driver_pipeline_cache = std::move(parked->second);
        hen_parked_driver_caches.erase(parked);
        hen_driver_cache_switches[0]++;
        LOG_INFO(Render_Vulkan, "Pipeline cache for title_id={:016X} was kept in memory",
                 title_id);
    } else {
        if (parked != hen_parked_driver_caches.end()) {
            hen_parked_driver_caches.erase(parked);
        }
        hen_driver_cache_switches[1]++;
        LoadDriverPipelineDiskCache(stop_loading, nullptr);
    }

    // Switch the disk shader cache after driver cache is switched
    SwitchDiskCache(title_id, stop_loading, callback);
#ifdef __SWITCH__
    for (const auto& disk_cache : disk_caches) {
        disk_cache->HenRegenResume();
    }
#endif
}

void PipelineCache::LoadDriverPipelineDiskCache(
    const std::atomic_bool& stop_loading, const VideoCore::DiskResourceLoadCallback& callback) {
    vk::PipelineCacheCreateInfo cache_info{};

    if (callback) {
        callback(VideoCore::LoadCallbackStage::Build, 0, 1, "Driver Pipeline Cache");
    }

    auto load_cache = [this, &cache_info, &callback](bool allow_fallback) {
        const vk::Device device = instance.GetDevice();
        try {
            driver_pipeline_cache = device.createPipelineCacheUnique(cache_info);
        } catch (const vk::SystemError& err) {
            LOG_ERROR(Render_Vulkan, "Failed to create pipeline cache: {}", err.what());
            if (allow_fallback) {
                // Fall back to empty cache
                cache_info.initialDataSize = 0;
                cache_info.pInitialData = nullptr;
                try {
                    driver_pipeline_cache = device.createPipelineCacheUnique(cache_info);
                } catch (const vk::SystemError& err) {
                    LOG_ERROR(Render_Vulkan, "Failed to create fallback pipeline cache: {}",
                              err.what());
                }
            }
        }
        if (callback) {
            callback(VideoCore::LoadCallbackStage::Build, 1, 1, "Driver Pipeline Cache");
        }
    };

    // Try to load existing pipeline cache if disk cache is enabled and directories exist
    if (!Settings::values.use_disk_shader_cache || !EnsureDirectories()) {
        load_cache(false);
        return;
    }

    // Try to load existing pipeline cache for this game/device combination
    const auto cache_dir = GetPipelineCacheDir();
    const u32 vendor_id = instance.GetVendorID();
    const u32 device_id = instance.GetDeviceID();
    const u64 program_id = GetProgramID();
    const auto cache_file_path =
        fmt::format("{}{:016X}-{:X}{:X}.bin", cache_dir, program_id, vendor_id, device_id);

    std::vector<u8> cache_data;
    FileUtil::IOFile cache_file{cache_file_path, "rb"};

    if (!cache_file.IsOpen()) {
        LOG_INFO(Render_Vulkan, "No pipeline cache found for title_id={:016X}", program_id);
        load_cache(false);
        return;
    }

    const u64 cache_file_size = cache_file.GetSize();
    cache_data.resize(cache_file_size);

    if (cache_file.ReadBytes(cache_data.data(), cache_file_size) != cache_file_size) {
        LOG_ERROR(Render_Vulkan, "Error reading pipeline cache");
        load_cache(false);
        return;
    }

    if (!IsCacheValid(cache_data)) {
        LOG_WARNING(Render_Vulkan, "Pipeline cache invalid, removing");
        cache_file.Close();
        FileUtil::Delete(cache_file_path);
        load_cache(false);
        return;
    }

    LOG_INFO(Render_Vulkan, "Loading pipeline cache for title_id={:016X} with size {} KB",
             program_id, cache_file_size / 1024);

    cache_info.initialDataSize = cache_file_size;
    cache_info.pInitialData = cache_data.data();
    // (for HenSaveDriverPipelineCache: what this title's file holds)
    hen_driver_cache_bytes[program_id] = static_cast<std::size_t>(cache_file_size);
    load_cache(true);
}

void PipelineCache::SaveDriverPipelineDiskCache() {
    // Save Vulkan pipeline cache
    if (!Settings::values.use_disk_shader_cache || !driver_pipeline_cache) {
        return;
    }
    // (Switch 2 port: the body moved to HenSaveDriverPipelineCache, which takes the title and
    // the cache, for the caches kept in memory)
    HenSaveDriverPipelineCache(GetProgramID(), *driver_pipeline_cache, false);
}

void PipelineCache::HenSaveDriverPipelineCache(u64 program_id, vk::PipelineCache cache,
                                               bool only_if_grown) {
    if (!Settings::values.use_disk_shader_cache || !cache) {
        return;
    }

    const vk::Device device = instance.GetDevice();
    std::vector<u8> cache_data;
    try {
        cache_data = device.getPipelineCacheData(cache);
    } catch (const vk::SystemError& err) {
        LOG_ERROR(Render_Vulkan, "Unable to get the pipeline cache's data: {}", err.what());
        return;
    }
    if (only_if_grown) {
        // (a pipeline cache only gains entries: the same length is taken as the same content)
        const auto known = hen_driver_cache_bytes.find(program_id);
        if (known != hen_driver_cache_bytes.end() && known->second == cache_data.size()) {
            return;
        }
    }

    const auto cache_dir = GetPipelineCacheDir();
    const u32 vendor_id = instance.GetVendorID();
    const u32 device_id = instance.GetDeviceID();
    // Include both device info and program id in cache path to handle both GPU changes and
    // different games
    const auto cache_file_path =
        fmt::format("{}{:016X}-{:X}{:X}.bin", cache_dir, program_id, vendor_id, device_id);

    FileUtil::IOFile cache_file{cache_file_path, "wb"};
    if (!cache_file.IsOpen()) {
        LOG_ERROR(Render_Vulkan, "Unable to open pipeline cache for writing");
        return;
    }

    if (cache_file.WriteBytes(cache_data.data(), cache_data.size()) != cache_data.size()) {
        LOG_ERROR(Render_Vulkan, "Error during pipeline cache write");
        return;
    }
    hen_driver_cache_bytes[program_id] = cache_data.size();
}

void PipelineCache::LoadDiskCache(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) {

    disk_caches.clear();
    curr_disk_cache =
        disk_caches.emplace_back(std::make_shared<ShaderDiskCache>(*this, GetProgramID()));

    curr_disk_cache->Init(stop_loading, callback);
}

void PipelineCache::EnsureDiskCache() {
    // The frontend loads the disk cache after loading a program (LoadDefaultDiskResources), but
    // System::Reset (a reboot requested by the program) creates a new renderer without it.
    if (!curr_disk_cache) {
        LoadDiskCache();
    }
}

void PipelineCache::SwitchDiskCache(u64 title_id, const std::atomic_bool& stop_loading,
                                    const VideoCore::DiskResourceLoadCallback& callback) {
    // NOTE: curr_disk_cache can be null if emulation restarted without calling
    // LoadDefaultDiskResources

    // Check if the current cache is for the specified TID.
    if (curr_disk_cache && curr_disk_cache->GetProgramID() == title_id) {
        return;
    }

    // Search for an existing manager
    size_t new_pos = 0;
    for (new_pos = 0; new_pos < disk_caches.size(); new_pos++) {
        if (disk_caches[new_pos]->GetProgramID() == title_id) {
            break;
        }
    }
    // Manager does not exist, create it and append to the end
    if (new_pos >= disk_caches.size()) {
        new_pos = disk_caches.size();
        auto& new_manager =
            disk_caches.emplace_back(std::make_shared<ShaderDiskCache>(*this, title_id));

        new_manager->Init(stop_loading, callback);
    }

    auto is_applet = [](u64 tid) {
        constexpr u32 APPLET_TID_HIGH = 0x00040030;
        return static_cast<u32>(tid >> 32) == APPLET_TID_HIGH;
    };

    bool prev_applet = curr_disk_cache ? is_applet(curr_disk_cache->GetProgramID()) : false;
    bool new_applet = is_applet(disk_caches[new_pos]->GetProgramID());
    curr_disk_cache = disk_caches[new_pos];

    if (prev_applet) {
        // If we came from an applet, clean up all other applets
        for (auto it = disk_caches.begin(); it != disk_caches.end();) {
            if (it == disk_caches.begin() || *it == curr_disk_cache ||
                !is_applet((*it)->GetProgramID())) {
                it++;
                continue;
            }
            it = disk_caches.erase(it);
        }
    }
    if (!new_applet) {
        // If we are going into a non-applet, clean up everything
        for (auto it = disk_caches.begin(); it != disk_caches.end();) {
            if (it == disk_caches.begin() || *it == curr_disk_cache) {
                it++;
                continue;
            }
            it = disk_caches.erase(it);
        }
    }
}

// (the two variants of the generic shader have their first pipelines apart)
static u64 HenGenericTargetsKey(const PipelineInfo& info, bool no_depth_write) {
    const u64 targets = Common::ComputeStructHash64(info.state.attachments);
    return no_depth_write ? targets ^ 0x9E3779B97F4A7C15ull : targets;
}

// The variant without the depth write is an improvement, not a need: the first module
// draws every draw correctly meanwhile. Its first pipeline for a pair of render target formats
// is 130 to 180 ms of a shader worker, and started with the first module's at the title's first
// draws it took the last free worker: the title's own first fragment shader waited in the queue
// behind four generic pipelines, the GPU thread waited for that shader (Build, WaitDone) and
// the emulation thread for the GPU thread, 146 ms (Mario Kart 7). So it starts
// only when the first module's pipeline for these targets is there, no other one of its kind
// is being built, and both worker pools have nothing queued or running at this instant. Asked
// again at every draw until then.
bool PipelineCache::HenGenericVariantMayStart(const PipelineInfo& info) {
    hen_generic_nd_wanted.try_emplace(HenGenericTargetsKey(info, true),
                                      std::chrono::steady_clock::now());
    if (hen_generic_nd_building.load(std::memory_order_relaxed) != 0) {
        return false;
    }
    const auto first = hen_generic_targets.find(HenGenericTargetsKey(info, false));
    if (first == hen_generic_targets.end() || !first->second->IsDone()) {
        return false;
    }
    return shader_workers.HenIdle() && pipeline_workers.HenIdle();
}

GraphicsPipeline* PipelineCache::HenGenericPipeline(PipelineInfo& info, bool no_depth_write) {
    Shader& generic_shader =
        no_depth_write ? hen_generic_nd_fragment_shader : hen_generic_fragment_shader;
    if (!hen_generic_fs || !generic_shader.IsDone() ||
        (no_depth_write && !__atomic_load_n(&hen_generic_early_depth, __ATOMIC_RELAXED))) {
        return nullptr;
    }
    const auto saved_shader = current_shaders[ProgramType::FS];
    const auto saved_hash = shader_hashes[ProgramType::FS];
    current_shaders[ProgramType::FS] = &generic_shader;
    shader_hashes[ProgramType::FS] = no_depth_write ? HEN_GENERIC_ND_FS_ID : HEN_GENERIC_FS_ID;
    for (u32 i = 0; i < MAX_SHADER_STAGES; i++) {
        info.state.shader_ids[i] = shader_hashes[i];
    }
    const u64 key = info.state.OptimizedHash(instance);
    auto [it, is_new] = hen_generic_pipelines.try_emplace(key);
    if (is_new) {
        it->second = std::make_unique<GraphicsPipeline>(instance, renderpass_cache, info,
                                                        *driver_pipeline_cache, *pipeline_layout,
                                                        current_shaders, &pipeline_workers);
    }
    GraphicsPipeline* const pipeline = it->second.get();
    current_shaders[ProgramType::FS] = saved_shader;
    shader_hashes[ProgramType::FS] = saved_hash;
    // (the driver refusing one, or leaving it unbuilt, must cost the feature, not the session;
    // for the variant without the depth write it costs the variant: its draws go back to the
    // first one)
    const auto build = [no_depth_write](GraphicsPipeline* p) {
        const char* const off = no_depth_write ? "its variant without the depth write off"
                                               : "generic fragment shader off";
        try {
            if (p->Build() && p->IsDone()) {
                return true;
            }
            std::printf("hen: gfx: a generic pipeline was not built: %s\n", off);
        } catch (const std::exception& e) {
            std::printf("hen: gfx: generic pipeline refused (%s): %s\n", e.what(), off);
        }
        __atomic_store_n(no_depth_write ? &hen_generic_early_depth : &hen_generic_fs, 0,
                         __ATOMIC_RELAXED);
        return false;
    };
    if (!pipeline->IsDone()) {
        // The slow part of one of these is the driver compiling the generic fragment shader,
        // once for a pair of render target formats (kept after that: the cache of parts in
        // vk_graphics_pipeline.cpp). The first pipeline for a pair is a worker's job and the
        // draw is left out meanwhile; the others are linked here from what is there.
        const u64 targets = HenGenericTargetsKey(info, no_depth_write);
        const auto ready = hen_generic_targets.find(targets);
        if (ready == hen_generic_targets.end()) {
            if (no_depth_write && !HenGenericVariantMayStart(info)) {
                return nullptr; // (held back: the caller takes the first module)
            }
            hen_generic_targets.emplace(targets, pipeline);
            if (no_depth_write) {
                hen_generic_nd_building.fetch_add(1, std::memory_order_relaxed);
            }
            const auto wanted = no_depth_write ? hen_generic_nd_wanted[targets]
                                               : std::chrono::steady_clock::time_point{};
            shader_workers.QueueWork([this, pipeline, build, no_depth_write, wanted] {
                const auto before = std::chrono::steady_clock::now();
                const bool built = build(pipeline);
                const auto after = std::chrono::steady_clock::now();
                const double took = std::chrono::duration<double, std::milli>(after - before).count();
                if (built && no_depth_write) {
                    std::printf("hen: gfx: generic pipeline (without the depth write) for new render "
                                "targets built in %.1f ms; ready %.2f s after the renderer's start, "
                                "held back %.0f ms for the first module's pipeline and idle workers\n",
                                took, std::chrono::duration<double>(after - hen_created).count(),
                                std::chrono::duration<double, std::milli>(before - wanted).count());
                } else if (built) {
                    std::printf("hen: gfx: generic pipeline for new render targets built in %.1f ms\n",
                                took);
                }
                if (no_depth_write) {
                    hen_generic_nd_building.fetch_sub(1, std::memory_order_relaxed);
                }
            });
        } else if (ready->second->IsDone() && pipeline != ready->second) {
            HEN_STAT(const auto before = std::chrono::steady_clock::now();)
            build(pipeline);
            HEN_STAT(hen_generic_stats[1]++;)
            HEN_STAT(hen_generic_stats[2] += static_cast<unsigned long long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - before)
                    .count());)
        }
    }
    return pipeline->IsDone() ? pipeline : nullptr;
}

bool PipelineCache::HenBindGeneric(PipelineInfo& info, GraphicsPipeline* pipeline,
                                   bool no_depth_write) {
    const auto saved_hash = shader_hashes[ProgramType::FS];
    // (BindPipeline copies the ids into info)
    shader_hashes[ProgramType::FS] = no_depth_write ? HEN_GENERIC_ND_FS_ID : HEN_GENERIC_FS_ID;
    hen_forced_pipeline = pipeline;
    const bool bound = BindPipeline(info, true);
    hen_forced_pipeline = nullptr;
    shader_hashes[ProgramType::FS] = saved_hash;
    HEN_STAT(hen_generic_stats[0]++;)
    return bound;
}

void PipelineCache::HenSeedGeneric(const PipelineInfo& info) {
    if (!hen_generic_fs || !hen_generic_seed) {
        return;
    }
    // (each variant when its own module is there: the second is compiled after the first)
    for (const bool no_depth_write : {false, true}) {
        Shader& generic_shader =
            no_depth_write ? hen_generic_nd_fragment_shader : hen_generic_fragment_shader;
        if (!generic_shader.IsDone()) {
            continue;
        }
        const u64 targets = HenGenericTargetsKey(info, no_depth_write);
        if (targets == hen_generic_seeded[no_depth_write]) {
            continue;
        }
        Shader* const vertex_shader = current_shaders[ProgramType::VS];
        if (vertex_shader == nullptr || !vertex_shader->IsDone()) {
            return; // (a later draw will do)
        }
        if (hen_generic_targets.contains(targets)) {
            hen_generic_seeded[no_depth_write] = targets;
            continue;
        }
        // (the variant waits its turn, and is asked for again at the next draw)
        if (no_depth_write && !HenGenericVariantMayStart(info)) {
            continue;
        }
        hen_generic_seeded[no_depth_write] = targets;
        PipelineInfo copy = info;
        HenGenericPipeline(copy, no_depth_write);
    }
}

void PipelineCache::HenDropUnbuiltGeneric() {
    // (before the driver's pipeline cache is replaced: a generic pipeline not built yet holds
    // the old cache's handle. Built ones stay: a pipeline needs no cache once it exists.)
    hen_generic_seeded = {};
    hen_generic_nd_wanted.clear();
    shader_workers.WaitForRequests();
    for (auto it = hen_generic_targets.begin(); it != hen_generic_targets.end();) {
        it = it->second->IsDone() ? std::next(it) : hen_generic_targets.erase(it);
    }
    for (auto it = hen_generic_pipelines.begin(); it != hen_generic_pipelines.end();) {
        it = it->second->IsDone() ? std::next(it) : hen_generic_pipelines.erase(it);
    }
}

bool PipelineCache::BindPipeline(PipelineInfo& info, bool wait_built, bool fast_lane) {
    MICROPROFILE_SCOPE(Vulkan_Bind);

    for (u32 i = 0; i < MAX_SHADER_STAGES; i++) {
        info.state.shader_ids[i] = shader_hashes[i];
    }

    EnsureDiskCache();
#ifdef __SWITCH__
    curr_disk_cache->HenRegenPoll();
#endif
    GraphicsPipeline* pipeline =
        hen_forced_pipeline ? hen_forced_pipeline : curr_disk_cache->GetPipeline(info);
    if (!pipeline->IsDone() && !pipeline->TryBuild(wait_built, fast_lane)) {
        hen_pending_pipeline = pipeline;
        return false;
    }

    const bool is_dirty = scheduler.IsStateDirty(StateFlags::Pipeline);
    const bool pipeline_dirty = (current_pipeline != pipeline) || is_dirty;
    scheduler.Record([this, is_dirty, pipeline_dirty, pipeline,
                      current_dynamic = current_info.dynamic_info, dynamic = info.dynamic_info,
                      descriptor_sets = bound_descriptor_sets, offsets = offsets,
                      current_rasterization = current_info.state.rasterization,
                      current_depth_stencil = current_info.state.depth_stencil,
                      rasterization = info.state.rasterization,
                      depth_stencil = info.state.depth_stencil](vk::CommandBuffer cmdbuf) {
        if (dynamic.viewport != current_dynamic.viewport ||
            dynamic.min_depth != current_dynamic.min_depth ||
            dynamic.max_depth != current_dynamic.max_depth || is_dirty) {
            // (hen, --early-depth: the depth range is 0..1 unless the draw's fragment shader
            // leaves depth to the rasteriser; vk_rasterizer.cpp, HenEarlyDepth)
            const vk::Viewport vk_viewport = {
                .x = static_cast<f32>(dynamic.viewport.left),
                .y = static_cast<f32>(dynamic.viewport.top),
                .width = static_cast<f32>(dynamic.viewport.GetWidth()),
                .height = static_cast<f32>(dynamic.viewport.GetHeight()),
                .minDepth = dynamic.min_depth,
                .maxDepth = dynamic.max_depth,
            };
            cmdbuf.setViewport(0, vk_viewport);
        }

        if (instance.HenCoarseShading() != 0 &&
            (dynamic.shading_rate != current_dynamic.shading_rate || is_dirty)) {
            const u32 side = dynamic.shading_rate >= 4 && instance.HenCoarseShading() >= 4 ? 4
                             : dynamic.shading_rate >= 2 ? 2 : 1;
            const vk::Extent2D size{side, side};
            const vk::FragmentShadingRateCombinerOpKHR combiners[2] = {
                vk::FragmentShadingRateCombinerOpKHR::eKeep,
                vk::FragmentShadingRateCombinerOpKHR::eKeep,
            };
            cmdbuf.setFragmentShadingRateKHR(size, combiners);
        }

        if (dynamic.scissor != current_dynamic.scissor || is_dirty) {
            const vk::Rect2D scissor = {
                .offset{
                    .x = static_cast<s32>(dynamic.scissor.left),
                    .y = static_cast<s32>(dynamic.scissor.bottom),
                },
                .extent{
                    .width = dynamic.scissor.GetWidth(),
                    .height = dynamic.scissor.GetHeight(),
                },
            };
            cmdbuf.setScissor(0, scissor);
        }

        if (dynamic.stencil_compare_mask != current_dynamic.stencil_compare_mask || is_dirty) {
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         dynamic.stencil_compare_mask);
        }

        if (dynamic.stencil_write_mask != current_dynamic.stencil_write_mask || is_dirty) {
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       dynamic.stencil_write_mask);
        }

        if (dynamic.stencil_reference != current_dynamic.stencil_reference || is_dirty) {
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       dynamic.stencil_reference);
        }

        if (dynamic.blend_color != current_dynamic.blend_color || is_dirty) {
            const Common::Vec4f color = PicaToVK::ColorRGBA8(dynamic.blend_color);
            cmdbuf.setBlendConstants(color.AsArray());
        }

        if (instance.IsExtendedDynamicStateSupported()) {
            const bool needs_flip =
                rasterization.flip_viewport != current_rasterization.flip_viewport;
            if (rasterization.cull_mode != current_rasterization.cull_mode || needs_flip ||
                is_dirty) {
                cmdbuf.setCullModeEXT(
                    PicaToVK::CullMode(rasterization.cull_mode, rasterization.flip_viewport));
                cmdbuf.setFrontFaceEXT(PicaToVK::FrontFace(rasterization.cull_mode));
            }

            if (depth_stencil.depth_compare_op != current_depth_stencil.depth_compare_op ||
                is_dirty) {
                cmdbuf.setDepthCompareOpEXT(PicaToVK::CompareFunc(depth_stencil.depth_compare_op));
            }

            if (depth_stencil.depth_test_enable != current_depth_stencil.depth_test_enable ||
                is_dirty) {
                cmdbuf.setDepthTestEnableEXT(depth_stencil.depth_test_enable);
            }

            if (depth_stencil.depth_write_enable != current_depth_stencil.depth_write_enable ||
                is_dirty) {
                cmdbuf.setDepthWriteEnableEXT(depth_stencil.depth_write_enable);
            }

            if (rasterization.topology != current_rasterization.topology || is_dirty) {
                cmdbuf.setPrimitiveTopologyEXT(PicaToVK::PrimitiveTopology(rasterization.topology));
            }

            if (depth_stencil.stencil_test_enable != current_depth_stencil.stencil_test_enable ||
                is_dirty) {
                cmdbuf.setStencilTestEnableEXT(depth_stencil.stencil_test_enable);
            }

            if (depth_stencil.stencil_fail_op != current_depth_stencil.stencil_fail_op ||
                depth_stencil.stencil_pass_op != current_depth_stencil.stencil_pass_op ||
                depth_stencil.stencil_depth_fail_op !=
                    current_depth_stencil.stencil_depth_fail_op ||
                depth_stencil.stencil_compare_op != current_depth_stencil.stencil_compare_op ||
                is_dirty) {
                cmdbuf.setStencilOpEXT(vk::StencilFaceFlagBits::eFrontAndBack,
                                       PicaToVK::StencilOp(depth_stencil.stencil_fail_op),
                                       PicaToVK::StencilOp(depth_stencil.stencil_pass_op),
                                       PicaToVK::StencilOp(depth_stencil.stencil_depth_fail_op),
                                       PicaToVK::CompareFunc(depth_stencil.stencil_compare_op));
            }
        }

        if (pipeline_dirty) {
            if (!pipeline->IsDone()) {
                HenGfxTimer hen_timer{HEN_GFX_PIPELINE_WAIT};
                pipeline->WaitDone();
            }
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
        }

        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0,
                                  descriptor_sets, offsets);
    });

    current_info = info;
    current_pipeline = pipeline;
    scheduler.MarkStateNonDirty(StateFlags::Pipeline | StateFlags::DescriptorSets);

    return true;
}

ExtraVSConfig PipelineCache::CalcExtraConfig(const PicaVSConfig& config) {
    auto res = ExtraVSConfig();

    // Enable the geometry-shader only if we are actually doing per-fragment lighting
    // and care about proper quaternions. Otherwise just use standard vertex+fragment shaders.
    // We also don't need the geometry shader if we have the barycentric extension.
    const bool use_geometry_shader = instance.UseGeometryShaders() &&
                                     !config.state.lighting_disable &&
                                     !instance.IsFragmentShaderBarycentricSupported();

    res.use_clip_planes = instance.IsShaderClipDistanceSupported();
    res.use_geometry_shader = use_geometry_shader;
    res.sanitize_mul = profile.enable_accurate_mul;
    res.separable_shader = true;
    res.load_flags.fill(AttribLoadFlags::Float);

    for (u32 i = 0; i < config.state.used_input_vertex_attributes; i++) {
        const auto& attr = config.state.input_vertex_attributes[i];
        const u32 location = attr.location;
        const Pica::PipelineRegs::VertexAttributeFormat type =
            static_cast<Pica::PipelineRegs::VertexAttributeFormat>(attr.type);
        const FormatTraits& traits = instance.GetTraits(type, attr.size);
        AttribLoadFlags& flags = res.load_flags[location];

        if (traits.needs_conversion) {
            flags = MakeAttribLoadFlag(type);
        }
        if (traits.needs_emulation) {
            flags |= AttribLoadFlags::ZeroW;
        }
    }

    return res;
}

bool PipelineCache::UseProgrammableVertexShader(const Pica::RegsInternal& regs,
                                                Pica::ShaderSetup& setup,
                                                const VertexLayout& layout) {

    EnsureDiskCache();
    auto res = curr_disk_cache->UseProgrammableVertexShader(regs, setup, layout);

    if (res.has_value()) {
        current_shaders[ProgramType::VS] = (*res).second;
        shader_hashes[ProgramType::VS] = (*res).first;
        return true;
    }

    return false;
}

void PipelineCache::UseTrivialVertexShader() {
    current_shaders[ProgramType::VS] = &trivial_vertex_shader;
    shader_hashes[ProgramType::VS] = 0;
}

bool PipelineCache::UseFixedGeometryShader(const Pica::RegsInternal& regs) {

    EnsureDiskCache();
    auto res = curr_disk_cache->UseFixedGeometryShader(regs);

    if (res.has_value()) {
        current_shaders[ProgramType::GS] = (*res).second;
        shader_hashes[ProgramType::GS] = (*res).first;
        return true;
    }

    return false;
}

void PipelineCache::UseTrivialGeometryShader() {
    current_shaders[ProgramType::GS] = nullptr;
    shader_hashes[ProgramType::GS] = 0;
}

void PipelineCache::UseFragmentShader(const Pica::RegsInternal& regs,
                                      const Pica::Shader::UserConfig& user, bool no_depth_write) {

    EnsureDiskCache();
    auto res = curr_disk_cache->UseFragmentShader(regs, user, no_depth_write);

    if (res.has_value()) {
        current_shaders[ProgramType::FS] = (*res).second;
        shader_hashes[ProgramType::FS] = (*res).first;
    }
}

bool PipelineCache::IsCacheValid(std::span<const u8> data) const {
    if (data.size() < sizeof(vk::PipelineCacheHeaderVersionOne)) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Invalid header");
        return false;
    }

    vk::PipelineCacheHeaderVersionOne header;
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.headerSize < sizeof(header)) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Invalid header length");
        return false;
    }

    if (header.headerVersion != vk::PipelineCacheHeaderVersion::eOne) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Invalid header version");
        return false;
    }

    if (u32 vendor_id = instance.GetVendorID(); header.vendorID != vendor_id) {
        LOG_ERROR(
            Render_Vulkan,
            "Pipeline cache failed validation: Incorrect vendor ID (file: {:#X}, device: {:#X})",
            header.vendorID, vendor_id);
        return false;
    }

    if (u32 device_id = instance.GetDeviceID(); header.deviceID != device_id) {
        LOG_ERROR(
            Render_Vulkan,
            "Pipeline cache failed validation: Incorrect device ID (file: {:#X}, device: {:#X})",
            header.deviceID, device_id);
        return false;
    }

    if (header.pipelineCacheUUID != instance.GetPipelineCacheUUID()) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Incorrect UUID");
        return false;
    }

    return true;
}

bool PipelineCache::EnsureDirectories() const {
    const auto create_dir = [](const std::string& dir) {
        if (!FileUtil::CreateDir(dir)) {
            LOG_ERROR(Render_Vulkan, "Failed to create directory={}", dir);
            return false;
        }

        return true;
    };

    return create_dir(FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir)) &&
           create_dir(GetVulkanDir()) && create_dir(GetPipelineCacheDir()) &&
           create_dir(GetTransferableDir());
}

std::string PipelineCache::GetVulkanDir() const {
    return FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) + "vulkan" + DIR_SEP;
}

std::string PipelineCache::GetPipelineCacheDir() const {
    return GetVulkanDir() + "pipeline" + DIR_SEP;
}

std::string PipelineCache::GetTransferableDir() const {
    return GetVulkanDir() + DIR_SEP + "transferable";
}

} // namespace Vulkan
