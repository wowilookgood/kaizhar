// Host tool for the generic fragment shader (glsl_fs_shader_gen.cpp).
//   gen generic 0|1     the generic shader's text (1: barycentric variant)
//   gen case SEED [canon]  a random valid fragment configuration (canon: made canonical first): its generated shader, whether the
//                       generic shader supports it, and the configuration as the generic one
//                       reads it (for difftest.py)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include "video_core/pica/regs_internal.h"
#include "video_core/shader/generator/glsl_fs_shader_gen.h"
#include "video_core/shader/generator/profile.h"

using namespace Pica::Shader;

static Profile MakeProfile(bool barycentric) {
    Profile profile{};
    profile.has_separable_shaders = true;
    profile.has_custom_border_color = true;
    profile.has_logic_op = true;
    profile.is_vulkan = true;
    profile.has_fragment_shader_barycentric = barycentric;
    return profile;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "generic") == 0) {
        const std::string code =
            Generator::GLSL::GenerateGenericFragmentShader(MakeProfile(std::atoi(argv[2]) != 0));
        std::fwrite(code.data(), 1, code.size(), stdout);
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "case") == 0) {
        std::mt19937 rng(static_cast<unsigned>(std::atoi(argv[2])));
        const auto pick = [&rng](std::initializer_list<unsigned> values) {
            return *(values.begin() + rng() % values.size());
        };
        const auto chance = [&rng](unsigned percent) { return rng() % 100 < percent; };
        auto regs = std::make_unique<Pica::RegsInternal>();
        std::memset(regs.get(), 0, sizeof(Pica::RegsInternal));
        FSConfig config{*regs};
        std::memset(&config, 0, sizeof(config));

        auto& fb = config.framebuffer;
        fb.alpha_test_func.Assign(static_cast<Pica::FramebufferRegs::CompareFunc>(
            chance(4) ? 0u : pick({1, 1, 1, 2, 3, 4, 5, 6, 7})));
        fb.scissor_test_mode.Assign(
            static_cast<Pica::RasterizerRegs::ScissorMode>(pick({0, 0, 1, 3})));
        fb.depthmap_enable.Assign(
            static_cast<Pica::RasterizerRegs::DepthBuffering>(pick({0, 1})));
        fb.logic_op.Assign(Pica::FramebufferRegs::LogicOp::Copy);
        fb.alphablend_enable.Assign(pick({0, 1}));

        auto& tex = config.texture;
        tex.texture0_type.Assign(static_cast<Pica::TexturingRegs::TextureConfig::TextureType>(
            pick({0, 0, 0, 3, 5})));
        tex.texture2_use_coord1.Assign(pick({0, 1}));
        tex.combiner_buffer_input.Assign(rng() & 0xFF);
        tex.fog_mode.Assign(static_cast<Pica::TexturingRegs::FogMode>(pick({0, 0, 5})));
        tex.fog_flip.Assign(pick({0, 1}));
        for (std::size_t i = 0; i < 6; i++) {
            auto& stage = tex.tev_stages[i];
            if (chance(30)) {
                // (the pass-through pattern: IsPassThroughTevStage)
                stage.sources_raw = 0xF | (0xF << 16) | ((rng() & 0xFF) << 4) | ((rng() & 0xFF) << 20);
                stage.modifiers_raw = 0;
                stage.ops_raw = 0;
                stage.scales_raw = chance(80) ? 0 : (rng() & 3) | ((rng() & 3) << 16);
                continue;
            }
            const auto source = [&] { return pick({0, 1, 2, 3, 4, 5, 6, 13, 14, 15, 15}); };
            const auto color_modifier = [&] { return pick({0, 1, 2, 3, 4, 5, 8, 9, 12, 13}); };
            stage.sources_raw = source() | (source() << 4) | (source() << 8) | (source() << 16) |
                                (source() << 20) | (source() << 24);
            stage.modifiers_raw = color_modifier() | (color_modifier() << 4) |
                                  (color_modifier() << 8) | ((rng() & 7) << 12) |
                                  ((rng() & 7) << 16) | ((rng() & 7) << 20);
            stage.ops_raw = pick({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) |
                            (pick({0, 1, 2, 3, 4, 5, 8, 9}) << 16);
            stage.scales_raw = (rng() & 3) | ((rng() & 3) << 16);
        }

        auto& lighting = config.lighting;
        if (chance(65)) {
            lighting.enable.Assign(1);
            lighting.src_num.Assign(pick({1, 1, 2, 2, 3, 4, 8}));
            lighting.bump_mode.Assign(
                static_cast<Pica::LightingRegs::LightingBumpMode>(pick({0, 0, 1, 2})));
            lighting.bump_selector.Assign(pick({0, 1, 2, 3}));
            lighting.bump_renorm.Assign(pick({0, 1}));
            lighting.clamp_highlights.Assign(pick({0, 1}));
            lighting.config.Assign(
                static_cast<Pica::LightingRegs::LightingConfig>(pick({0, 1, 2, 3, 4, 5, 6, 8})));
            lighting.enable_primary_alpha.Assign(pick({0, 1}));
            lighting.enable_secondary_alpha.Assign(pick({0, 1}));
            lighting.enable_shadow.Assign(pick({0, 1}));
            lighting.shadow_primary.Assign(pick({0, 1}));
            lighting.shadow_secondary.Assign(pick({0, 1}));
            lighting.shadow_invert.Assign(pick({0, 1}));
            lighting.shadow_alpha.Assign(pick({0, 1}));
            lighting.shadow_selector.Assign(pick({0, 1, 2, 3}));
            for (LutConfig* lut : {&lighting.lut_d0, &lighting.lut_d1, &lighting.lut_sp,
                                   &lighting.lut_fr, &lighting.lut_rr, &lighting.lut_rg,
                                   &lighting.lut_rb}) {
                lut->enable.Assign(pick({0, 1, 1}));
                lut->abs_input.Assign(pick({0, 1}));
                lut->type.Assign(
                    static_cast<Pica::LightingRegs::LightingLutInput>(pick({0, 1, 2, 3, 4, 5})));
                static constexpr float scales[] = {1.0f, 2.0f, 4.0f, 8.0f, 0.25f, 0.5f};
                lut->SetScale(scales[rng() % 6]);
            }
            lighting.lut_sp.enable.Assign(1);
            for (auto& light : lighting.lights) {
                light.raw = 0;
                light.num.Assign(rng() & 7);
                light.directional.Assign(pick({0, 1}));
                light.two_sided_diffuse.Assign(pick({0, 1}));
                light.dist_atten_enable.Assign(pick({0, 1}));
                light.spot_atten_enable.Assign(pick({0, 1}));
                light.geometric_factor_0.Assign(pick({0, 1}));
                light.geometric_factor_1.Assign(pick({0, 1}));
                light.shadow_enable.Assign(pick({0, 1}));
            }
        }

        const UserConfig user{};
        const Profile profile = MakeProfile(false);
        Generator::FSUniformData data{};
        Generator::GLSL::FillGenericFragmentConfig(config, data);
        std::printf("//SUPPORTED %d\n", Generator::GLSL::GenericFragmentSupports(config, user));
        std::printf("//CFG");
        for (const auto& word : data.hen_cfg) {
            std::printf(" %d %d %d %d", word.x, word.y, word.z, word.w);
        }
        std::printf("\n//SCALE");
        for (const auto& scale : data.hen_lut_scale) {
            std::printf(" %.9g %.9g %.9g %.9g", scale.x, scale.y, scale.z, scale.w);
        }
        std::printf("\n");
        // (gen case SEED canon: the shader for the configuration made canonical instead,
        // FSConfig::Canonicalize: it must draw the same as the one for the configuration as is)
        if (argc >= 4 && std::strcmp(argv[3], "canon") == 0) {
            config.Canonicalize(profile);
        }
        const std::string code = Generator::GLSL::GenerateFragmentShader(config, user, profile);
        std::fwrite(code.data(), 1, code.size(), stdout);
        return 0;
    }
    std::fprintf(stderr, "usage: gen generic 0|1 | gen case SEED\n");
    return 2;
}
