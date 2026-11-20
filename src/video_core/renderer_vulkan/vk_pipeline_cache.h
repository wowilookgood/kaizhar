// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <chrono>
#include <unordered_map>

#include <bitset>

#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_shader_disk_cache.h"
#include "video_core/shader/generator/pica_fs_config.h"
#include "video_core/shader/generator/profile.h"
#include "video_core/shader/generator/shader_gen.h"

namespace Pica {
struct RegsInternal;
struct ShaderSetup;
} // namespace Pica

namespace Vulkan {

class Instance;
class Scheduler;
class RenderManager;
class DescriptorUpdateQueue;

enum class DescriptorHeapType : u32 {
    Buffer,
    Texture,
    Utility,
};

/**
 * Stores a collection of rasterizer pipelines used during rendering.
 */
class PipelineCache {
    static constexpr u32 NumRasterizerSets = 3;
    static constexpr u32 NumDescriptorHeaps = 3;
    static constexpr u32 NumDynamicOffsets = 3;

public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           RenderManager& renderpass_cache, DescriptorUpdateQueue& update_queue);
    ~PipelineCache();

    /// Acquires and binds a free descriptor set from the appropriate heap.
    vk::DescriptorSet Acquire(DescriptorHeapType type) {
        const u32 index = static_cast<u32>(type);
        const auto descriptor_set = descriptor_heaps[index].Commit();
        bound_descriptor_sets[index] = descriptor_set;
        return descriptor_set;
    }

    /// Sets the dynamic offset for the uniform buffer at binding
    void UpdateRange(u8 binding, u32 offset) {
        offsets[binding] = offset;
    }

    /// Loads the driver pipeline cache and the disk shader cache
    void LoadCache(const std::atomic_bool& stop_loading = std::atomic_bool{false},
                   const VideoCore::DiskResourceLoadCallback& callback = {});

    /// Switches the driver pipeline cache and the shader disk cache to the specified title
    void SwitchCache(u64 title_id, const std::atomic_bool& stop_loading = std::atomic_bool{false},
                     const VideoCore::DiskResourceLoadCallback& callback = {});

    /// Binds a pipeline using the provided information
    bool BindPipeline(PipelineInfo& info, bool wait_built = false, bool fast_lane = false);

    /// (Switch 2 port) The pipeline the last BindPipeline that returned false was for.
    GraphicsPipeline* HenPendingPipeline() const {
        return hen_pending_pipeline;
    }

    Pica::Shader::Generator::ExtraVSConfig CalcExtraConfig(
        const Pica::Shader::Generator::PicaVSConfig& config);

    /// Binds a PICA decompiled vertex shader
    bool UseProgrammableVertexShader(const Pica::RegsInternal& regs, Pica::ShaderSetup& setup,
                                     const VertexLayout& layout);

    /// Binds a passthrough vertex shader
    void UseTrivialVertexShader();

    /// Binds a PICA decompiled geometry shader
    bool UseFixedGeometryShader(const Pica::RegsInternal& regs);

    /// Binds a passthrough geometry shader
    void UseTrivialGeometryShader();

    /// (hen) The pipeline with the generic fragment shader for a draw whose vertices were
    /// shaded on the CPU (the trivial vertex shader is current), or null if it is not ready:
    /// the first one for a pair of render target formats is built by a worker.
    /// no_depth_write: with the variant of the shader that leaves depth to the rasteriser, for
    /// a draw whose viewport carries the PICA depth range (early depth). Null too while that
    /// variant is not ready or is switched off: the caller then takes the other one, which
    /// is right for every draw under the range 0..1.
    GraphicsPipeline* HenGenericPipeline(PipelineInfo& info, bool no_depth_write = false);
    /// (hen) Binds it (after the configuration is in the uniforms).
    bool HenBindGeneric(PipelineInfo& info, GraphicsPipeline* pipeline,
                        bool no_depth_write = false);
    void HenDropUnbuiltGeneric();
    /// (hen) Called for every draw: the first one to a pair of render target formats starts
    /// the generic pipeline for them on a worker, so it is there (about 200 ms later) before
    /// a draw needs it. Whatever vertex shader is current will do: the slow part is the
    /// fragment shader's, kept for every pipeline that follows. Both variants of the shader
    /// are started: the first as soon as its module is there, the variant without the depth
    /// write when it competes with nothing (HenGenericVariantMayStart).
    void HenSeedGeneric(const PipelineInfo& info);
    const Pica::Shader::Profile& HenProfile() const {
        return profile;
    }

    /// Binds a fragment shader generated from PICA state
    /// (hen) no_depth_write: the variant that leaves depth to the rasteriser (--early-depth)
    void UseFragmentShader(const Pica::RegsInternal& regs, const Pica::Shader::UserConfig& user,
                           bool no_depth_write = false);

    /// Gets the current program ID
    u64 GetProgramID() const {
        return current_program_id;
    }

    void SetProgramID(u64 program_id) {
        current_program_id = program_id;
    }

    void SetAccurateMul(bool _accurate_mul) {
        profile.enable_accurate_mul = _accurate_mul;
    }

private:
    friend ShaderDiskCache;

    /// Loads the driver pipeline cache
    void LoadDriverPipelineDiskCache(const std::atomic_bool& stop_loading = std::atomic_bool{false},
                                     const VideoCore::DiskResourceLoadCallback& callback = {});

    /// Stores the generated pipeline cache
    void SaveDriverPipelineDiskCache();

    /// (Switch 2 port) Stores one title's driver pipeline cache. only_if_grown: not written if
    /// it is as long as when it was read from the card or last written.
    void HenSaveDriverPipelineCache(u64 program_id, vk::PipelineCache cache, bool only_if_grown);

    /// Loads the shader disk cache
    /// Loads the disk cache if no cache is current yet
    void EnsureDiskCache();

    void LoadDiskCache(const std::atomic_bool& stop_loading = std::atomic_bool{false},
                       const VideoCore::DiskResourceLoadCallback& callback = {});

    /// Switches the disk cache at runtime to use a different title ID
    void SwitchDiskCache(u64 title_id, const std::atomic_bool& stop_loading,
                         const VideoCore::DiskResourceLoadCallback& callback);

    /// Builds the rasterizer pipeline layout
    void BuildLayout();

    /// Returns true when the disk data can be used by the current driver
    bool IsCacheValid(std::span<const u8> cache_data) const;

    /// Create pipeline cache directories. Returns true on success.
    bool EnsureDirectories() const;

    /// Returns the Vulkan shader directory
    std::string GetVulkanDir() const;

    /// Returns the pipeline cache storage dir
    std::string GetPipelineCacheDir() const;

    /// Returns the transferable shader dir
    std::string GetTransferableDir() const;

private:
    const Instance& instance;
    Scheduler& scheduler;
    RenderManager& renderpass_cache;
    DescriptorUpdateQueue& update_queue;

    Pica::Shader::Profile profile{};
    vk::UniquePipelineCache driver_pipeline_cache;
    /// (Switch 2 port, hen_driver_cache_keep) The driver pipeline caches of the titles that
    /// held the GPU right earlier in this run and do not now, by title: see SwitchCache.
    std::unordered_map<u64, vk::UniquePipelineCache> hen_parked_driver_caches;
    /// Bytes of each title's driver pipeline cache when it was read from the card or last
    /// written there.
    std::unordered_map<u64, std::size_t> hen_driver_cache_bytes;
    unsigned hen_driver_cache_switches[2]{}; ///< taken from memory, read from the card
    vk::UniquePipelineLayout pipeline_layout;
    std::size_t num_worker_threads;
    Common::ThreadWorker pipeline_workers;
    Common::ThreadWorker shader_workers;
    PipelineInfo current_info{};
    GraphicsPipeline* current_pipeline{};
    GraphicsPipeline* hen_pending_pipeline{};
    /// (hen) The generic fragment shader (GenerateGenericFragmentShader) and its pipelines, by
    /// state. Kept apart from the disk cache's: they are not the program's own and must not be
    /// written to its files.
    std::unordered_map<u64, std::unique_ptr<GraphicsPipeline>> hen_generic_pipelines;
    GraphicsPipeline* hen_forced_pipeline{};
    /// (the first generic pipeline for each pair of render target formats and each variant
    /// of the shader: HenGenericTargetsKey)
    std::unordered_map<u64, GraphicsPipeline*> hen_generic_targets;
    std::array<u64, 2> hen_generic_seeded{}; ///< (by variant: with, without the depth write)
    /// (hen) The variant's first pipeline for a pair of render target formats is held
    /// back until it competes with nothing (HenGenericVariantMayStart): when it was first
    /// wanted, by HenGenericTargetsKey; how many are being built (one at a time).
    bool HenGenericVariantMayStart(const PipelineInfo& info);
    std::unordered_map<u64, std::chrono::steady_clock::time_point> hen_generic_nd_wanted;
    std::atomic<u32> hen_generic_nd_building{0};
    std::chrono::steady_clock::time_point hen_created = std::chrono::steady_clock::now();
    std::array<DescriptorHeap, NumDescriptorHeaps> descriptor_heaps;
    std::array<vk::DescriptorSet, NumRasterizerSets> bound_descriptor_sets{};
    std::array<u32, NumDynamicOffsets> offsets{};

    std::array<u64, MAX_SHADER_STAGES> shader_hashes;
    std::array<Shader*, MAX_SHADER_STAGES> current_shaders;

    Shader trivial_vertex_shader;
    Shader hen_generic_fragment_shader;
    Shader hen_generic_nd_fragment_shader; ///< (the variant without the depth write)

    u64 current_program_id{0};
    std::vector<std::shared_ptr<ShaderDiskCache>> disk_caches;
    std::shared_ptr<ShaderDiskCache> curr_disk_cache{};
};

} // namespace Vulkan
