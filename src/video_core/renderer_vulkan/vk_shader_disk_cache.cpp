// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_set>
#include "common/hen_stats.h"
#include "common/hen_crumb.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/static_lru_cache.h"
#include "common/zstd_compression.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_disk_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/shader/generator/glsl_fs_shader_gen.h"
#include "video_core/shader/generator/glsl_shader_gen.h"
#include "video_core/shader/generator/shader_gen.h"
#include "video_core/shader/generator/spv_fs_shader_gen.h"

#define MALFORMED_DISK_CACHE                                                                       \
    do {                                                                                           \
        LOG_ERROR(Render_Vulkan, "Malformed disk shader cache");                                   \
        cleanup_on_error();                                                                        \
        return false;                                                                              \
    } while (0)

// Enable to debug when new cache objects are created.
// #define ENABLE_LOG_NEW_OBJECT

#ifdef ENABLE_LOG_NEW_OBJECT
#define LOG_NEW_OBJECT LOG_DEBUG
#else
#define LOG_NEW_OBJECT(...) ((void)0)
#endif

namespace Vulkan {

using namespace Pica::Shader::Generator;
using namespace Pica::Shader;

static VideoCore::DiskResourceLoadCallback MakeThrottledCallback(
    VideoCore::DiskResourceLoadCallback original) {

    if (!original)
        return nullptr;

    auto last_call = std::chrono::steady_clock::now() - std::chrono::milliseconds(10);

    return [original, last_call](VideoCore::LoadCallbackStage stage, std::size_t current,
                                 std::size_t total, const std::string& name) mutable {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_call >= std::chrono::milliseconds(10)) {
            last_call = now;
            original(stage, current, total, name);
        }
    };
}

void ShaderDiskCache::Init(const std::atomic_bool& stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback) {

    if (!Settings::values.use_disk_shader_cache)
        return;

    auto new_callback = MakeThrottledCallback(callback);

#ifdef __SWITCH__
    if (HenRegenWanted()) {
        // A deferred regeneration ends by deleting the old file and renaming the "_temp" one
        // to its name. If the first was done and the second was not (power lost in between),
        // the complete file is the "_temp" one: it gets its name now.
        const auto recover = [](const std::string& final_path, const std::string& temp_path) {
            if (!FileUtil::Exists(final_path) && FileUtil::Exists(temp_path) &&
                FileUtil::Rename(temp_path, final_path)) {
                std::printf("hen: gfx: shader cache: %s takes its name (an interrupted swap)\n",
                            temp_path.c_str());
            }
        };
        recover(GetVSFile(title_id, false), GetVSFile(title_id, true));
        recover(GetFSFile(title_id, false), GetFSFile(title_id, true));
    }
#endif

    if (!stop_loading && !InitVSCache(stop_loading, new_callback)) {
        RecreateCache(vs_cache, CacheFileType::VS_CACHE);
    }
    if (!stop_loading && !InitFSCache(stop_loading, new_callback)) {
        RecreateCache(fs_cache, CacheFileType::FS_CACHE);
    }
    if (!stop_loading && !InitGSCache(stop_loading, new_callback)) {
        RecreateCache(gs_cache, CacheFileType::GS_CACHE);
    }
    if (!stop_loading && !InitPLCache(stop_loading, new_callback)) {
        RecreateCache(pl_cache, CacheFileType::PL_CACHE);
    }
#ifdef __SWITCH__
    // (after the pipeline cache is read: nothing else is left to do on this thread)
    HenRegenStart(stop_loading);
#endif
}

std::optional<std::pair<u64, Shader* const>> ShaderDiskCache::UseProgrammableVertexShader(
    const Pica::RegsInternal& regs, Pica::ShaderSetup& setup, const VertexLayout& layout) {

    HEN_CRUMB("VS: config");
    PicaVSConfig config{regs, setup};

    // Transfer vertex attributes to the VS config
    config.state.used_input_vertex_attributes = layout.attribute_count;
    for (u32 i = 0; i < layout.attribute_count; i++) {
        auto& dst = config.state.input_vertex_attributes[i];
        const auto& src = layout.attributes[i];

        dst.location = src.location;
        dst.type = static_cast<u8>(src.type.Value());
        dst.size = src.size;
    }

    HEN_CRUMB("VS: hash");
    const auto config_hash = config.Hash();

    const auto [iter_config, new_config] = programmable_vertex_map.try_emplace(config_hash);
    if (new_config) {

        LOG_NEW_OBJECT(Render_Vulkan, "New VS config {:016X}", config_hash);

        HEN_CRUMB("VS: CalcExtraConfig");
        ExtraVSConfig extra_config = parent.CalcExtraConfig(config);
        HEN_CRUMB("VS: GenerateVertexShader");

        auto program =
            Common::HashableString(GLSL::GenerateVertexShader(setup, config, extra_config));

        if (program.empty()) {
            LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
            programmable_vertex_map.erase(config_hash);
            return {};
        }

        HEN_CRUMB("VS: program hash");
        const u64 spirv_id = program.Hash();

        auto [iter_prog, new_program] =
            programmable_vertex_cache.try_emplace(spirv_id, parent.instance);
        auto& shader = iter_prog->second;

        if (new_program) {
            LOG_NEW_OBJECT(Render_Vulkan, "New VS SPIRV {:016X}", spirv_id);

            shader.program = std::move(program);
            const vk::Device device = parent.instance.GetDevice();
            HEN_CRUMB("VS: queue compile");
            parent.shader_workers.QueueWork([device, &shader, this, spirv_id] {
                HenGfxTimer hen_timer{HEN_GFX_SHADER_COMPILE};
                HEN_CRUMB("VS worker: CompileGLSL");
                auto spirv = CompileGLSL(shader.program, vk::ShaderStageFlagBits::eVertex);
                HEN_CRUMB("VS worker: AppendVSSPIRV");
                AppendVSSPIRV(vs_cache, spirv, spirv_id);
                shader.program.clear();
                HEN_CRUMB("VS worker: CompileSPV");
                shader.module = CompileSPV(spirv, device);
                HEN_CRUMB("VS worker: done");
                shader.MarkDone();
            });
        }

        HEN_CRUMB("VS: AppendVSConfigProgram");
        AppendVSConfigProgram(vs_cache, config, setup, config_hash, spirv_id);
        HEN_CRUMB("VS: appended");

        iter_config->second = &shader;
    }

    Shader* const shader{iter_config->second};
    if (!shader) {
        LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
        return {};
    }
#ifdef __SWITCH__
    if (hen_regen && !shader->IsDone()) {
        HenRegenBump(shader); // (a draw needs one that is waiting to be regenerated: it goes first)
    }
#endif

    return std::make_pair(config_hash, shader);
}

extern "C" int hen_fs_canon;
extern "C" unsigned long long hen_fs_configs_raw, hen_fs_configs_built;
namespace {
std::unordered_set<u64> hen_fs_seen; // (raw hashes, for the count only; one renderer thread)
}

std::optional<std::pair<u64, Shader* const>> ShaderDiskCache::UseFragmentShader(
    const Pica::RegsInternal& regs, const Pica::Shader::UserConfig& user) {

    FSConfig fs_config{regs};
    // (hen) --no-fs-canon: the configuration as the registers give it, as upstream.
    if (hen_fs_canon) {
        if (hen_fs_seen.insert(fs_config.Hash()).second) {
            hen_fs_configs_raw++;
        }
        fs_config.Canonicalize(parent.profile);
    }
    const auto fs_config_hash = fs_config.Hash();
    const auto [it, new_shader] = fragment_shaders.try_emplace(fs_config_hash, parent.instance);
    if (new_shader) {
        hen_fs_configs_built++;
    }
    auto& shader = it->second;

    if (new_shader) {
        LOG_NEW_OBJECT(Render_Vulkan, "New FS config {:016X}", fs_config_hash);

        parent.shader_workers.QueueWork([fs_config, user, this, &shader, fs_config_hash]() {
                HenGfxTimer hen_timer{HEN_GFX_SHADER_COMPILE};
            std::vector<u32> spirv;
            const bool use_spirv = parent.profile.vk_use_spirv_generator;
            if (use_spirv && !fs_config.UsesSpirvIncompatibleConfig()) {
                spirv = SPIRV::GenerateFragmentShader(fs_config, parent.profile);
                shader.module = CompileSPV(spirv, parent.instance.GetDevice());
            } else {
                const std::string code =
                    GLSL::GenerateFragmentShader(fs_config, user, parent.profile);
                spirv = CompileGLSL(code, vk::ShaderStageFlagBits::eFragment);
                shader.module = CompileSPV(spirv, parent.instance.GetDevice());
            }
            shader.MarkDone();

            if (user.IsCacheable()) {
                // Only cache to disk if the user config is cacheable
                AppendFSSPIRV(fs_cache, spirv, fs_config_hash);
                FSConfigEntry entry{.version = FSConfigEntry::EXPECTED_VERSION,
                                    .fs_config = fs_config};
                AppendFSConfig(fs_cache, entry, fs_config_hash);
            }
        });
    }
#ifdef __SWITCH__
    else if (hen_regen && !shader.IsDone()) {
        HenRegenBump(&shader); // (as for the vertex shaders)
    }
#endif

    return std::make_pair(fs_config_hash, &shader);
}

std::optional<std::pair<u64, Shader* const>> ShaderDiskCache::UseFixedGeometryShader(
    const Pica::RegsInternal& regs) {

    const PicaFixedGSConfig gs_config{regs};
    const auto gs_config_hash = gs_config.Hash();

    if (!parent.instance.UseGeometryShaders() ||
        parent.instance.IsFragmentShaderBarycentricSupported()) {
        // Even if we don't support geometry shaders, we still need to cache them
        // so that the shader cache is transferable. There is no need to cache
        // or build the SPIRV.

        if (known_geometry_shaders.emplace(gs_config_hash).second) {
            GSConfigEntry entry{
                .version = GSConfigEntry::EXPECTED_VERSION,
                .gs_config = gs_config,
            };
            AppendGSConfig(gs_cache, entry, gs_config_hash);
        }

        return std::make_pair(gs_config_hash, nullptr);
    } else {
        auto [it, new_shader] = fixed_geometry_shaders.try_emplace(gs_config_hash, parent.instance);
        auto& shader = it->second;

        if (new_shader) {
            LOG_NEW_OBJECT(Render_Vulkan, "New GS config {:016X}", gs_config_hash);

            parent.shader_workers.QueueWork([gs_config, this, &shader, gs_config_hash]() {
                HenGfxTimer hen_timer{HEN_GFX_SHADER_COMPILE};
                ExtraFixedGSConfig extra;
                extra.use_clip_planes = parent.profile.has_clip_planes;
                extra.separable_shader = true;

                const auto code = GLSL::GenerateFixedGeometryShader(gs_config, extra);
                const auto spirv = CompileGLSL(code, vk::ShaderStageFlagBits::eGeometry);
                shader.module = CompileSPV(spirv, parent.instance.GetDevice());
                shader.MarkDone();

                AppendGSSPIRV(gs_cache, spirv, gs_config_hash);
                GSConfigEntry entry{
                    .version = GSConfigEntry::EXPECTED_VERSION,
                    .gs_config = gs_config,
                };
                AppendGSConfig(gs_cache, entry, gs_config_hash);
            });
        }

        return std::make_pair(gs_config_hash, &shader);
    }
}

GraphicsPipeline* ShaderDiskCache::GetPipeline(const PipelineInfo& info) {

    u64 hash = info.Hash();
    u64 optimized_hash = info.state.OptimizedHash(parent.instance);

    auto [it, new_pipeline] = graphics_pipelines.try_emplace(optimized_hash);
    if (new_pipeline) {
        if (!parent.instance.UseGeometryShaders() ||
            parent.instance.IsFragmentShaderBarycentricSupported()) {
            // If we don't need geometry shaders disable
            // them before building the pipeline. It's done here
            // so that the shader ID could be hashed and saved with
            // the pipeline info so that it is transferable.
            parent.UseTrivialGeometryShader();
        }
        it.value() = std::make_unique<GraphicsPipeline>(
            parent.instance, parent.renderpass_cache, info, *parent.driver_pipeline_cache,
            *parent.pipeline_layout, parent.current_shaders, &parent.pipeline_workers);
    }

    if (known_graphic_pipelines.emplace(hash).second) {
        LOG_NEW_OBJECT(Render_Vulkan, "New Pipeline {:016X}", hash);

        PLConfigEntry entry{
            .version = PLConfigEntry::EXPECTED_VERSION,
            .pl_info = info.state,
        };

        AppendPLConfig(pl_cache, entry, hash);
    }

    return it.value().get();
}

ShaderDiskCache::SourceFileCacheVersionHash ShaderDiskCache::GetSourceFileCacheVersionHash() {
    SourceFileCacheVersionHash hash{};
    const std::size_t length = std::min(std::strlen(Common::g_shader_cache_version), hash.size());
    std::memcpy(hash.data(), Common::g_shader_cache_version, length);
    return hash;
}

ShaderDiskCache::CacheEntry ShaderDiskCache::CacheFile::ReadFirst() {
    return ReadAt(0);
}

ShaderDiskCache::CacheEntry ShaderDiskCache::CacheFile::ReadNext(const CacheEntry& previous) {
    if (!previous.valid)
        return CacheEntry();

    return ReadAt(previous.position + previous.header.entry_size);
}

std::pair<size_t, ShaderDiskCache::CacheEntry::CacheEntryHeader>
ShaderDiskCache::CacheFile::ReadNextHeader(
    const ShaderDiskCache::CacheEntry::CacheEntryHeader& previous, size_t previous_position) {

    size_t new_pos = previous_position + previous.entry_size;

    return {new_pos, ReadAtHeader(new_pos)};
}

ShaderDiskCache::CacheEntry::CacheEntryHeader ShaderDiskCache::CacheFile::ReadAtHeader(
    size_t position) {

    CacheEntry::CacheEntryHeader header;

    if (!ReadFromFileCached(&header, position, sizeof(header))) {
        return CacheEntry::CacheEntryHeader();
    }

    return header;
}

ShaderDiskCache::CacheEntry ShaderDiskCache::CacheFile::ReadAt(size_t position) {
    CacheEntry res{};
    res.position = position;

    constexpr u32 headers_size =
        sizeof(CacheEntry::CacheEntryHeader) + sizeof(CacheEntry::CacheEntryFooter);

    res.header = ReadAtHeader(position);

    if (res.header.Valid()) {

        // We have everything validated, read the data.
        u32 payload_size = res.header.entry_size - headers_size;
        std::vector<u8> payload(payload_size);

        if (ReadFromFileCached(payload.data(), (position + sizeof(CacheEntry::CacheEntryHeader)),
                               payload_size)) {
            // Decompress data if needed
            if (res.header.zstd_compressed) {
                if (Common::Compression::GetDecompressedSize(payload) <
                    CacheEntry::MAX_ENTRY_SIZE) {
                    res.data = Common::Compression::DecompressDataZSTD(payload);
                    res.valid = true;
                }
            } else {
                res.data = std::move(payload);
                res.valid = true;
            }
        }
    }
    return res;
}

size_t ShaderDiskCache::CacheFile::GetTotalEntries() {
    if (!file.IsGood()) {
        next_entry_id = SIZE_MAX;
        file_size = 0;
        return next_entry_id;
    }

    if (next_entry_id != SIZE_MAX) {
        return next_entry_id;
    }

    file_size = file.GetSize();
    if (file_size == 0) {
        next_entry_id = 0;
        return next_entry_id;
    }

    CacheEntry::CacheEntryFooter footer{};

    if (file.ReadAtArray(&footer, 1, file_size - sizeof(footer)) == sizeof(footer) &&
        footer.version == CacheEntry::CacheEntryFooter::ENTRY_VERSION) {
        next_entry_id = footer.entry_id + 1;
    } else {
        file_size = 0;
        return SIZE_MAX;
    }

    return next_entry_id;
}

void ShaderDiskCache::CacheFile::Append(CacheEntryType type, u64 id, std::span<const u8> data,
                                        bool compress) {
    if (curr_mode != CacheOpMode::APPEND) {
        return;
    }

    std::vector<u8> copy_data(data.begin(), data.end());

    append_worker.QueueWork([this, type, id, compress, data = std::move(copy_data)]() {
        if (next_entry_id == SIZE_MAX || !file.IsGood()) {
            return;
        }

        std::span<const u8> data_final;
        std::vector<u8> data_compress;

        CacheEntry::CacheEntryHeader header{};
        CacheEntry::CacheEntryFooter footer{};

        constexpr u32 headers_size =
            sizeof(CacheEntry::CacheEntryHeader) + sizeof(CacheEntry::CacheEntryFooter);

        if (compress) {
            data_compress = Common::Compression::CompressDataZSTDDefault(data);
            data_final = data_compress;
            header.zstd_compressed.Assign(true);
        } else {
            data_final = data;
        }
        header.entry_version = CacheEntry::CacheEntryHeader::ENTRY_VERSION;
        footer.version.Assign(CacheEntry::CacheEntryFooter::ENTRY_VERSION);
        header.entry_size = footer.entry_size = data_final.size() + headers_size;
        footer.entry_id.Assign(next_entry_id++);

        header.type = type;
        header.id = id;

        std::vector<u8> out_data(data_final.size() + headers_size);
        memcpy(out_data.data(), &header, sizeof(header));
        memcpy(out_data.data() + sizeof(header), data_final.data(), data_final.size());
        memcpy(out_data.data() + sizeof(header) + data_final.size(), &footer, sizeof(footer));

        file.WriteBytes(out_data.data(), out_data.size());
        if (file.IsGood()) {
            file.Flush();
        }
    });
}

bool ShaderDiskCache::CacheFile::SwitchMode(CacheOpMode mode) {
    if (curr_mode == mode) {
        return true;
    }
    if (curr_mode == CacheOpMode::APPEND) {
        append_worker.WaitForRequests();
    }

    switch (mode) {
    case CacheOpMode::READ: {
        next_entry_id = SIZE_MAX; // Force reading entries again
        cached_file_data_start = 0;
        cached_file_data.clear();
        file = FileUtil::IOFile(filepath, "rb");
        bool is_open = file.IsGood();
        if (is_open) {
            GetTotalEntries();
        }
        curr_mode = mode;
        return is_open;
    }
    case CacheOpMode::APPEND: {
        if (!SwitchMode(CacheOpMode::READ)) {
            curr_mode = mode;
            return false;
        }
        file.Close();
        cached_file_data_start = 0;
        cached_file_data.clear();
        curr_mode = mode;
        if (next_entry_id == SIZE_MAX) {
            // Cannot append if getting total items fails
            return false;
        }

        file = FileUtil::IOFile(filepath, "ab");
        return file.IsGood();
    }
    case CacheOpMode::DELETE: {
        next_entry_id = SIZE_MAX;
        cached_file_data_start = 0;
        cached_file_data.clear();
        file.Close();
        curr_mode = mode;
        return FileUtil::Delete(filepath);
    }
    case CacheOpMode::RECREATE: {
        if (!SwitchMode(CacheOpMode::DELETE)) {
            return false;
        }
        if (!FileUtil::CreateEmptyFile(filepath)) {
            return false;
        }
        return SwitchMode(CacheOpMode::APPEND);
    }
    default:
        UNREACHABLE();
    }
    return false;
}

bool ShaderDiskCache::CacheFile::ReadFromFileCached(void* dst, size_t position, size_t size) {
    if (!dst || position + size > file_size) {
        return false;
    }

    size_t offset = position - cached_file_data_start;
    if (position < cached_file_data_start || offset > cached_file_data.size() ||
        size > cached_file_data.size() - offset) {
        if (size > CacheEntry::MAX_ENTRY_SIZE) {
            return false;
        }

        size_t to_read = std::min<size_t>(CacheEntry::MAX_ENTRY_SIZE, file_size - position);

        cached_file_data_start = position;
        cached_file_data.resize(to_read);

        if (file.ReadAtBytes(cached_file_data.data(), to_read, position) != to_read) {
            return false;
        }

        offset = 0;
    }

    std::memcpy(dst, cached_file_data.data() + offset, size);
    return true;
}

std::string ShaderDiskCache::GetVSFile(u64 title_id, bool is_temp) const {
    return parent.GetTransferableDir() + DIR_SEP + fmt::format("{:016X}_vs", title_id) +
           (is_temp ? "_temp" : "") + ".vkch";
}

std::string ShaderDiskCache::GetFSFile(u64 title_id, bool is_temp) const {
    return parent.GetTransferableDir() + DIR_SEP + fmt::format("{:016X}_fs", title_id) +
           (is_temp ? "_temp" : "") + ".vkch";
}

std::string ShaderDiskCache::GetGSFile(u64 title_id, bool is_temp) const {
    return parent.GetTransferableDir() + DIR_SEP + fmt::format("{:016X}_gs", title_id) +
           (is_temp ? "_temp" : "") + ".vkch";
}

std::string ShaderDiskCache::GetPLFile(u64 title_id, bool is_temp) const {
    return parent.GetTransferableDir() + DIR_SEP + fmt::format("{:016X}_pl", title_id) +
           (is_temp ? "_temp" : "") + ".vkch";
}

bool ShaderDiskCache::RecreateCache(CacheFile& file, CacheFileType type) {
    file.SwitchMode(CacheFile::CacheOpMode::RECREATE);

    std::array<char, 0x20> build_name{};
    size_t name_len = std::strlen(Common::g_build_fullname);
    memcpy(build_name.data(), Common::g_build_fullname, std::min(name_len, build_name.size()));

    auto get_hash = [](CacheFileType type) {
        switch (type) {
        case CacheFileType::VS_CACHE:
            return PicaVSConfigState::StructHash();
        case CacheFileType::FS_CACHE:
            return FSConfig::StructHash();
        case CacheFileType::GS_CACHE:
            return PicaGSConfigState::StructHash();
        case CacheFileType::PL_CACHE:
            return StaticPipelineInfo::StructHash();
        default:
            UNREACHABLE();
            return u64{};
        };
    };

    FileInfoEntry entry{
        .cache_magic = FileInfoEntry::CACHE_FILE_MAGIC,
        .file_version = FileInfoEntry::CACHE_FILE_VERSION,
        .config_struct_hash = get_hash(type),
        .file_type = type,
        .source_hash = GetSourceFileCacheVersionHash(),
        .build_name = build_name,
        .profile = parent.profile,
    };

    file.Append(CacheEntryType::FILE_INFO, 0, entry, false);
    return true;
}

bool ShaderDiskCache::InitVSCache(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) {
    std::vector<size_t> pending_configs;
    std::unordered_map<u64, size_t> pending_programs;
    std::unique_ptr<Pica::ShaderSetup> shader_setup;
    std::unique_ptr<CacheFile> regenerate_file;

    auto cleanup_on_error = [&]() {
        programmable_vertex_cache.clear();
        programmable_vertex_map.clear();
        known_vertex_programs.clear();
#ifdef __SWITCH__
        hen_vs_heal.clear();
#endif
        if (regenerate_file) {
            regenerate_file->SwitchMode(CacheFile::CacheOpMode::DELETE);
        }
    };

    LOG_INFO(Render_Vulkan, "Loading VS disk shader cache for title {:016X}", title_id);

#ifdef __SWITCH__
    // (hen) hen_vs_on_temp: the second pass of a deferred regeneration (HenDeferVS), reading
    // the file the regenerated entries go to as the ordinary cache it is.
    vs_cache.SetFilePath(GetVSFile(title_id, hen_vs_on_temp));
#else
    vs_cache.SetFilePath(GetVSFile(title_id, false));
#endif

    if (!vs_cache.SwitchMode(CacheFile::CacheOpMode::READ)) {
        LOG_INFO(Render_Vulkan, "Missing VS disk shader cache for title {:016X}", title_id);
        cleanup_on_error();
        return false;
    }

    u32 tot_entries = vs_cache.GetTotalEntries();
    auto curr = vs_cache.ReadFirst();
    if (!curr.Valid() || curr.Type() != CacheEntryType::FILE_INFO) {
        MALFORMED_DISK_CACHE;
    }

    const FileInfoEntry* file_info = curr.Payload<FileInfoEntry>();
    if (!file_info || file_info->cache_magic != FileInfoEntry::CACHE_FILE_MAGIC ||
        file_info->file_version != FileInfoEntry::CACHE_FILE_VERSION ||
        file_info->file_type != CacheFileType::VS_CACHE) {
        MALFORMED_DISK_CACHE;
    }

    if (file_info->config_struct_hash != PicaVSConfigState::StructHash()) {
        LOG_ERROR(Render_Vulkan,
                  "Cache was created for a different PicaVSConfigState, resetting...");
        cleanup_on_error();
        return false;
    }

#ifdef __SWITCH__
    if (HenRegenWanted() && (file_info->source_hash != GetSourceFileCacheVersionHash() ||
                             file_info->profile != parent.profile)) {
        // (hen) Not regenerated here, before the title starts, but by the shader workers
        // while it runs.
        return HenDeferVS(stop_loading, callback, curr, tot_entries);
    }
#endif

    if (file_info->source_hash != GetSourceFileCacheVersionHash()) {
        LOG_INFO(Render_Vulkan, "Cache contains old vertex program, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetVSFile(title_id, true));
    }

    if (file_info->profile != parent.profile && !regenerate_file) {
        LOG_INFO(Render_Vulkan,
                 "Cache has driver and user settings mismatch, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetVSFile(title_id, true));
    }

    if (regenerate_file) {
        RecreateCache(*regenerate_file, CacheFileType::VS_CACHE);
    }

    CacheEntry::CacheEntryHeader curr_header = curr.Header();
    size_t curr_offset = curr.Position();

    size_t current_callback_index = 0;
    size_t tot_callback_index = tot_entries - 1;

    // Scan the entire file first, while keeping track of configs and programs.
    // SPIRV can be compiled directly and will be linked to the proper config entries
    // later.
    for (int i = 1; i < tot_entries; i++) {

        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        std::tie(curr_offset, curr_header) = vs_cache.ReadNextHeader(curr_header, curr_offset);

        if (!curr_header.Valid()) {
            MALFORMED_DISK_CACHE;
        }

        LOG_DEBUG(Render_Vulkan, "Processing ID: {:016X} (type {})", curr_header.Id(),
                  curr_header.Type());

        if (curr_header.Type() == CacheEntryType::VS_CONFIG) {
            pending_configs.push_back(curr_offset);
        } else if (curr_header.Type() == CacheEntryType::VS_PROGRAM) {
            pending_programs.try_emplace(curr_header.Id(), curr_offset);

            // We won't use this entry sequentially again, so report progress.
            if (callback) {
                callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                         tot_callback_index, "Vertex Shader");
            }
        } else if (curr_header.Type() == CacheEntryType::VS_SPIRV) {

            // Only use SPIRV entries if we are not regenerating the cache, as the driver or
            // user settings do not match, which could lead to different SPIRV.
            // These will be re-created from the cached config and programs later.
            if (!regenerate_file) {
                LOG_DEBUG(Render_Vulkan, "    processing SPIRV.");

                curr = vs_cache.ReadAt(curr_offset);
                if (!curr.Valid() || curr.Type() != CacheEntryType::VS_SPIRV) {
                    MALFORMED_DISK_CACHE;
                }

                const u8* spirv_data = curr.Data().data();
                const size_t spirv_size = curr.Data().size();

                auto [iter_prog, new_program] =
                    programmable_vertex_cache.try_emplace(curr.Id(), parent.instance);
                if (new_program) {
                    LOG_DEBUG(Render_Vulkan, "    compiling SPIRV.");

                    const auto spirv = std::span<const u32>(
                        reinterpret_cast<const u32*>(spirv_data), spirv_size / sizeof(u32));

                    iter_prog->second.module = CompileSPV(spirv, parent.instance.GetDevice());
                    iter_prog->second.MarkDone();

                    if (!iter_prog->second.module) {
                        // Compilation failed for some reason, remove from cache to let it
                        // be re-generated at runtime or during config and program processing.
                        LOG_ERROR(Render_Vulkan, "Unexpected program compilation failure");
                        programmable_vertex_cache.erase(iter_prog);
                    }
                }
            }

            if (callback) {
                callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                         tot_callback_index, "Vertex Shader");
            }
        } else {
            MALFORMED_DISK_CACHE;
        }
    }

    // Once we have all the shader instances created from SPIRV, we can link them to the VS configs.
    LOG_DEBUG(Render_Vulkan, "Linking with config entries.");

    // Mmultiple config entries may point to the same program entry. We could load all program
    // entries to memory to prevent having to read them from disk on every config entry, but program
    // entries are pretty big (around 50KB each). A LRU cache is a middle point between disk access
    // and memory usage.
    std::unique_ptr<Common::StaticLRUCache<u64, VSProgramEntry, 10>> program_lru =
        std::make_unique<Common::StaticLRUCache<u64, VSProgramEntry, 10>>();

    for (auto& offset : pending_configs) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        if (callback) {
            callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                     tot_callback_index, "Vertex Shader");
        }

        curr = vs_cache.ReadAt(offset);
        const VSConfigEntry* entry;

        if (!curr.Valid() || curr.Type() != CacheEntryType::VS_CONFIG ||
            !(entry = curr.Payload<VSConfigEntry>()) ||
            entry->version != VSConfigEntry::EXPECTED_VERSION) {
            MALFORMED_DISK_CACHE;
        }

        if (curr.Id() != entry->vs_config.Hash()) {
            LOG_ERROR(Render_Vulkan, "Unexpected PicaVSConfig hash mismatch");
            continue;
        }

        LOG_DEBUG(Render_Vulkan, "Linking {:016X}.", curr.Id());

        auto [iter_config, new_config] = programmable_vertex_map.try_emplace(curr.Id());
        if (new_config) {
            // New config entry, usually always taken unless there is duplicate entries on the cache
            // for some reason.

            // We cannot trust the SPIRV entry ID anymore if we are regenerating.
            auto shader_it = regenerate_file
                                 ? programmable_vertex_cache.end()
                                 : programmable_vertex_cache.find(entry->spirv_entry_id);
            if (shader_it != programmable_vertex_cache.end()) {
                // The config entry uses a SPIRV entry that was already compiled (this is the usual
                // path when the cache doesn't need to be re-generated).

                LOG_DEBUG(Render_Vulkan, "    linked with existing SPIRV {:016X}.",
                          entry->spirv_entry_id);

                iter_config->second = &shader_it->second;
                known_vertex_programs.emplace(entry->program_entry_id).second;
            } else {
                // Cached SPIRV not found, need to recompile.

                // Search program entry in a LRU first, to prevent having to read from the cache
                // file on each separate config entry.
                auto [found, program_lru_entry] = program_lru->request(entry->program_entry_id);
                if (!found) {
                    LOG_DEBUG(Render_Vulkan, "    reading program {:016X}.",
                              entry->program_entry_id);

                    // Program not on the LRU, need to read it from cache file
                    auto program_it = pending_programs.find(entry->program_entry_id);
                    if (program_it == pending_programs.end()) {
                        // Program code not in disk cache, should never happen.
                        LOG_ERROR(Render_Vulkan, "Missing program code for config entry");
                        programmable_vertex_map.erase(iter_config);
                        continue;
                    }

                    auto program_cache_entry = vs_cache.ReadAt(program_it->second);
                    const VSProgramEntry* program_entry;

                    if (!program_cache_entry.Valid() ||
                        program_cache_entry.Type() != CacheEntryType::VS_PROGRAM ||
                        !(program_entry = program_cache_entry.Payload<VSProgramEntry>()) ||
                        program_entry->version != VSProgramEntry::EXPECTED_VERSION) {
                        MALFORMED_DISK_CACHE;
                    }

                    program_lru_entry = *program_entry;

                    bool new_program =
                        known_vertex_programs.emplace(entry->program_entry_id).second;

                    if (new_program && regenerate_file) {
                        // When regenerating, only append if it's a new program entry not seen
                        // before.
                        AppendVSProgram(*regenerate_file, program_lru_entry,
                                        entry->program_entry_id);
                    }
                }

                // Recompile SPIRV from config and program now.
                LOG_DEBUG(Render_Vulkan, "    using program {:016X}.", entry->program_entry_id);

                shader_setup = std::make_unique<Pica::ShaderSetup>();
                shader_setup->UpdateProgramCode(program_lru_entry.program_code,
                                                program_lru_entry.program_len);
                shader_setup->UpdateSwizzleData(program_lru_entry.swizzle_code,
                                                program_lru_entry.swizzle_len);
                shader_setup->DoProgramCodeFixup();

                if (entry->vs_config.state.program_hash != shader_setup->GetProgramCodeHash() ||
                    entry->vs_config.state.swizzle_hash != shader_setup->GetSwizzleDataHash()) {
                    LOG_ERROR(Render_Vulkan, "Unexpected ShaderSetup hash mismatch");
                    programmable_vertex_map.erase(iter_config);
                    continue;
                }

                ExtraVSConfig extra_config = parent.CalcExtraConfig(entry->vs_config);

                auto program_glsl = Common::HashableString(
                    GLSL::GenerateVertexShader(*shader_setup, entry->vs_config, extra_config));
                if (program_glsl.empty()) {
                    LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
                    programmable_vertex_map.erase(iter_config);
                    continue;
                }

                const u64 spirv_id = program_glsl.Hash();

                auto [iter_prog, new_spirv] =
                    programmable_vertex_cache.try_emplace(spirv_id, parent.instance);

                LOG_DEBUG(Render_Vulkan, "    processing SPIRV.");

                if (new_spirv) {
                    LOG_DEBUG(Render_Vulkan, "    compiling SPIRV.");

                    auto spirv = CompileGLSL(program_glsl, vk::ShaderStageFlagBits::eVertex);

                    iter_prog->second.module = CompileSPV(spirv, parent.instance.GetDevice());
                    iter_prog->second.MarkDone();

                    if (regenerate_file) {
                        // If we are regenerating, save the new spirv to disk.
                        AppendVSSPIRV(*regenerate_file, spirv, spirv_id);
                    }
#ifdef __SWITCH__
                    else if (HenRegenWanted() && spirv_id == entry->spirv_entry_id) {
                        // (hen) A current file with a configuration whose SPIR-V is not in it
                        // (written by a draw while its shader waited to be regenerated, and
                        // the session ended before that): upstream compiles it at every start
                        // and never writes it. It is written below, so that this happens once
                        // and so that HenPrepareVS can rely on "ready means in the file".
                        hen_vs_heal.emplace_back(spirv_id, std::move(spirv));
                    }
#endif
                }

                if (regenerate_file) {
                    // If we are regenerating, save the config entry to the cache. We need to make a
                    // copy first because it's possible the SPIRV id has changed and we need to
                    // adjust it.
                    std::unique_ptr<VSConfigEntry> entry_copy =
                        std::make_unique<VSConfigEntry>(*entry);
                    entry_copy->spirv_entry_id = spirv_id;
                    AppendVSConfig(*regenerate_file, *entry_copy, curr.Id());
                }

                // Asign the SPIRV shader to the config
                iter_config->second = &iter_prog->second;

                LOG_DEBUG(Render_Vulkan, "    linked with SPIRV {:016X}.", entry->spirv_entry_id);
            }
        }
    }

    if (regenerate_file) {
        // If we are regenerating, replace the old file with the new one.
        vs_cache.SwitchMode(CacheFile::CacheOpMode::DELETE);
        regenerate_file.reset();
        FileUtil::Rename(GetVSFile(title_id, true), GetVSFile(title_id, false));
    }

#ifdef __SWITCH__
    {
        const bool appendable = vs_cache.SwitchMode(CacheFile::CacheOpMode::APPEND);
        if (appendable) {
            for (const auto& [spirv_id, spirv] : hen_vs_heal) {
                AppendVSSPIRV(vs_cache, spirv, spirv_id);
            }
        }
        hen_vs_heal.clear();
        return appendable;
    }
#else
    // Switch to append mode to receive new entries.
    return vs_cache.SwitchMode(CacheFile::CacheOpMode::APPEND);
#endif
}

bool ShaderDiskCache::InitFSCache(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) {
    std::vector<std::pair<u64, size_t>> pending_configs;
    std::unique_ptr<CacheFile> regenerate_file;

    auto cleanup_on_error = [&]() {
        fragment_shaders.clear();
        if (regenerate_file) {
            regenerate_file->SwitchMode(CacheFile::CacheOpMode::DELETE);
        }
    };

    LOG_INFO(Render_Vulkan, "Loading FS disk shader cache for title {:016X}", title_id);

#ifdef __SWITCH__
    fs_cache.SetFilePath(GetFSFile(title_id, hen_fs_on_temp)); // (as in InitVSCache)
#else
    fs_cache.SetFilePath(GetFSFile(title_id, false));
#endif

    if (!fs_cache.SwitchMode(CacheFile::CacheOpMode::READ)) {
        LOG_INFO(Render_Vulkan, "Missing FS disk shader cache for title {:016X}", title_id);
        cleanup_on_error();
        return false;
    }

    u32 tot_entries = fs_cache.GetTotalEntries();
    auto curr = fs_cache.ReadFirst();
    if (!curr.Valid() || curr.Type() != CacheEntryType::FILE_INFO) {
        MALFORMED_DISK_CACHE;
    }

    const FileInfoEntry* file_info = curr.Payload<FileInfoEntry>();
    if (!file_info || file_info->cache_magic != FileInfoEntry::CACHE_FILE_MAGIC ||
        file_info->file_version != FileInfoEntry::CACHE_FILE_VERSION ||
        file_info->file_type != CacheFileType::FS_CACHE) {
        MALFORMED_DISK_CACHE;
    }

    if (file_info->config_struct_hash != FSConfig::StructHash()) {
        LOG_ERROR(Render_Vulkan, "Cache was created for a different FSConfig, resetting...");
        cleanup_on_error();
        return false;
    }

#ifdef __SWITCH__
    if (HenRegenWanted() && (file_info->source_hash != GetSourceFileCacheVersionHash() ||
                             file_info->profile != parent.profile)) {
        return HenDeferFS(stop_loading, callback, curr, tot_entries); // (as in InitVSCache)
    }
#endif

    if (file_info->source_hash != GetSourceFileCacheVersionHash()) {
        LOG_INFO(Render_Vulkan, "Cache contains old fragment program, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetFSFile(title_id, true));
    }

    if (file_info->profile != parent.profile && !regenerate_file) {
        LOG_INFO(Render_Vulkan,
                 "Cache has driver and user settings mismatch, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetFSFile(title_id, true));
    }

    if (regenerate_file) {
        RecreateCache(*regenerate_file, CacheFileType::FS_CACHE);
    }

    CacheEntry::CacheEntryHeader curr_header = curr.Header();
    size_t curr_offset = curr.Position();

    size_t current_callback_index = 0;
    size_t tot_callback_index = tot_entries - 1;

    // Scan the entire file first, while keeping track of configs.
    // SPIRV can be compiled directly, if a config has a missing
    // SPIRV entry it can be regenerated later.
    for (int i = 1; i < tot_entries; i++) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        std::tie(curr_offset, curr_header) = fs_cache.ReadNextHeader(curr_header, curr_offset);

        if (!curr_header.Valid()) {
            MALFORMED_DISK_CACHE;
        }

        LOG_DEBUG(Render_Vulkan, "Processing ID: {:016X} (type {})", curr_header.Id(),
                  curr_header.Type());

        if (curr_header.Type() == CacheEntryType::FS_CONFIG) {
            pending_configs.push_back({curr_header.Id(), curr_offset});
        } else if (curr_header.Type() == CacheEntryType::FS_SPIRV) {

            // Only use SPIRV entries if we are not regenerating the cache, as the driver or
            // user settings do not match, which could lead to different SPIRV.
            // These will be regenerated from the cached config later.
            if (!regenerate_file) {
                LOG_DEBUG(Render_Vulkan, "    processing SPIRV.");

                curr = fs_cache.ReadAt(curr_offset);
                if (!curr.Valid() || curr.Type() != CacheEntryType::FS_SPIRV) {
                    MALFORMED_DISK_CACHE;
                }

                const u8* spirv_data = curr.Data().data();
                const size_t spirv_size = curr.Data().size();

                auto [iter_spirv, new_program] =
                    fragment_shaders.try_emplace(curr.Id(), parent.instance);
                if (new_program) {
                    LOG_DEBUG(Render_Vulkan, "    compiling SPIRV.");

                    const auto spirv = std::span<const u32>(
                        reinterpret_cast<const u32*>(spirv_data), spirv_size / sizeof(u32));

                    iter_spirv->second.module = CompileSPV(spirv, parent.instance.GetDevice());
                    iter_spirv->second.MarkDone();

                    if (!iter_spirv->second.module) {
                        // Compilation failed for some reason, remove from cache to let it
                        // be regenerated at runtime or during config processing.
                        LOG_ERROR(Render_Vulkan, "Unexpected program compilation failure");
                        fragment_shaders.erase(iter_spirv);
                    }
                }
            }

            if (callback) {
                callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                         tot_callback_index, "Fragment Shader");
            }
        } else {
            MALFORMED_DISK_CACHE;
        }
    }

    // Once we have all the shader instances created from SPIRV, we can link them to the FS configs.
    LOG_DEBUG(Render_Vulkan, "Linking with config entries.");

    for (auto& offset : pending_configs) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        if (callback) {
            callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                     tot_callback_index, "Fragment Shader");
        }

        LOG_DEBUG(Render_Vulkan, "Linking {:016X}.", offset.first);

        if (fragment_shaders.find(offset.first) != fragment_shaders.end()) {
            // SPIRV of config was already compiled, no need to regenerate
            // it from the cache. This can only happen if we are not regenerating
            // the cache.
            LOG_DEBUG(Render_Vulkan, "    linked with existing SPIRV.");
            continue;
        }

        // Cached SPIRV not found, need to recompile. Should only happen if
        // we are regenerating the cache.

        curr = fs_cache.ReadAt(offset.second);
        const FSConfigEntry* entry;

        if (!curr.Valid() || curr.Type() != CacheEntryType::FS_CONFIG ||
            !(entry = curr.Payload<FSConfigEntry>()) ||
            entry->version != FSConfigEntry::EXPECTED_VERSION) {
            MALFORMED_DISK_CACHE;
        }

        const auto fs_config_hash = entry->fs_config.Hash();
        if (curr.Id() != fs_config_hash) {
            LOG_ERROR(Render_Vulkan, "Unexpected FSConfig hash mismatch");
            continue;
        }

        const auto [it, new_shader] = fragment_shaders.try_emplace(fs_config_hash, parent.instance);
        auto& shader = it->second;

        std::vector<u32> spirv;
        if (parent.profile.vk_use_spirv_generator &&
            !entry->fs_config.UsesSpirvIncompatibleConfig()) {
            // Use SPIRV generator directly

            spirv = SPIRV::GenerateFragmentShader(entry->fs_config, parent.profile);
            shader.module = CompileSPV(spirv, parent.instance.GetDevice());
        } else {
            // Use GLSL generator then convert to SPIRV

            UserConfig user{};
            const std::string code_glsl =
                GLSL::GenerateFragmentShader(entry->fs_config, user, parent.profile);

            if (code_glsl.empty()) {
                LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
                fragment_shaders.erase(it);
                continue;
            }

            spirv = CompileGLSL(code_glsl, vk::ShaderStageFlagBits::eFragment);
            shader.module = CompileSPV(spirv, parent.instance.GetDevice());
        }
        shader.MarkDone();

        if (regenerate_file) {
            // Append the config and SPIRV to the new file.
            AppendFSSPIRV(*regenerate_file, spirv, fs_config_hash);
            AppendFSConfig(*regenerate_file, *entry, fs_config_hash);
        }

        LOG_DEBUG(Render_Vulkan, "    linked with new SPIRV.");
    }

    if (regenerate_file) {
        // If we are regenerating, replace the old file with the new one.
        fs_cache.SwitchMode(CacheFile::CacheOpMode::DELETE);
        regenerate_file.reset();
        FileUtil::Rename(GetFSFile(title_id, true), GetFSFile(title_id, false));
    }

    // Switch to append mode to receive new entries.
    return fs_cache.SwitchMode(CacheFile::CacheOpMode::APPEND);
}

bool ShaderDiskCache::InitGSCache(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) {
    std::vector<std::pair<u64, size_t>> pending_configs;
    std::unique_ptr<CacheFile> regenerate_file;

    auto cleanup_on_error = [&]() {
        fixed_geometry_shaders.clear();
        if (regenerate_file) {
            regenerate_file->SwitchMode(CacheFile::CacheOpMode::DELETE);
        }
    };

    LOG_INFO(Render_Vulkan, "Loading GS disk shader cache for title {:016X}", title_id);

    gs_cache.SetFilePath(GetGSFile(title_id, false));

    if (!gs_cache.SwitchMode(CacheFile::CacheOpMode::READ)) {
        LOG_INFO(Render_Vulkan, "Missing GS disk shader cache for title {:016X}", title_id);
        cleanup_on_error();
        return false;
    }

    u32 tot_entries = gs_cache.GetTotalEntries();
    auto curr = gs_cache.ReadFirst();
    if (!curr.Valid() || curr.Type() != CacheEntryType::FILE_INFO) {
        MALFORMED_DISK_CACHE;
    }

    const FileInfoEntry* file_info = curr.Payload<FileInfoEntry>();
    if (!file_info || file_info->cache_magic != FileInfoEntry::CACHE_FILE_MAGIC ||
        file_info->file_version != FileInfoEntry::CACHE_FILE_VERSION ||
        file_info->file_type != CacheFileType::GS_CACHE) {
        MALFORMED_DISK_CACHE;
    }

    if (file_info->config_struct_hash != PicaGSConfigState::StructHash()) {
        LOG_ERROR(Render_Vulkan,
                  "Cache was created for a different PicaGSConfigState, resetting...");
        cleanup_on_error();
        return false;
    }

    // There is no need to load geometry shaders if we don't support them.
    // We can just load the known IDs and skip SPIRV and cache regeneration.
    const auto geo_shaders_needed = parent.instance.UseGeometryShaders() &&
                                    !parent.instance.IsFragmentShaderBarycentricSupported();

    if (geo_shaders_needed && file_info->source_hash != GetSourceFileCacheVersionHash()) {
        LOG_INFO(Render_Vulkan, "Cache contains old fragment program, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetGSFile(title_id, true));
    }

    if (geo_shaders_needed && file_info->profile != parent.profile && !regenerate_file) {
        LOG_INFO(Render_Vulkan,
                 "Cache has driver and user settings mismatch, cache needs regeneration.");
        regenerate_file = std::make_unique<CacheFile>(GetGSFile(title_id, true));
    }

    if (regenerate_file) {
        RecreateCache(*regenerate_file, CacheFileType::GS_CACHE);
    }

    CacheEntry::CacheEntryHeader curr_header = curr.Header();
    size_t curr_offset = curr.Position();

    size_t current_callback_index = 0;
    size_t tot_callback_index = tot_entries - 1;

    // Scan the entire file first, while keeping track of configs.
    // SPIRV can be compiled directly, if a config has a missing
    // SPIRV entry it can be regenerated later.
    for (int i = 1; i < tot_entries; i++) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        std::tie(curr_offset, curr_header) = gs_cache.ReadNextHeader(curr_header, curr_offset);

        if (!curr_header.Valid()) {
            MALFORMED_DISK_CACHE;
        }

        LOG_DEBUG(Render_Vulkan, "Processing ID: {:016X} (type {})", curr_header.Id(),
                  curr_header.Type());

        if (curr_header.Type() == CacheEntryType::GS_CONFIG) {
            if (geo_shaders_needed) {
                pending_configs.push_back({curr_header.Id(), curr_offset});
            } else {
                known_geometry_shaders.emplace(curr_header.Id());

                if (callback) {
                    callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                             tot_callback_index, "Geometry Shader");
                }
            }
        } else if (curr_header.Type() == CacheEntryType::GS_SPIRV) {

            // Only use SPIRV entries if we are not regenerating the cache, as the driver or
            // user settings do not match, which could lead to different SPIRV.
            // These will be regenerated from the cached config later.
            // Also, only use SPIRV entries if we support geometry shaders on this device.
            if (geo_shaders_needed && !regenerate_file) {
                LOG_DEBUG(Render_Vulkan, "    processing SPIRV.");

                curr = gs_cache.ReadAt(curr_offset);
                if (!curr.Valid() || curr.Type() != CacheEntryType::GS_SPIRV) {
                    MALFORMED_DISK_CACHE;
                }

                const u8* spirv_data = curr.Data().data();
                const size_t spirv_size = curr.Data().size();

                auto [iter_spirv, new_program] =
                    fixed_geometry_shaders.try_emplace(curr.Id(), parent.instance);
                if (new_program) {
                    LOG_DEBUG(Render_Vulkan, "    compiling SPIRV.");

                    const auto spirv = std::span<const u32>(
                        reinterpret_cast<const u32*>(spirv_data), spirv_size / sizeof(u32));

                    iter_spirv->second.module = CompileSPV(spirv, parent.instance.GetDevice());
                    iter_spirv->second.MarkDone();

                    if (!iter_spirv->second.module) {
                        // Compilation failed for some reason, remove from cache to let it
                        // be regenerated at runtime or during config processing.
                        LOG_ERROR(Render_Vulkan, "Unexpected program compilation failure");
                        fixed_geometry_shaders.erase(iter_spirv);
                    }
                }
            }

            if (callback) {
                callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                         tot_callback_index, "Geometry Shader");
            }
        } else {
            MALFORMED_DISK_CACHE;
        }
    }

    // Once we have all the shader instances created from SPIRV, we can link them to the FS configs.
    LOG_DEBUG(Render_Vulkan, "Linking with config entries.");

    for (auto& offset : pending_configs) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        if (callback) {
            callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                     tot_callback_index, "Geometry Shader");
        }

        LOG_DEBUG(Render_Vulkan, "Linking {:016X}.", offset.first);

        if (fixed_geometry_shaders.find(offset.first) != fixed_geometry_shaders.end()) {
            // SPIRV of config was already compiled, no need to regenerate
            // it from the cache. This can only happen if we are not regenerating
            // the cache.
            LOG_DEBUG(Render_Vulkan, "    linked with existing SPIRV.");
            continue;
        }

        // Cached SPIRV not found, need to recompile. Should only happen if
        // we are regenerating the cache.

        curr = gs_cache.ReadAt(offset.second);
        const GSConfigEntry* entry;

        if (!curr.Valid() || curr.Type() != CacheEntryType::GS_CONFIG ||
            !(entry = curr.Payload<GSConfigEntry>()) ||
            entry->version != GSConfigEntry::EXPECTED_VERSION) {
            MALFORMED_DISK_CACHE;
        }

        const auto gs_config_hash = entry->gs_config.Hash();
        if (curr.Id() != gs_config_hash) {
            LOG_ERROR(Render_Vulkan, "Unexpected PicaGSConfigState hash mismatch");
            continue;
        }

        const auto [it, new_shader] =
            fixed_geometry_shaders.try_emplace(gs_config_hash, parent.instance);
        auto& shader = it->second;

        std::vector<u32> spirv;
        ExtraFixedGSConfig extra;
        extra.use_clip_planes = parent.profile.has_clip_planes;
        extra.separable_shader = true;

        const auto code_glsl = GLSL::GenerateFixedGeometryShader(entry->gs_config, extra);

        if (code_glsl.empty()) {
            LOG_ERROR(Render_Vulkan, "Failed to retrieve fixed geometry shader");
            fixed_geometry_shaders.erase(it);
            continue;
        }

        spirv = CompileGLSL(code_glsl, vk::ShaderStageFlagBits::eGeometry);
        shader.module = CompileSPV(spirv, parent.instance.GetDevice());
        shader.MarkDone();

        if (regenerate_file) {
            // Append the config and SPIRV to the new file.
            AppendGSSPIRV(*regenerate_file, spirv, gs_config_hash);
            AppendGSConfig(*regenerate_file, *entry, gs_config_hash);
        }

        LOG_DEBUG(Render_Vulkan, "    linked with new SPIRV.");
    }

    if (regenerate_file) {
        // If we are regenerating, replace the old file with the new one.
        gs_cache.SwitchMode(CacheFile::CacheOpMode::DELETE);
        regenerate_file.reset();
        FileUtil::Rename(GetGSFile(title_id, true), GetGSFile(title_id, false));
    }

    // Switch to append mode to receive new entries.
    return gs_cache.SwitchMode(CacheFile::CacheOpMode::APPEND);
}

bool ShaderDiskCache::InitPLCache(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) {

    auto cleanup_on_error = [&]() {
#ifdef __SWITCH__
        hen_deferred_pipelines.clear();
#endif
        graphics_pipelines.clear();
    };

    LOG_INFO(Render_Vulkan, "Loading PL disk shader cache for title {:016X}", title_id);

    pl_cache.SetFilePath(GetPLFile(title_id, false));

    if (!pl_cache.SwitchMode(CacheFile::CacheOpMode::READ)) {
        LOG_INFO(Render_Vulkan, "Missing PL disk shader cache for title {:016X}", title_id);
        cleanup_on_error();
        return false;
    }

    u32 tot_entries = pl_cache.GetTotalEntries();
    auto curr = pl_cache.ReadFirst();
    if (!curr.Valid() || curr.Type() != CacheEntryType::FILE_INFO) {
        MALFORMED_DISK_CACHE;
    }

    const FileInfoEntry* file_info = curr.Payload<FileInfoEntry>();
    if (!file_info || file_info->cache_magic != FileInfoEntry::CACHE_FILE_MAGIC ||
        file_info->file_version != FileInfoEntry::CACHE_FILE_VERSION ||
        file_info->file_type != CacheFileType::PL_CACHE) {
        MALFORMED_DISK_CACHE;
    }

    if (file_info->config_struct_hash != StaticPipelineInfo::StructHash()) {
        LOG_ERROR(Render_Vulkan,
                  "Cache was created for a different StaticPipelineInfo, resetting...");
        cleanup_on_error();
        return false;
    }

    size_t current_callback_index = 0;
    size_t tot_callback_index = tot_entries - 1;

    // There is only one entry type in the pipeline info cache,
    // no need to keep track of anything.
    for (int i = 1; i < tot_entries; i++) {
        if (stop_loading) {
            cleanup_on_error();
            return true;
        }

        curr = pl_cache.ReadNext(curr);

        if (!curr.Valid()) {
            MALFORMED_DISK_CACHE;
        }

        LOG_DEBUG(Render_Vulkan, "Processing ID: {:016X} (type {})", curr.Id(), curr.Type());

        if (curr.Type() == CacheEntryType::PL_CONFIG) {

            const PLConfigEntry* entry;

            if (!(entry = curr.Payload<PLConfigEntry>()) ||
                entry->version != PLConfigEntry::EXPECTED_VERSION) {
                MALFORMED_DISK_CACHE;
            }

            if (callback) {
                callback(VideoCore::LoadCallbackStage::Build, current_callback_index++,
                         tot_callback_index, "Pipeline");
            }

            known_graphic_pipelines.emplace(curr.Id());
            auto pl_hash_opt = entry->pl_info.OptimizedHash(parent.instance);

            if (graphics_pipelines.find(pl_hash_opt) != graphics_pipelines.end()) {
                // Multiple entries can have the same optimized hash. Skip if that's the case.
                LOG_DEBUG(Render_Vulkan, "    skipping.", curr.Id());
                continue;
            }

            // Fetch all the shaders used in the pipeline,
            // if any is missing we cannot build it.
            std::array<Shader*, MAX_SHADER_STAGES> shaders;

            LOG_DEBUG(Render_Vulkan, "    uses VS: {:016X}, FS: {:016X}, GS: {:016X}",
                      entry->pl_info.shader_ids[ProgramType::VS],
                      entry->pl_info.shader_ids[ProgramType::FS],
                      entry->pl_info.shader_ids[ProgramType::GS]);

            if (entry->pl_info.shader_ids[ProgramType::VS]) {
                auto it_vs =
                    programmable_vertex_map.find(entry->pl_info.shader_ids[ProgramType::VS]);
                if (it_vs == programmable_vertex_map.end()) {
                    LOG_ERROR(Render_Vulkan, "Missing vertex shader {:016X} for pipeline {:016X}",
                              entry->pl_info.shader_ids[ProgramType::VS], curr.Id());
                    continue;
                }
                shaders[ProgramType::VS] = it_vs->second;
            } else {
                shaders[ProgramType::VS] = &parent.trivial_vertex_shader;
            }

            auto it_fs = fragment_shaders.find(entry->pl_info.shader_ids[ProgramType::FS]);
            if (it_fs == fragment_shaders.end()) {
                LOG_ERROR(Render_Vulkan, "Missing fragment shader {:016X} for pipeline {:016X}",
                          entry->pl_info.shader_ids[ProgramType::FS], curr.Id());
                continue;
            }
            shaders[ProgramType::FS] = &it_fs->second;

            if (parent.instance.UseGeometryShaders() &&
                !parent.instance.IsFragmentShaderBarycentricSupported() &&
                entry->pl_info.shader_ids[ProgramType::GS]) {
                auto it_gs =
                    fixed_geometry_shaders.find(entry->pl_info.shader_ids[ProgramType::GS]);
                if (it_gs == fixed_geometry_shaders.end()) {
                    LOG_ERROR(Render_Vulkan, "Missing geometry shader {:016X} for pipeline {:016X}",
                              entry->pl_info.shader_ids[ProgramType::GS], curr.Id());
                    continue;
                }
                shaders[ProgramType::GS] = &it_gs->second;
            } else {
                shaders[ProgramType::GS] = nullptr;
            }

            // Build the pipeline using the cached pipeline info.
            // The dynamic state can be left default initialized.
            PipelineInfo info{};
            info.state = entry->pl_info;

            auto [it_pl, _] = graphics_pipelines.try_emplace(pl_hash_opt);
            it_pl.value() = std::make_unique<GraphicsPipeline>(
                parent.instance, parent.renderpass_cache, info, *parent.driver_pipeline_cache,
                *parent.pipeline_layout, shaders, &parent.pipeline_workers);

            it_pl.value()->TryBuild(false);
#ifdef __SWITCH__
            // (hen) With a shader still waiting to be regenerated, TryBuild did nothing. The
            // renderer's thread tries again when the shaders are there (HenRegenPoll), so that
            // the cached pipelines are built ahead of their first draw as after any start.
            if (std::any_of(shaders.begin(), shaders.end(),
                            [](Shader* shader) { return shader && !shader->IsDone(); })) {
                hen_deferred_pipelines.push_back({it_pl.value().get(), shaders});
            }
#endif

            LOG_DEBUG(Render_Vulkan, "    built.");

        } else {
            MALFORMED_DISK_CACHE;
        }
    }

    // Switch to append mode to receive new entries.
    return pl_cache.SwitchMode(CacheFile::CacheOpMode::APPEND);
}

void ShaderDiskCache::AppendVSConfigProgram(CacheFile& file,
                                            const Pica::Shader::Generator::PicaVSConfig& config,
                                            const Pica::ShaderSetup& setup, u64 config_id,
                                            u64 spirv_id) {

    VSConfigEntry entry;
    entry.version = VSConfigEntry::EXPECTED_VERSION;
    entry.vs_config = config;
    entry.spirv_entry_id = spirv_id;
    entry.program_entry_id =
        Common::HashCombine(config.state.program_hash, config.state.swizzle_hash);

    bool new_entry = known_vertex_programs.emplace(entry.program_entry_id).second;
    if (new_entry) {
        std::unique_ptr<VSProgramEntry> prog_entry = std::make_unique<VSProgramEntry>();
        prog_entry->version = VSProgramEntry::EXPECTED_VERSION;
        prog_entry->program_len = setup.GetBiggestProgramSize();
        prog_entry->program_code = setup.GetProgramCode();
        prog_entry->swizzle_len = setup.GetBiggestSwizzleSize();
        prog_entry->swizzle_code = setup.GetSwizzleData();

        AppendVSProgram(file, *prog_entry, entry.program_entry_id);
    }

    AppendVSConfig(file, entry, config_id);
}

void ShaderDiskCache::AppendVSProgram(CacheFile& file, const VSProgramEntry& entry,
                                      u64 program_id) {
    file.Append(CacheEntryType::VS_PROGRAM, program_id, entry, true);
}

void ShaderDiskCache::AppendVSConfig(CacheFile& file, const VSConfigEntry& entry, u64 config_id) {
    file.Append(CacheEntryType::VS_CONFIG, config_id, entry, true);
}

void ShaderDiskCache::AppendVSSPIRV(CacheFile& file, std::span<const u32> program, u64 program_id) {
    file.Append(CacheEntryType::VS_SPIRV, program_id,
                {reinterpret_cast<const u8*>(program.data()), program.size() * sizeof(u32)}, true);
}

void ShaderDiskCache::AppendFSConfig(CacheFile& file, const FSConfigEntry& entry, u64 config_id) {
    file.Append(CacheEntryType::FS_CONFIG, config_id, entry, true);
}

void ShaderDiskCache::AppendFSSPIRV(CacheFile& file, std::span<const u32> program, u64 program_id) {
    file.Append(CacheEntryType::FS_SPIRV, program_id,
                {reinterpret_cast<const u8*>(program.data()), program.size() * sizeof(u32)}, true);
}

void ShaderDiskCache::AppendGSConfig(CacheFile& file, const GSConfigEntry& entry, u64 config_id) {
    file.Append(CacheEntryType::GS_CONFIG, config_id, entry, true);
}

void ShaderDiskCache::AppendGSSPIRV(CacheFile& file, std::span<const u32> program, u64 program_id) {
    file.Append(CacheEntryType::GS_SPIRV, program_id,
                {reinterpret_cast<const u8*>(program.data()), program.size() * sizeof(u32)}, true);
}

void ShaderDiskCache::AppendPLConfig(CacheFile& file, const PLConfigEntry& entry, u64 config_id) {
    file.Append(CacheEntryType::PL_CONFIG, config_id, entry, true);
}

#ifdef __SWITCH__
// ---------------------------------------------------------------------------------------------
// (hen) Deferred regeneration of the vertex and fragment shader caches.
//
// Upstream regenerates a cache file whose header names another build of the shader generator
// or another profile entry by entry inside Init, before the title starts: 8 s for a small
// cache on this console, 33 s seen once. Here (unless --cache-regen-sync) Init only reads the
// stale file's configurations into memory and gives every one of them its Shader object, not
// done yet, in the same maps as upstream. The title starts; the shader workers generate and
// compile the entries behind it. A draw that needs one of them finds a shader that is "being
// compiled", exactly as for a shader first seen in this session, and its entry is moved to the
// front (HenRegenBump).
//
// Files. F is the cache file ("..._vs.vkch"), T the name upstream regenerates into
// ("..._vs_temp.vkch"). F is never written while it is stale. T is created with the current
// header and is the session's live file: regenerated entries and the entries of shaders first
// seen in this session are appended to it, each written whole and flushed. When the last entry
// of the kind is in, T's append worker deletes F and renames T to F (CacheFile::HenSwapTo).
// Leaving earlier leaves F stale and T current but incomplete: the next launch reads F's
// configurations again, reads T as the ordinary cache it is (its shaders are ready at once) and
// regenerates what T does not have. A T that is unreadable or stale itself is made anew.
//
// Threads. The maps (programmable_vertex_cache, programmable_vertex_map, known_vertex_programs,
// fragment_shaders, graphics_pipelines) are touched by the thread that calls Init and then by
// the renderer's thread only, as upstream: every insertion the regeneration needs is made in
// Init (HenPrepareVS, HenPrepareFS), the vertex shaders' GLSL included, since their SPIR-V id is
// its hash. A worker's job has: its own Job (claimed once, by an atomic exchange); its own
// Shader, whose module it sets before MarkDone, the protocol of the jobs upstream queues from a
// draw; the profile, a copy made in Init; CacheFile::Append, which queues on the file's one
// append thread. Counters and flags are atomics; `active` (jobs that still hold this object) is
// under `mutex`, and the destructor waits for it to be zero.
// ---------------------------------------------------------------------------------------------

extern "C" int hen_cache_regen_sync; // hen/jit/hen_jit.c: --cache-regen-sync

struct ShaderDiskCache::HenRegen {
    struct Job {
        std::atomic<bool> claimed{false};
        bool vertex = false;
        Shader* shader = nullptr;
        u64 id = 0; ///< vertex: the SPIR-V entry's id; fragment: the configuration's hash
        std::unique_ptr<FSConfigEntry> fs_entry;
        /// (vertex) The configurations that use this shader: written after its SPIR-V.
        std::vector<std::pair<u64, VSConfigEntry>> vs_configs;
    };
    // Filled in Init, not changed after the first job is queued.
    std::deque<Job> jobs;
    std::unordered_map<const Shader*, u32> index;
    Pica::Shader::Profile profile{};
    bool vs_deferred = false, fs_deferred = false;
    std::string vs_final, fs_final;
    u32 vs_jobs = 0, vs_config_count = 0, fs_jobs = 0;
    std::chrono::steady_clock::time_point started{};
    std::chrono::nanoseconds prepare{};

    // What Init read from the stale files (freed in HenRegenStart).
    std::vector<std::pair<u64, VSConfigEntry>> vs_configs;
    std::unordered_set<u64> vs_seen;
    std::unordered_map<u64, std::shared_ptr<VSProgramEntry>> vs_programs;
    std::vector<std::pair<u64, FSConfigEntry>> fs_configs;
    std::unordered_set<u64> fs_seen;

    // Shared with the workers.
    std::atomic<u32> next{0}, done{0}, vs_left{0}, fs_left{0}, bumped{0};
    std::atomic<bool> stop{false}, paused{false};
    std::atomic<int> pumps{0};
    std::mutex mutex;
    std::condition_variable idle;
    int active = 0;

    void Enter() {
        std::scoped_lock lock{mutex};
        active++;
    }
    void Leave() {
        std::scoped_lock lock{mutex};
        if (--active == 0) {
            idle.notify_all();
        }
    }
    bool More() const {
        return !stop && !paused && next.load() < jobs.size();
    }
};

bool ShaderDiskCache::HenRegenWanted() const {
    return !hen_cache_regen_sync;
}

ShaderDiskCache::HenRegen& ShaderDiskCache::HenRegenState() {
    if (!hen_regen) {
        hen_regen = std::make_shared<HenRegen>();
        hen_regen->profile = parent.profile;
    }
    return *hen_regen;
}

ShaderDiskCache::~ShaderDiskCache() {
    HenRegenStop();
}

void ShaderDiskCache::CacheFile::HenClose() {
    file.Close();
    curr_mode = CacheOpMode::NONE;
    next_entry_id = SIZE_MAX;
    cached_file_data_start = 0;
    cached_file_data.clear();
}

void ShaderDiskCache::CacheFile::HenSwapTo(const std::string& final_path) {
    if (curr_mode != CacheOpMode::APPEND) {
        return;
    }
    append_worker.QueueWork([this, final_path] {
        if (next_entry_id == SIZE_MAX || !file.IsGood()) {
            // (a write failed on the way: the old file stays, stale, and the next launch
            // starts again)
            std::printf("hen: gfx: shader cache: %s is not complete, %s stays\n",
                        filepath.c_str(), final_path.c_str());
            return;
        }
        file.Flush();
        file.Close();
        FileUtil::Delete(final_path);
        if (FileUtil::Rename(filepath, final_path)) {
            filepath = final_path;
        } else {
            // (Init gives it its name at the next launch if the old file is gone)
            std::printf("hen: gfx: shader cache: %s could not be renamed to %s\n",
                        filepath.c_str(), final_path.c_str());
        }
        file = FileUtil::IOFile(filepath, "ab");
    });
}

bool ShaderDiskCache::HenHarvestVS(const std::atomic_bool& stop_loading, const CacheEntry& first,
                                   u32 tot_entries) {
    HenRegen& regen = HenRegenState();
    const std::size_t kept = regen.vs_configs.size();
    const auto malformed = [&] {
        LOG_ERROR(Render_Vulkan, "Malformed disk shader cache");
        regen.vs_configs.erase(regen.vs_configs.begin() + kept, regen.vs_configs.end());
        return false;
    };
    CacheEntry::CacheEntryHeader header = first.Header();
    size_t offset = first.Position();
    for (u32 i = 1; i < tot_entries && !stop_loading; i++) {
        std::tie(offset, header) = vs_cache.ReadNextHeader(header, offset);
        if (!header.Valid()) {
            return malformed();
        }
        if (header.Type() == CacheEntryType::VS_CONFIG) {
            const CacheEntry entry = vs_cache.ReadAt(offset);
            const VSConfigEntry* config;
            if (!entry.Valid() || entry.Type() != CacheEntryType::VS_CONFIG ||
                !(config = entry.Payload<VSConfigEntry>()) ||
                config->version != VSConfigEntry::EXPECTED_VERSION) {
                return malformed();
            }
            if (entry.Id() != config->vs_config.Hash()) {
                LOG_ERROR(Render_Vulkan, "Unexpected PicaVSConfig hash mismatch");
                continue;
            }
            if (regen.vs_seen.insert(entry.Id()).second) {
                regen.vs_configs.emplace_back(entry.Id(), *config);
            }
        } else if (header.Type() == CacheEntryType::VS_PROGRAM) {
            if (regen.vs_programs.contains(header.Id())) {
                continue;
            }
            const CacheEntry entry = vs_cache.ReadAt(offset);
            const VSProgramEntry* program;
            if (!entry.Valid() || entry.Type() != CacheEntryType::VS_PROGRAM ||
                !(program = entry.Payload<VSProgramEntry>()) ||
                program->version != VSProgramEntry::EXPECTED_VERSION) {
                return malformed();
            }
            regen.vs_programs.emplace(header.Id(), std::make_shared<VSProgramEntry>(*program));
        } else if (header.Type() != CacheEntryType::VS_SPIRV) {
            // (the SPIR-V of a stale file is of no use, as upstream)
            return malformed();
        }
    }
    return true;
}

bool ShaderDiskCache::HenHarvestFS(const std::atomic_bool& stop_loading, const CacheEntry& first,
                                   u32 tot_entries) {
    HenRegen& regen = HenRegenState();
    const std::size_t kept = regen.fs_configs.size();
    const auto malformed = [&] {
        LOG_ERROR(Render_Vulkan, "Malformed disk shader cache");
        regen.fs_configs.erase(regen.fs_configs.begin() + kept, regen.fs_configs.end());
        return false;
    };
    CacheEntry::CacheEntryHeader header = first.Header();
    size_t offset = first.Position();
    for (u32 i = 1; i < tot_entries && !stop_loading; i++) {
        std::tie(offset, header) = fs_cache.ReadNextHeader(header, offset);
        if (!header.Valid()) {
            return malformed();
        }
        if (header.Type() == CacheEntryType::FS_CONFIG) {
            const CacheEntry entry = fs_cache.ReadAt(offset);
            const FSConfigEntry* config;
            if (!entry.Valid() || entry.Type() != CacheEntryType::FS_CONFIG ||
                !(config = entry.Payload<FSConfigEntry>()) ||
                config->version != FSConfigEntry::EXPECTED_VERSION) {
                return malformed();
            }
            if (entry.Id() != config->fs_config.Hash()) {
                LOG_ERROR(Render_Vulkan, "Unexpected FSConfig hash mismatch");
                continue;
            }
            if (regen.fs_seen.insert(entry.Id()).second) {
                regen.fs_configs.emplace_back(entry.Id(), *config);
            }
        } else if (header.Type() != CacheEntryType::FS_SPIRV) {
            return malformed();
        }
    }
    return true;
}

// Called by InitVSCache when the file it has open for reading (header checked) is stale.
// First pass (the file is F): F's configurations and programs are read, F is closed and left
// alone, and InitVSCache runs again on T. Second pass (the file is T, stale as well: two
// updates with an unfinished regeneration between them): T's are read too and false is
// returned, on which the first pass makes T anew. Returns what InitVSCache returns.
bool ShaderDiskCache::HenDeferVS(const std::atomic_bool& stop_loading,
                                 const VideoCore::DiskResourceLoadCallback& callback,
                                 const CacheEntry& first, u32 tot_entries) {
    const auto before = std::chrono::steady_clock::now();
    if (!HenHarvestVS(stop_loading, first, tot_entries)) {
        return false; // (the caller makes the file it was reading anew, as upstream)
    }
    if (hen_vs_on_temp) {
        return false;
    }
    if (stop_loading) {
        return true;
    }
    HenRegen& regen = HenRegenState();
    vs_cache.HenClose();
    hen_vs_on_temp = true;
    if (!InitVSCache(stop_loading, callback)) {
        RecreateCache(vs_cache, CacheFileType::VS_CACHE);
    }
    if (stop_loading) {
        return true;
    }
    regen.vs_deferred = true;
    regen.vs_final = GetVSFile(title_id, false);
    HenPrepareVS();
    regen.prepare += std::chrono::steady_clock::now() - before;
    return true;
}

bool ShaderDiskCache::HenDeferFS(const std::atomic_bool& stop_loading,
                                 const VideoCore::DiskResourceLoadCallback& callback,
                                 const CacheEntry& first, u32 tot_entries) {
    const auto before = std::chrono::steady_clock::now();
    if (!HenHarvestFS(stop_loading, first, tot_entries)) {
        return false;
    }
    if (hen_fs_on_temp) {
        return false;
    }
    if (stop_loading) {
        return true;
    }
    HenRegen& regen = HenRegenState();
    fs_cache.HenClose();
    hen_fs_on_temp = true;
    if (!InitFSCache(stop_loading, callback)) {
        RecreateCache(fs_cache, CacheFileType::FS_CACHE);
    }
    if (stop_loading) {
        return true;
    }
    regen.fs_deferred = true;
    regen.fs_final = GetFSFile(title_id, false);
    HenPrepareFS();
    regen.prepare += std::chrono::steady_clock::now() - before;
    return true;
}

// The part of upstream's regeneration that touches the maps, for every configuration of the
// stale file the live file does not have: program found and checked, GLSL generated, Shader
// created under the GLSL's hash and linked to the configuration. What is left for a worker is
// CompileGLSL (the slow part), the module and the appends.
void ShaderDiskCache::HenPrepareVS() {
    HenRegen& regen = *hen_regen;
    std::unordered_map<u64, std::unique_ptr<Pica::ShaderSetup>> setups;

    for (const auto& [config_id, config] : regen.vs_configs) {
        const auto [iter_config, new_config] = programmable_vertex_map.try_emplace(config_id);
        if (!new_config) {
            continue; // (the live file has it)
        }
        const auto program_it = regen.vs_programs.find(config.program_entry_id);
        if (program_it == regen.vs_programs.end()) {
            LOG_ERROR(Render_Vulkan, "Missing program code for config entry");
            programmable_vertex_map.erase(iter_config);
            continue;
        }
        const VSProgramEntry& program = *program_it->second;
        if (known_vertex_programs.emplace(config.program_entry_id).second) {
            AppendVSProgram(vs_cache, program, config.program_entry_id);
        }
        auto& setup = setups[config.program_entry_id];
        if (!setup) {
            setup = std::make_unique<Pica::ShaderSetup>();
            setup->UpdateProgramCode(program.program_code, program.program_len);
            setup->UpdateSwizzleData(program.swizzle_code, program.swizzle_len);
            setup->DoProgramCodeFixup();
        }
        if (config.vs_config.state.program_hash != setup->GetProgramCodeHash() ||
            config.vs_config.state.swizzle_hash != setup->GetSwizzleDataHash()) {
            LOG_ERROR(Render_Vulkan, "Unexpected ShaderSetup hash mismatch");
            programmable_vertex_map.erase(iter_config);
            continue;
        }
        const ExtraVSConfig extra_config = parent.CalcExtraConfig(config.vs_config);
        auto program_glsl = Common::HashableString(
            GLSL::GenerateVertexShader(*setup, config.vs_config, extra_config));
        if (program_glsl.empty()) {
            LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
            programmable_vertex_map.erase(iter_config);
            continue;
        }
        const u64 spirv_id = program_glsl.Hash();
        const auto [iter_prog, new_spirv] =
            programmable_vertex_cache.try_emplace(spirv_id, parent.instance);
        Shader& shader = iter_prog->second;
        // (the SPIR-V id may have changed with the generator: the entry is written with the
        // new one, as upstream)
        VSConfigEntry entry = config;
        entry.spirv_entry_id = spirv_id;
        regen.vs_config_count++;
        if (new_spirv) {
            shader.program = std::move(program_glsl);
            regen.index.emplace(&shader, static_cast<u32>(regen.jobs.size()));
            HenRegen::Job& job = regen.jobs.emplace_back();
            job.vertex = true;
            job.shader = &shader;
            job.id = spirv_id;
            job.vs_configs.emplace_back(config_id, entry);
            regen.vs_jobs++;
        } else if (const auto job_it = regen.index.find(&shader); job_it != regen.index.end()) {
            // (the shader of an earlier configuration, not compiled yet: this configuration
            // is written after its SPIR-V)
            regen.jobs[job_it->second].vs_configs.emplace_back(config_id, entry);
        } else {
            // (a shader the live file has the SPIR-V of)
            AppendVSConfig(vs_cache, entry, config_id);
        }
        iter_config->second = &shader;
    }
    regen.vs_left = regen.vs_jobs;
}

void ShaderDiskCache::HenPrepareFS() {
    HenRegen& regen = *hen_regen;
    for (const auto& [config_id, config] : regen.fs_configs) {
        const auto [it, new_shader] = fragment_shaders.try_emplace(config_id, parent.instance);
        if (!new_shader) {
            continue; // (the live file has it)
        }
        regen.index.emplace(&it->second, static_cast<u32>(regen.jobs.size()));
        HenRegen::Job& job = regen.jobs.emplace_back();
        job.shader = &it->second;
        job.id = config_id;
        job.fs_entry = std::make_unique<FSConfigEntry>(config);
        regen.fs_jobs++;
    }
    regen.fs_left = regen.fs_jobs;
}

void ShaderDiskCache::HenRegenStart(const std::atomic_bool& stop_loading) {
    if (!hen_regen) {
        return;
    }
    HenRegen& regen = *hen_regen;
    regen.vs_configs = {};
    regen.vs_seen = {};
    regen.vs_programs = {};
    regen.fs_configs = {};
    regen.fs_seen = {};
    if (!regen.vs_deferred && !regen.fs_deferred) {
        hen_regen.reset(); // (the stale file was unreadable: made anew, nothing to regenerate)
        return;
    }
    const int workers = std::max(1, static_cast<int>(parent.num_worker_threads) - 1);
    std::printf("hen: gfx: shader cache: regeneration deferred (%s%s%s stale): %u vertex shaders "
                "for %u configurations and %u fragment shaders go to %d shader workers; %zu "
                "cached pipelines wait for them; the start was held up %.1f ms\n",
                regen.vs_deferred ? "vertex" : "",
                regen.vs_deferred && regen.fs_deferred ? " and " : "",
                regen.fs_deferred ? "fragment" : "", regen.vs_jobs, regen.vs_config_count,
                regen.fs_jobs, workers, hen_deferred_pipelines.size(),
                std::chrono::duration<double, std::milli>(regen.prepare).count());
    regen.started = std::chrono::steady_clock::now();
    if (stop_loading) {
        return; // (loading was cancelled: a draw can still ask for an entry, see HenRegenBump)
    }
    // (a kind with nothing left to do: a regeneration that an earlier session all but ended)
    if (regen.vs_deferred && regen.vs_jobs == 0) {
        vs_cache.HenSwapTo(regen.vs_final);
    }
    if (regen.fs_deferred && regen.fs_jobs == 0) {
        fs_cache.HenSwapTo(regen.fs_final);
    }
    HenRegenKick();
}

// One entry: what upstream's regeneration does for it after the map insertions, or what the
// job queued by a draw does. Worker thread.
void ShaderDiskCache::HenRegenRun(HenRegen& regen, u32 index) {
    HenRegen::Job& job = regen.jobs[index];
    {
        HenGfxTimer hen_timer{HEN_GFX_SHADER_COMPILE};
        Shader& shader = *job.shader;
        const vk::Device device = parent.instance.GetDevice();
        if (job.vertex) {
            const auto spirv = CompileGLSL(shader.program, vk::ShaderStageFlagBits::eVertex);
            AppendVSSPIRV(vs_cache, spirv, job.id);
            for (const auto& [config_id, entry] : job.vs_configs) {
                AppendVSConfig(vs_cache, entry, config_id);
            }
            shader.program.clear();
            shader.module = CompileSPV(spirv, device);
            shader.MarkDone();
        } else {
            const FSConfig& fs_config = job.fs_entry->fs_config;
            std::vector<u32> spirv;
            bool generated = true;
            if (regen.profile.vk_use_spirv_generator && !fs_config.UsesSpirvIncompatibleConfig()) {
                spirv = SPIRV::GenerateFragmentShader(fs_config, regen.profile);
            } else {
                const UserConfig user{};
                const std::string code =
                    GLSL::GenerateFragmentShader(fs_config, user, regen.profile);
                generated = !code.empty();
                if (generated) {
                    spirv = CompileGLSL(code, vk::ShaderStageFlagBits::eFragment);
                } else {
                    LOG_ERROR(Render_Vulkan, "Failed to retrieve fragment shader");
                }
            }
            if (generated) {
                shader.module = CompileSPV(spirv, device);
                shader.MarkDone();
                AppendFSSPIRV(fs_cache, spirv, job.id);
                AppendFSConfig(fs_cache, *job.fs_entry, job.id);
            }
            // (else, which the generator has no path to today: upstream leaves the entry out
            // of the new file and of the map. Here it stays in the map, never done: its draws
            // are left out or drawn by the generic shader, and no pipeline gets a null module.)
        }
    }
    // The last entry of a kind: its file is complete and takes the stale one's place.
    if (job.vertex) {
        if (regen.vs_left.fetch_sub(1) == 1) {
            vs_cache.HenSwapTo(regen.vs_final);
        }
    } else if (regen.fs_left.fetch_sub(1) == 1) {
        fs_cache.HenSwapTo(regen.fs_final);
    }
    if (regen.done.fetch_add(1) + 1 == regen.jobs.size()) {
        std::printf("hen: gfx: shader cache: %u vertex / %u fragment entries regenerated in the "
                    "background in %.0f ms (%u of them asked for by a draw first)\n",
                    regen.vs_jobs, regen.fs_jobs,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              regen.started)
                        .count(),
                    regen.bumped.load());
    }
}

// A worker's turn: one entry, then to the back of the workers' queue again, so that a shader a
// draw asks for waits behind one entry of each worker at most, not behind the whole list.
void ShaderDiskCache::HenRegenPump(std::shared_ptr<HenRegen> regen) {
    if (!regen->stop && !regen->paused) {
        u32 index;
        while ((index = regen->next.fetch_add(1)) < regen->jobs.size()) {
            if (!regen->jobs[index].claimed.exchange(true)) {
                HenRegenRun(*regen, index);
                break;
            }
        }
    }
    const auto again = [this, &regen] {
        parent.shader_workers.QueueWork([this, regen] { HenRegenPump(regen); });
    };
    if (regen->More()) {
        again();
        return;
    }
    regen->pumps--;
    // (HenRegenResume clears `paused` and then reads `pumps`: either it saw this one gone and
    // queued another, or this one sees the flag cleared here and goes on)
    if (regen->More()) {
        regen->pumps++;
        again();
        return;
    }
    regen->Leave();
}

void ShaderDiskCache::HenRegenKick() {
    if (!hen_regen || hen_regen->stop) {
        return;
    }
    // One worker is left for the shaders the title asks for: the title is starting too.
    const int workers = std::max(1, static_cast<int>(parent.num_worker_threads) - 1);
    while (hen_regen->More() && hen_regen->pumps.load() < workers) {
        hen_regen->pumps++;
        hen_regen->Enter();
        parent.shader_workers.QueueWork([this, regen = hen_regen] { HenRegenPump(regen); });
    }
}

// Renderer's thread: a draw has a shader that is not done. If it is one waiting to be
// regenerated, its entry is queued now, as the job of a shader first seen would be.
void ShaderDiskCache::HenRegenBump(const Shader* shader) {
    HenRegen& regen = *hen_regen;
    if (regen.done.load(std::memory_order_relaxed) == regen.jobs.size()) {
        return;
    }
    const auto it = regen.index.find(shader);
    if (it == regen.index.end() || regen.jobs[it->second].claimed.exchange(true)) {
        return;
    }
    regen.bumped++;
    regen.Enter();
    parent.shader_workers.QueueWork([this, regen = hen_regen, index = it->second] {
        HenRegenRun(*regen, index);
        regen->Leave();
    });
}

void ShaderDiskCache::HenRegenPoll() {
    if (hen_deferred_pipelines.empty()) {
        return;
    }
    // One pipeline started every 2 ms at most: TryBuild first asks the driver for the pipeline
    // from its cache on this thread. (Not measured on the console.)
    const auto now = std::chrono::steady_clock::now();
    if (now - hen_deferred_last < std::chrono::milliseconds(2)) {
        return;
    }
    for (int looked = 0; looked < 16 && !hen_deferred_pipelines.empty(); looked++) {
        if (hen_deferred_cursor >= hen_deferred_pipelines.size()) {
            hen_deferred_cursor = 0;
        }
        HenDeferredPipeline& entry = hen_deferred_pipelines[hen_deferred_cursor];
        const bool built = entry.pipeline->IsDone(); // (a draw needed it meanwhile)
        if (!built && std::any_of(entry.shaders.begin(), entry.shaders.end(), [](Shader* shader) {
                return shader && !shader->IsDone();
            })) {
            hen_deferred_cursor++;
            continue;
        }
        GraphicsPipeline* const pipeline = entry.pipeline;
        entry = hen_deferred_pipelines.back();
        hen_deferred_pipelines.pop_back();
        if (!built) {
            pipeline->TryBuild(false); // (nothing if a draw has started it)
            hen_deferred_last = now;
            return;
        }
    }
}

void ShaderDiskCache::HenRegenPause() {
    if (hen_regen) {
        hen_regen->paused = true;
    }
}

void ShaderDiskCache::HenRegenResume() {
    if (hen_regen) {
        hen_regen->paused = false;
        HenRegenKick();
    }
}

void ShaderDiskCache::HenRegenStop() {
    if (!hen_regen) {
        return;
    }
    HenRegen& regen = *hen_regen;
    const bool was_stopped = regen.stop.exchange(true);
    {
        std::unique_lock lock{regen.mutex};
        regen.idle.wait(lock, [&regen] { return regen.active == 0; });
    }
    const u32 done = regen.done.load();
    if (!was_stopped && done < regen.jobs.size()) {
        std::printf("hen: gfx: shader cache: regeneration stopped at %u of %zu entries: the "
                    "next launch goes on from there\n",
                    done, regen.jobs.size());
    }
}
#endif // __SWITCH__

} // namespace Vulkan
