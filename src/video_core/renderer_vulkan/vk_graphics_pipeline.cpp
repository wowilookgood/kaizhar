// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <unordered_map>
#include <boost/container/static_vector.hpp>

#include "common/alignment.h"
#include "common/hash.h"
#include <mutex>
#include <unordered_set>
#include "common/hen_gap.h"
#include "common/hen_stats.h"
#include "common/microprofile.h"
#include "video_core/renderer_vulkan/pica_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_render_manager.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#ifdef __SWITCH__
#include <cstdio>
#include <map>
#include <mutex>
#include <utility>
#include <vector>
#include <set>
#if HEN_DEV
extern "C" int hen_pipe_log; // hen/jit/hen_jit.c: --pipe-log
extern "C" void hen_jit_profile_retarget(void);
#else
static constexpr int hen_pipe_log = 0;
#endif
extern "C" int hen_pipeline_libraries;
/// How often a pre-rasterization or fragment part is asked of the driver that is identical to
/// one asked before (same shader modules, state, layout and render pass), and the time those
/// requests take: what keeping the parts and sharing them between pipelines would save.
HEN_STAT(static std::mutex hen_repeat_mutex;)
HEN_STAT(static std::unordered_set<u64> hen_part_seen[2];)
// The generic fragment shader's part of a pipeline, kept for reuse. The driver keeps the
// vertex side of a pipeline in its cache and gives a repeat back in microseconds, but a
// repeat of a fragment shader part costs as much as the first. (Keeping every fragment
// shader's part as well gives fewer requests to the driver and leaves time and counts
// unchanged.) Keyed by the fragment shader's id (not its module: a handle can come back for
// another shader; the variant without the depth write has an id of its own, so its parts
// are kept apart), layout, render pass and sample count, and by the depth state only where
// that is not dynamic state. Emptied when the pipeline cache goes (HenDestroyPipelineParts).
extern "C" unsigned long long hen_part_cache_hits;
static std::mutex hen_part_cache_mutex;
static std::unordered_map<u64, VkPipeline> hen_part_cache_map;
#if HEN_DEV
static std::array<std::atomic<u64>, 2> hen_part_requests{}, hen_part_repeats{}, hen_part_repeat_ns{};
extern "C" void hen_gfx_part_repeat_stats(unsigned long long* out) {
    for (int i = 0; i < 2; i++) {
        out[3 * i] = hen_part_requests[i].exchange(0);
        out[3 * i + 1] = hen_part_repeats[i].exchange(0);
        out[3 * i + 2] = hen_part_repeat_ns[i].exchange(0);
    }
}
#endif
static u64 HenHashBytes(u64 h, const void* data, std::size_t size) {
    const u8* bytes = static_cast<const u8*>(data);
    for (std::size_t i = 0; i < size; i++) {
        h = (h ^ bytes[i]) * 0x100000001b3ull;
    }
    return h;
}
#if HEN_DEV
/// Time in each of the four parts of pipelines the driver had to compile (not its cache hits).
static std::array<std::atomic<u64>, 4> hen_part_ns{}, hen_part_count{};
extern "C" void hen_gfx_part_stats(unsigned long long* out) {
    for (int i = 0; i < 4; i++) {
        out[2 * i] = hen_part_count[i].exchange(0);
        out[2 * i + 1] = hen_part_ns[i].exchange(0);
    }
}
#endif
#endif

#include <chrono>
extern "C" unsigned long long hen_standin_stats[7]; // hen_jit.c
extern "C" unsigned long long hen_standin_part_ns[6];
extern "C" __thread int hen_standin_build;

namespace Vulkan {

MICROPROFILE_DEFINE(Vulkan_Pipeline, "Vulkan", "Pipeline Building", MP_RGB(0, 192, 32));

vk::ShaderStageFlagBits MakeShaderStage(std::size_t index) {
    switch (index) {
    case 0:
        return vk::ShaderStageFlagBits::eVertex;
    case 1:
        return vk::ShaderStageFlagBits::eFragment;
    case 2:
        return vk::ShaderStageFlagBits::eGeometry;
    default:
        LOG_CRITICAL(Render_Vulkan, "Invalid shader stage index!");
        UNREACHABLE();
    }
    return vk::ShaderStageFlagBits::eVertex;
}

u64 StaticPipelineInfo::OptimizedHash(const Instance& instance) const {
    u64 info_hash = Common::HashCombine(
        shader_ids[0], shader_ids[1], shader_ids[2], Common::ComputeStructHash64(vertex_layout),
        Common::ComputeStructHash64(attachments), Common::ComputeStructHash64(blending));

    if (!instance.IsExtendedDynamicStateSupported()) {
        info_hash = Common::HashCombine(info_hash, Common::ComputeStructHash64(rasterization),
                                        Common::ComputeStructHash64(depth_stencil));
    }

    return info_hash;
}

u16 PipelineInfo::GetFinalColorWriteMask(const Instance& instance) {
    u16 color_write_mask = state.blending.color_write_mask;
    const bool is_logic_op_emulated =
        instance.NeedsLogicOpEmulation() && !state.blending.blend_enable;
    const bool is_logic_op_noop = state.blending.logic_op == Pica::FramebufferRegs::LogicOp::NoOp;
    if (is_logic_op_emulated && is_logic_op_noop) {
        // Color output is disabled by logic operation. We use color write mask to skip
        // color but allow depth write.
        color_write_mask = 0;
    }
    return color_write_mask;
}

Shader::Shader(const Instance& instance) : device{instance.GetDevice()} {}

Shader::Shader(const Instance& instance, vk::ShaderStageFlagBits stage, std::string code)
    : Shader{instance} {
    module = Compile(code, stage, instance.GetDevice());
    MarkDone();
}

Shader::~Shader() {
    if (device && module) {
        device.destroyShaderModule(module);
    }
}

GraphicsPipeline::GraphicsPipeline(const Instance& instance_, RenderManager& renderpass_cache_,
                                   const PipelineInfo& info_, vk::PipelineCache pipeline_cache_,
                                   vk::PipelineLayout layout_, std::array<Shader*, 3> stages_,
                                   Common::ThreadWorker* worker_)
    : instance{instance_}, renderpass_cache{renderpass_cache_}, worker{worker_},
      pipeline_layout{layout_}, pipeline_cache{pipeline_cache_}, info{info_}, stages{stages_} {}

GraphicsPipeline::~GraphicsPipeline() = default;

#ifdef __SWITCH__
void HenDestroyPipelineParts(vk::Device device) {
    std::scoped_lock lock{hen_part_cache_mutex};
    for (const auto& [key, part] : hen_part_cache_map) {
        device.destroyPipeline(vk::Pipeline{part});
    }
    hen_part_cache_map.clear();
}
#endif

#ifdef __SWITCH__
namespace {
std::atomic<unsigned long long> hen_count[HEN_GFX_KINDS], hen_ns[HEN_GFX_KINDS],
    hen_max[HEN_GFX_KINDS];
/// Set around a Build that the caller of TryBuild waits for on its own thread.
thread_local bool hen_build_here = false;
}
// (hen/jit/hen_jit.c) those builds: count, ns, longest ns; the frontend's report empties them
extern "C" unsigned long long hen_here_builds[3];
extern "C" void hen_gfx_count(int kind, unsigned long long ns) {
    if (kind < 0 || kind >= HEN_GFX_KINDS) {
        return;
    }
#if !HEN_DEV
    // (a release build reads one kind: the pipelines compiled, for the session report)
    if (kind != HEN_GFX_PIPELINE_COMPILE) {
        return;
    }
#endif
    hen_count[kind]++;
    hen_ns[kind] += ns;
    // (for the line about a gap between two frames: common/hen_gap.h)
    switch (kind) {
    // (a build on the renderer's own thread is that thread standing still for a pipeline:
    // it goes with the waits, not with the workers' summed time)
    case HEN_GFX_PIPELINE_COMPILE:
        hen_gap_add(hen_build_here ? HEN_GAP_PIPELINE_WAIT : HEN_GAP_PIPELINES, ns);
        break;
    case HEN_GFX_SHADER_COMPILE: hen_gap_add(HEN_GAP_SHADERS, ns); break;
    case HEN_GFX_PIPELINE_WAIT: hen_gap_add(HEN_GAP_PIPELINE_WAIT, ns); break;
    case HEN_GFX_SOFTWARE_DRAW: hen_gap_add(HEN_GAP_CPU_DRAWS, ns); break;
    case HEN_GFX_DOWNLOAD_FINISH: hen_gap_add(HEN_GAP_DOWNLOADS, ns); break;
    case HEN_GFX_GPU_WAIT: hen_gap_add(HEN_GAP_DRIVER_WAIT, ns); break;
    case HEN_GFX_MEM_FLUSH: hen_gap_add(HEN_GAP_CACHE_FLUSH, ns); break;
    case HEN_GFX_FILE_SYNC: hen_gap_add(HEN_GAP_TITLE_READS, ns); break;
    default: break;
    }
    if (ns > hen_max[kind].load(std::memory_order_relaxed)) {
        hen_max[kind].store(ns, std::memory_order_relaxed);
    }
}
#if HEN_DEV
/// Adds to a kind's count only (events counted elsewhere without atomics).
extern "C" void hen_gfx_add(int kind, unsigned long long count) {
    if (kind >= 0 && kind < HEN_GFX_KINDS) {
        hen_count[kind] += count;
    }
}
#endif
/// Per kind: count, total and longest time in nanoseconds since the last call.
extern "C" void hen_gfx_stats(unsigned long long* out) {
    for (int k = 0; k < HEN_GFX_KINDS; k++) {
        out[3 * k] = hen_count[k].exchange(0);
        out[3 * k + 1] = hen_ns[k].exchange(0);
        out[3 * k + 2] = hen_max[k].exchange(0);
    }
}
#endif

bool GraphicsPipeline::TryBuild(bool wait_built, bool fast_lane) {
    // The pipeline is currently being compiled. We can either wait for it
    // or skip the draw.
    if (is_pending) {
        return wait_built && !fast_lane;
    }

    // If the shaders haven't been compiled yet, we cannot proceed.
    const bool shaders_pending = std::any_of(
        stages.begin(), stages.end(), [](Shader* shader) { return shader && !shader->IsDone(); });
    if ((!wait_built || fast_lane) && shaders_pending) {
        return false;
    }

    // Ask the driver if it can give us the pipeline quickly.
    if (!shaders_pending && instance.IsPipelineCreationCacheControlSupported() && Build(true)) {
        return true;
    }

#ifdef __SWITCH__
    // (Switch 2 port) fast_lane: the pipeline for a draw whose vertices were shaded on the CPU
    // because its own pipeline is compiling. It has no vertex shader of the program's, so it
    // takes milliseconds, but building it here stood the emulation thread still at scene
    // starts (several fragment shaders in one frame, 140 ms). It is built by the shader
    // workers, which are not behind the slow pipelines, and the draw is left out until it is
    // done: a frame or two.
    if (fast_lane && hen_fast_worker != nullptr) {
        if (!is_pending && !shaders_pending) {
            hen_fast_worker->QueueWork([this] {
                HEN_STAT(const auto before = std::chrono::steady_clock::now();)
                hen_standin_build = 1;
                Build();
                hen_standin_build = 0;
                HEN_STAT(const unsigned long long took = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    std::chrono::steady_clock::now() - before)
                                                    .count();
                __atomic_fetch_add(&hen_standin_stats[3], 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&hen_standin_stats[4], took, __ATOMIC_RELAXED);
                if (took > hen_standin_stats[5]) {
                    hen_standin_stats[5] = took;
                })
            });
            is_pending = true;
        }
        return false;
    }
    // (Switch 2 port) A pipeline the caller is going to wait for is built here, on its thread,
    // rather than behind whatever the workers have queued: the pipelines for vertices shaded on
    // the CPU take about 13 ms to build but waited over 600 ms behind the program's vertex
    // shaders (260 ms each on this driver) in a first launch.
    // The same when its shaders are still being compiled (by the shader workers, which are
    // not behind the pipelines): Build waits for them, a few tens of milliseconds at most.
    if (wait_built) {
        // (counted apart: "hen: gfx: pipelines built on the GPU thread")
        const auto here_start = std::chrono::steady_clock::now();
        hen_build_here = true;
        Build();
        hen_build_here = false;
        const unsigned long long here_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 here_start)
                .count();
        __atomic_fetch_add(&hen_here_builds[0], 1ull, __ATOMIC_RELAXED);
        __atomic_fetch_add(&hen_here_builds[1], here_ns, __ATOMIC_RELAXED);
        if (here_ns > hen_here_builds[2]) {
            hen_here_builds[2] = here_ns;
        }
        return true;
    }
#endif

    // Fallback to (a)synchronous compilation
#ifdef __SWITCH__
    worker->QueueWork([this] {
#if HEN_DEV
        hen_jit_profile_retarget();
#endif
        Build();
    });
#else
    worker->QueueWork([this] { Build(); });
#endif
    is_pending = true;
    return wait_built;
}

#ifdef __SWITCH__
extern "C" void hen_boost_note_work(unsigned long long ns); // hen/jit/hen_jit.c: the loading boost
extern "C" unsigned long long hen_jit_now_ns(void);
namespace {
struct HenBoostBuildNote {
    unsigned long long start = hen_jit_now_ns();
    ~HenBoostBuildNote() {
        const unsigned long long took = hen_jit_now_ns() - start;
        if (took > 3'000'000ull) {
            hen_boost_note_work(took); // (a pipeline that had to be compiled: more usually follow)
        }
    }
};
} // namespace
#endif

bool GraphicsPipeline::Build(bool fail_on_compile_required) {
    MICROPROFILE_SCOPE(Vulkan_Pipeline);
#ifdef __SWITCH__
    const HenBoostBuildNote hen_boost_note;
#endif

    const u32 stride_alignment = instance.GetMinVertexStrideAlignment();
    std::array<vk::VertexInputBindingDescription, MAX_VERTEX_BINDINGS> bindings;
    for (u32 i = 0; i < info.state.vertex_layout.binding_count; i++) {
        const auto& binding = info.state.vertex_layout.bindings[i];
        bindings[i] = vk::VertexInputBindingDescription{
            .binding = binding.binding,
            .stride = Common::AlignUp(binding.byte_count.Value(), stride_alignment),
            .inputRate = binding.fixed.Value() ? vk::VertexInputRate::eInstance
                                               : vk::VertexInputRate::eVertex,
        };
    }

    std::array<vk::VertexInputAttributeDescription, MAX_VERTEX_ATTRIBUTES> attributes;
    for (u32 i = 0; i < info.state.vertex_layout.attribute_count; i++) {
        const auto& attr = info.state.vertex_layout.attributes[i];
        const FormatTraits& traits = instance.GetTraits(attr.type, attr.size);
        attributes[i] = vk::VertexInputAttributeDescription{
            .location = attr.location,
            .binding = attr.binding,
            .format = traits.native,
            .offset = attr.offset,
        };

        // At the end there's always the fixed binding which takes up
        // at least 16 bytes so we should always be able to alias.
        if (traits.needs_emulation) {
            const FormatTraits& comp_four_traits = instance.GetTraits(attr.type, 4);
            attributes[i].format = comp_four_traits.native;
        }
    }

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .vertexBindingDescriptionCount = info.state.vertex_layout.binding_count,
        .pVertexBindingDescriptions = bindings.data(),
        .vertexAttributeDescriptionCount = info.state.vertex_layout.attribute_count,
        .pVertexAttributeDescriptions = attributes.data(),
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = PicaToVK::PrimitiveTopology(info.state.rasterization.topology),
        .primitiveRestartEnable = false,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state = {
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .cullMode = PicaToVK::CullMode(info.state.rasterization.cull_mode,
                                       info.state.rasterization.flip_viewport),
        .frontFace = PicaToVK::FrontFace(info.state.rasterization.cull_mode),
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = false,
    };

    const vk::PipelineColorBlendAttachmentState colorblend_attachment = {
        .blendEnable = info.state.blending.blend_enable,
        .srcColorBlendFactor = PicaToVK::BlendFunc(info.state.blending.src_color_blend_factor),
        .dstColorBlendFactor = PicaToVK::BlendFunc(info.state.blending.dst_color_blend_factor),
        .colorBlendOp = PicaToVK::BlendEquation(info.state.blending.color_blend_eq),
        .srcAlphaBlendFactor = PicaToVK::BlendFunc(info.state.blending.src_alpha_blend_factor),
        .dstAlphaBlendFactor = PicaToVK::BlendFunc(info.state.blending.dst_alpha_blend_factor),
        .alphaBlendOp = PicaToVK::BlendEquation(info.state.blending.alpha_blend_eq),
        .colorWriteMask =
            static_cast<vk::ColorComponentFlags>(info.GetFinalColorWriteMask(instance)),
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = !info.state.blending.blend_enable && !instance.NeedsLogicOpEmulation(),
        .logicOp = PicaToVK::LogicOp(info.state.blending.logic_op),
        .attachmentCount = 1,
        .pAttachments = &colorblend_attachment,
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    const vk::Viewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {1, 1},
    };

    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    boost::container::static_vector<vk::DynamicState, 15> dynamic_states = {
        vk::DynamicState::eViewport,           vk::DynamicState::eScissor,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilReference,   vk::DynamicState::eBlendConstants,
    };

    if (instance.IsExtendedDynamicStateSupported()) {
        constexpr std::array extended = {
            vk::DynamicState::eCullModeEXT,        vk::DynamicState::eDepthCompareOpEXT,
            vk::DynamicState::eDepthTestEnableEXT, vk::DynamicState::eDepthWriteEnableEXT,
            vk::DynamicState::eFrontFaceEXT,       vk::DynamicState::ePrimitiveTopologyEXT,
            vk::DynamicState::eStencilOpEXT,       vk::DynamicState::eStencilTestEnableEXT,
        };
        dynamic_states.insert(dynamic_states.end(), extended.begin(), extended.end());
    }
#ifdef __SWITCH__
    // (the fragment size: 1x1 for every draw but the generic fragment shader's, PipelineCache::BindPipeline)
    if (instance.HenCoarseShading() != 0) {
        dynamic_states.push_back(vk::DynamicState::eFragmentShadingRateKHR);
    }
#endif

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::StencilOpState stencil_op_state = {
        .failOp = PicaToVK::StencilOp(info.state.depth_stencil.stencil_fail_op),
        .passOp = PicaToVK::StencilOp(info.state.depth_stencil.stencil_pass_op),
        .depthFailOp = PicaToVK::StencilOp(info.state.depth_stencil.stencil_depth_fail_op),
        .compareOp = PicaToVK::CompareFunc(info.state.depth_stencil.stencil_compare_op),
    };

    const vk::PipelineDepthStencilStateCreateInfo depth_info = {
        .depthTestEnable = static_cast<u32>(info.state.depth_stencil.depth_test_enable.Value()),
        .depthWriteEnable = static_cast<u32>(info.state.depth_stencil.depth_write_enable.Value()),
        .depthCompareOp = PicaToVK::CompareFunc(info.state.depth_stencil.depth_compare_op),
        .depthBoundsTestEnable = false,
        .stencilTestEnable = static_cast<u32>(info.state.depth_stencil.stencil_test_enable.Value()),
        .front = stencil_op_state,
        .back = stencil_op_state,
    };

    u32 shader_count = 0;
    std::array<vk::PipelineShaderStageCreateInfo, MAX_SHADER_STAGES> shader_stages;
    for (std::size_t i = 0; i < stages.size(); i++) {
        Shader* shader = stages[i];
        if (!shader) {
            continue;
        }

        shader->WaitDone();
        shader_stages[shader_count++] = vk::PipelineShaderStageCreateInfo{
            .stage = MakeShaderStage(i),
            .module = shader->Handle(),
            .pName = "main",
        };
    }

    vk::GraphicsPipelineCreateInfo pipeline_info = {
        .stageCount = shader_count,
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_info,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = pipeline_layout,
        .renderPass = renderpass_cache.GetRenderpass(info.state.attachments.color,
                                                     info.state.attachments.depth, false),
    };

    if (fail_on_compile_required) {
        pipeline_info.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequiredEXT;
    }

    const auto hen_start = std::chrono::steady_clock::now();
#ifdef __SWITCH__
    // (Azahar's Switch 2 port) The pipeline from four separately compiled parts
    // (VK_EXT_graphics_pipeline_library), linked without cross-stage optimisation. Measured on
    // the Switch 2's driver over 44 pipelines of a first launch: the whole pipeline at once
    // took 208 ms on average; its vertex shader part 22 ms, its fragment shader part 8 ms,
    // the two interface parts and the link under 0.1 ms. The driver optimises a whole
    // pipeline's stages against each other, so it compiled the vertex shader again for every
    // new fragment shader or state; the parts are compiled alone and cached alone.
    if (hen_pipeline_libraries && instance.IsGraphicsPipelineLibrarySupported()) {
        const vk::Device device = instance.GetDevice();
        const vk::PipelineCreateFlags fail_flag =
            fail_on_compile_required ? vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequiredEXT
                                     : vk::PipelineCreateFlags{};
        std::array<vk::PipelineShaderStageCreateInfo, MAX_SHADER_STAGES> pre{}, frag{};
        u32 pre_count = 0, frag_count = 0;
        for (u32 i = 0; i < shader_count; i++) {
            if (shader_stages[i].stage == vk::ShaderStageFlagBits::eFragment) {
                frag[frag_count++] = shader_stages[i];
            } else {
                pre[pre_count++] = shader_stages[i];
            }
        }
        std::array<vk::UniquePipeline, 4> owned;
        std::array<vk::Pipeline, 4> borrowed{}; // (from the cache of parts: not ours to destroy)
        bool compile_required = false, failed = false;
        const auto make = [&](std::size_t slot, vk::GraphicsPipelineLibraryFlagBitsEXT which,
                              vk::GraphicsPipelineCreateInfo create) {
            if (compile_required || failed) {
                return;
            }
            u64 cache_key = 0;
            // (the generic fragment shader: it is the one part worth keeping, tens of
            // milliseconds to compile and wanted again with every blend and depth state)
            if (slot == 2 && HenIsGenericFs(info.state.shader_ids[1])) {
                cache_key = 0xcbf29ce484222325ull;
                cache_key = HenHashBytes(cache_key, &info.state.shader_ids[1],
                                         sizeof(info.state.shader_ids[1])); // (ProgramType::FS)
                cache_key = HenHashBytes(cache_key, &pipeline_info.layout, sizeof(pipeline_info.layout));
                cache_key = HenHashBytes(cache_key, &pipeline_info.renderPass,
                                         sizeof(pipeline_info.renderPass));
                cache_key = HenHashBytes(cache_key, &multisampling.rasterizationSamples,
                                         sizeof(multisampling.rasterizationSamples));
                if (!instance.IsExtendedDynamicStateSupported()) {
                    cache_key = HenHashBytes(cache_key, &info.state.depth_stencil,
                                             sizeof(info.state.depth_stencil));
                }
                cache_key |= 1;
                std::scoped_lock lock{hen_part_cache_mutex};
                if (const auto found = hen_part_cache_map.find(cache_key);
                    found != hen_part_cache_map.end()) {
                    borrowed[slot] = vk::Pipeline{found->second};
                    HEN_STAT(hen_part_cache_hits++;)
                    return;
                }
            }
            const vk::GraphicsPipelineLibraryCreateInfoEXT part{.flags = which};
            create.pNext = &part;
            create.flags = vk::PipelineCreateFlagBits::eLibraryKHR | fail_flag;
            HEN_STAT(const auto part_start = std::chrono::steady_clock::now();)
            create.layout = pipeline_info.layout;
            create.renderPass = pipeline_info.renderPass;
            create.pDynamicState = &dynamic_info;
            // (the development builds' figures: repeats of a part, and the time in each part)
            HEN_STAT(bool hen_repeat = false;
            if (slot == 1 || slot == 2) {
                u64 key = 0xcbf29ce484222325ull;
                for (u32 i = 0; i < create.stageCount; i++) {
                    key = HenHashBytes(key, &create.pStages[i].module, sizeof(create.pStages[i].module));
                    key = HenHashBytes(key, &create.pStages[i].stage, sizeof(create.pStages[i].stage));
                }
                key = HenHashBytes(key, &create.layout, sizeof(create.layout));
                key = HenHashBytes(key, &create.renderPass, sizeof(create.renderPass));
                if (slot == 1) {
                    key = HenHashBytes(key, &raster_state, sizeof(raster_state));
                } else {
                    key = HenHashBytes(key, &depth_info, sizeof(depth_info));
                    key = HenHashBytes(key, &multisampling.rasterizationSamples,
                                       sizeof(multisampling.rasterizationSamples));
                }
                std::scoped_lock lock{hen_repeat_mutex};
                hen_repeat = !hen_part_seen[slot - 1].insert(key).second;
                hen_part_requests[slot - 1]++;
            })
            auto made = device.createGraphicsPipelineUnique(pipeline_cache, create);
            HEN_STAT(if (hen_standin_build && !fail_on_compile_required) {
                __atomic_fetch_add(&hen_standin_part_ns[slot],
                                   static_cast<unsigned long long>(
                                       std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - part_start)
                                           .count()),
                                   __ATOMIC_RELAXED);
                if ((slot == 1 || slot == 2) && !hen_repeat) {
                    __atomic_fetch_add(&hen_standin_part_ns[5], 1ull, __ATOMIC_RELAXED);
                }
            }
            if (hen_repeat) {
                hen_part_repeats[slot - 1]++;
                hen_part_repeat_ns[slot - 1] +=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - part_start)
                        .count();
            }
            if (!fail_on_compile_required) {
                hen_part_ns[slot] += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - part_start)
                                         .count();
                hen_part_count[slot]++;
            })
            if (made.result == vk::Result::eSuccess) {
                owned[slot] = std::move(made.value);
                if (cache_key != 0) {
                    // (kept: the cache owns it from here; if another thread made the same part
                    // meanwhile, this one stays ours and goes when the link is done)
                    std::scoped_lock lock{hen_part_cache_mutex};
                    if (hen_part_cache_map.try_emplace(cache_key, static_cast<VkPipeline>(*owned[slot]))
                            .second) {
                        borrowed[slot] = owned[slot].release();
                    }
                }
            } else if (made.result == vk::Result::eErrorPipelineCompileRequiredEXT) {
                compile_required = true;
            } else {
                failed = true;
            }
        };
        make(0, vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface,
             vk::GraphicsPipelineCreateInfo{
                 .pVertexInputState = &vertex_input_info,
                 .pInputAssemblyState = &input_assembly,
             });
        make(1, vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders,
             vk::GraphicsPipelineCreateInfo{
                 .stageCount = pre_count,
                 .pStages = pre.data(),
                 .pViewportState = &viewport_info,
                 .pRasterizationState = &raster_state,
             });
        make(2, vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader,
             vk::GraphicsPipelineCreateInfo{
                 .stageCount = frag_count,
                 .pStages = frag.data(),
                 .pMultisampleState = &multisampling,
                 .pDepthStencilState = &depth_info,
             });
        make(3, vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface,
             vk::GraphicsPipelineCreateInfo{
                 .pMultisampleState = &multisampling,
                 .pColorBlendState = &color_blending,
             });
        if (compile_required) {
            return false;
        }
        if (!failed) {
            const std::array<vk::Pipeline, 4> parts{
                *owned[0], *owned[1], borrowed[2] ? borrowed[2] : *owned[2], *owned[3]};
            const vk::PipelineLibraryCreateInfoKHR libraries{
                .libraryCount = static_cast<u32>(parts.size()),
                .pLibraries = parts.data(),
            };
            const vk::GraphicsPipelineCreateInfo link{
                .pNext = &libraries,
                .flags = fail_flag,
                .layout = pipeline_info.layout,
            };
            HEN_STAT(const auto link_start = std::chrono::steady_clock::now();)
            auto linked = device.createGraphicsPipelineUnique(pipeline_cache, link);
            HEN_STAT(if (hen_standin_build && !fail_on_compile_required) {
                __atomic_fetch_add(&hen_standin_part_ns[4],
                                   static_cast<unsigned long long>(
                                       std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - link_start)
                                           .count()),
                                   __ATOMIC_RELAXED);
            })
            if (linked.result == vk::Result::eErrorPipelineCompileRequiredEXT) {
                return false;
            }
            if (linked.result == vk::Result::eSuccess) {
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - hen_start)
                                    .count();
                hen_gfx_note(fail_on_compile_required ? HEN_GFX_PIPELINE_CACHED
                                                      : HEN_GFX_PIPELINE_COMPILE,
                             ns);
                if (hen_pipe_log && !fail_on_compile_required) {
                    std::printf("hen: pipe %.1f ms (from parts)\n", ns / 1e6);
                }
                // (the parts are no longer needed once linked: the pipeline keeps what it uses)
                pipeline = std::move(linked.value);
                MarkDone();
                return true;
            }
        }
        // (a part or the link was refused: build it the usual way)
        static bool told = false;
        if (!told) {
            told = true;
            std::printf("hen: a pipeline couldn't be made from parts; using whole pipelines for it\n");
        }
    }
#endif
    auto result = instance.GetDevice().createGraphicsPipelineUnique(pipeline_cache, pipeline_info);
    if (result.result == vk::Result::eSuccess) {
        hen_gfx_note(fail_on_compile_required ? HEN_GFX_PIPELINE_CACHED : HEN_GFX_PIPELINE_COMPILE,
                     std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - hen_start)
                         .count());
#ifdef __SWITCH__
        // --pipe-log: for each pipeline the driver had to compile, how long it took and which
        // of its shaders no earlier pipeline of this run had used (to tell what the time is for).
        if (hen_pipe_log && !fail_on_compile_required) {
            static std::mutex seen_mutex;
            static std::set<VkShaderModule> seen;
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - hen_start)
                                  .count();
            char text[4] = {'-', '-', '-', 0};
            {
                std::scoped_lock lock{seen_mutex};
                for (std::size_t i = 0; i < stages.size() && i < 3; i++) {
                    if (stages[i]) {
                        text[i] = seen.insert(static_cast<VkShaderModule>(stages[i]->Handle())).second
                                      ? 'N'
                                      : 's';
                    }
                }
            }
            std::printf("hen: pipe %.1f ms, shaders (vertex fragment geometry; N new, s seen) %s\n",
                        ms, text);
        }
#endif
        pipeline = std::move(result.value);
    } else if (result.result == vk::Result::eErrorPipelineCompileRequiredEXT) {
        return false;
    } else {
        UNREACHABLE_MSG("Graphics pipeline creation failed!");
    }

    MarkDone();
    return true;
}

} // namespace Vulkan
