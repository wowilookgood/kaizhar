// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <memory>
#include <SPIRV/GlslangToSpv.h>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <glslang/Include/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include "common/assert.h"
#include "common/literals.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#if defined(__SWITCH__) && HEN_DEV
extern "C" int hen_pipe_log; // --pipe-log
extern "C" int hen_dump_spirv; // --dump-spirv
#endif

namespace Vulkan {

using namespace Common::Literals;

namespace {
constexpr TBuiltInResource DefaultTBuiltInResource = {
    .maxLights = 32,
    .maxClipPlanes = 6,
    .maxTextureUnits = 32,
    .maxTextureCoords = 32,
    .maxVertexAttribs = 64,
    .maxVertexUniformComponents = 4096,
    .maxVaryingFloats = 64,
    .maxVertexTextureImageUnits = 32,
    .maxCombinedTextureImageUnits = 80,
    .maxTextureImageUnits = 32,
    .maxFragmentUniformComponents = 4096,
    .maxDrawBuffers = 32,
    .maxVertexUniformVectors = 128,
    .maxVaryingVectors = 8,
    .maxFragmentUniformVectors = 16,
    .maxVertexOutputVectors = 16,
    .maxFragmentInputVectors = 15,
    .minProgramTexelOffset = -8,
    .maxProgramTexelOffset = 7,
    .maxClipDistances = 8,
    .maxComputeWorkGroupCountX = 65535,
    .maxComputeWorkGroupCountY = 65535,
    .maxComputeWorkGroupCountZ = 65535,
    .maxComputeWorkGroupSizeX = 1024,
    .maxComputeWorkGroupSizeY = 1024,
    .maxComputeWorkGroupSizeZ = 64,
    .maxComputeUniformComponents = 1024,
    .maxComputeTextureImageUnits = 16,
    .maxComputeImageUniforms = 8,
    .maxComputeAtomicCounters = 8,
    .maxComputeAtomicCounterBuffers = 1,
    .maxVaryingComponents = 60,
    .maxVertexOutputComponents = 64,
    .maxGeometryInputComponents = 64,
    .maxGeometryOutputComponents = 128,
    .maxFragmentInputComponents = 128,
    .maxImageUnits = 8,
    .maxCombinedImageUnitsAndFragmentOutputs = 8,
    .maxCombinedShaderOutputResources = 8,
    .maxImageSamples = 0,
    .maxVertexImageUniforms = 0,
    .maxTessControlImageUniforms = 0,
    .maxTessEvaluationImageUniforms = 0,
    .maxGeometryImageUniforms = 0,
    .maxFragmentImageUniforms = 8,
    .maxCombinedImageUniforms = 8,
    .maxGeometryTextureImageUnits = 16,
    .maxGeometryOutputVertices = 256,
    .maxGeometryTotalOutputComponents = 1024,
    .maxGeometryUniformComponents = 1024,
    .maxGeometryVaryingComponents = 64,
    .maxTessControlInputComponents = 128,
    .maxTessControlOutputComponents = 128,
    .maxTessControlTextureImageUnits = 16,
    .maxTessControlUniformComponents = 1024,
    .maxTessControlTotalOutputComponents = 4096,
    .maxTessEvaluationInputComponents = 128,
    .maxTessEvaluationOutputComponents = 128,
    .maxTessEvaluationTextureImageUnits = 16,
    .maxTessEvaluationUniformComponents = 1024,
    .maxTessPatchComponents = 120,
    .maxPatchVertices = 32,
    .maxTessGenLevel = 64,
    .maxViewports = 16,
    .maxVertexAtomicCounters = 0,
    .maxTessControlAtomicCounters = 0,
    .maxTessEvaluationAtomicCounters = 0,
    .maxGeometryAtomicCounters = 0,
    .maxFragmentAtomicCounters = 8,
    .maxCombinedAtomicCounters = 8,
    .maxAtomicCounterBindings = 1,
    .maxVertexAtomicCounterBuffers = 0,
    .maxTessControlAtomicCounterBuffers = 0,
    .maxTessEvaluationAtomicCounterBuffers = 0,
    .maxGeometryAtomicCounterBuffers = 0,
    .maxFragmentAtomicCounterBuffers = 1,
    .maxCombinedAtomicCounterBuffers = 1,
    .maxAtomicCounterBufferSize = 16384,
    .maxTransformFeedbackBuffers = 4,
    .maxTransformFeedbackInterleavedComponents = 64,
    .maxCullDistances = 8,
    .maxCombinedClipAndCullDistances = 8,
    .maxSamples = 4,
    .maxMeshOutputVerticesNV = 256,
    .maxMeshOutputPrimitivesNV = 512,
    .maxMeshWorkGroupSizeX_NV = 32,
    .maxMeshWorkGroupSizeY_NV = 1,
    .maxMeshWorkGroupSizeZ_NV = 1,
    .maxTaskWorkGroupSizeX_NV = 32,
    .maxTaskWorkGroupSizeY_NV = 1,
    .maxTaskWorkGroupSizeZ_NV = 1,
    .maxMeshViewCountNV = 4,
    .maxDualSourceDrawBuffersEXT = 1,
    .limits =
        TLimits{
            .nonInductiveForLoops = 1,
            .whileLoops = 1,
            .doWhileLoops = 1,
            .generalUniformIndexing = 1,
            .generalAttributeMatrixVectorIndexing = 1,
            .generalVaryingIndexing = 1,
            .generalSamplerIndexing = 1,
            .generalVariableIndexing = 1,
            .generalConstantMatrixVectorIndexing = 1,
        },
};

EShLanguage ToEshShaderStage(vk::ShaderStageFlagBits stage) {
    switch (stage) {
    case vk::ShaderStageFlagBits::eVertex:
        return EShLanguage::EShLangVertex;
    case vk::ShaderStageFlagBits::eGeometry:
        return EShLanguage::EShLangGeometry;
    case vk::ShaderStageFlagBits::eFragment:
        return EShLanguage::EShLangFragment;
    case vk::ShaderStageFlagBits::eCompute:
        return EShLanguage::EShLangCompute;
    default:
        UNREACHABLE_MSG("Unkown shader stage {}", stage);
    }
    return EShLanguage::EShLangVertex;
}

bool InitializeCompiler() {
    static bool glslang_initialized = false;

    if (glslang_initialized) {
        return true;
    }

    if (!glslang::InitializeProcess()) {
        LOG_CRITICAL(Render_Vulkan, "Failed to initialize glslang shader compiler");
        return false;
    }

    std::atexit([]() { glslang::FinalizeProcess(); });

    glslang_initialized = true;
    return true;
}
} // Anonymous namespace

#if defined(__SWITCH__) && HEN_DEV
namespace {
/// A SPIR-V module as base64 over the log (tools: spvdump.py, spvcorpus.py).
void HenDumpSpirv(std::span<const u32> out_code, std::size_t glsl_bytes, const char* stage) {
    static std::mutex dump_mutex;
    static unsigned next_id = 0;
    std::scoped_lock lock{dump_mutex};
    const unsigned id = next_id++;
    static const char digits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const u8* const bytes = reinterpret_cast<const u8*>(out_code.data());
    const std::size_t size = out_code.size() * 4;
    std::printf("hen: spvdump %u begin %zu words, %zu bytes of GLSL, %s\n", id, out_code.size(),
                glsl_bytes, stage);
    char line[100];
    for (std::size_t at = 0; at < size;) {
        std::size_t n = 0;
        for (; n < 76 && at < size; at += 3) {
            const u32 v = (bytes[at] << 16) | (at + 1 < size ? bytes[at + 1] << 8 : 0) |
                          (at + 2 < size ? bytes[at + 2] : 0);
            line[n++] = digits[v >> 18];
            line[n++] = digits[(v >> 12) & 63];
            line[n++] = at + 1 < size ? digits[(v >> 6) & 63] : '=';
            line[n++] = at + 2 < size ? digits[v & 63] : '=';
        }
        line[n] = 0;
        std::printf("hen: spvdump %u %s\n", id, line);
    }
    std::printf("hen: spvdump %u end\n", id);
}
} // namespace
#endif

std::vector<u32> CompileGLSL(std::string_view code, vk::ShaderStageFlagBits stage,
                             std::string_view premable) {
    if (!InitializeCompiler()) {
        return {};
    }
#if defined(__SWITCH__) && HEN_DEV
    const auto hen_start = std::chrono::steady_clock::now();
#endif

    EProfile profile = ECoreProfile;
    EShMessages messages =
        static_cast<EShMessages>(EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);
    EShLanguage lang = ToEshShaderStage(stage);

    const int default_version = 450;
    const char* pass_source_code = code.data();
    int pass_source_code_length = static_cast<int>(code.size());

    auto shader = std::make_unique<glslang::TShader>(lang);
    shader->setEnvTarget(glslang::EShTargetSpv,
                         glslang::EShTargetLanguageVersion::EShTargetSpv_1_3);
    shader->setStringsWithLengths(&pass_source_code, &pass_source_code_length, 1);
    shader->setPreamble(premable.data());

    glslang::TShader::ForbidIncluder includer;
    if (!shader->parse(&DefaultTBuiltInResource, default_version, profile, false, true, messages,
                       includer)) [[unlikely]] {
        LOG_INFO(Render_Vulkan, "Shader Info Log:\n{}\n{}", shader->getInfoLog(),
                 shader->getInfoDebugLog());
        LOG_INFO(Render_Vulkan, "Shader Source:\n{}", code);
        return {};
    }

    // Even though there's only a single shader, we still need to link it to generate SPV
    auto program = std::make_unique<glslang::TProgram>();
    program->addShader(shader.get());
    if (!program->link(messages)) {
        LOG_INFO(Render_Vulkan, "Program Info Log:\n{}\n{}", program->getInfoLog(),
                 program->getInfoDebugLog());
        return {};
    }

    glslang::TIntermediate* intermediate = program->getIntermediate(lang);
    std::vector<u32> out_code;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions options;

    // Controls optimizations on the generated SPIR-V code.
    options.disableOptimizer = Settings::values.disable_spirv_optimizer.GetValue();
    options.validate = false;
    options.optimizeSize = true;

    out_code.reserve(8_KiB);
    glslang::GlslangToSpv(*intermediate, out_code, &logger, &options);

    const std::string spv_messages = logger.getAllMessages();
    if (!spv_messages.empty()) {
        LOG_INFO(Render_Vulkan, "SPIR-V conversion messages: {}", spv_messages);
    }
#if defined(__SWITCH__) && HEN_DEV
    // --dump-spirv: every vertex shader of 2000 words or more, as base64 over the log (the
    // ones the driver's compiler takes 150 to 700 ms over).
    if (hen_dump_spirv == 1 && stage == vk::ShaderStageFlagBits::eVertex && out_code.size() >= 2000) {
        HenDumpSpirv(out_code, code.size(), "vertex");
    }
    if (hen_pipe_log) {
        std::printf("hen: spv: %s, %zu bytes of GLSL, %zu words of SPIR-V, %.1f ms\n",
                    stage == vk::ShaderStageFlagBits::eVertex ? "vertex"
                    : stage == vk::ShaderStageFlagBits::eFragment ? "fragment" : "geometry",
                    code.size(), out_code.size(),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              hen_start)
                        .count());
    }
#endif

    return out_code;
}

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
#if defined(__SWITCH__) && HEN_DEV
    // --dump-spirv-all: every module handed to the driver, whichever generator made it (the
    // fragment shaders mostly come from the SPIR-V generator, not through glslang above).
    // The collector reads the stage from the module itself.
    if (hen_dump_spirv == 2) {
        HenDumpSpirv(code, 0, "module");
    }
#endif
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    try {
        return device.createShaderModule(shader_info);
    } catch (vk::SystemError& err) {
        LOG_ERROR(Render_Vulkan, "{}", err.what());
    }

    return {};
}

vk::ShaderModule Compile(std::string_view code, vk::ShaderStageFlagBits stage, vk::Device device,
                         std::string_view premable) {
    return CompileSPV(CompileGLSL(code, stage, premable), device);
}

} // namespace Vulkan
