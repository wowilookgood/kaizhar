#!/usr/bin/env python3
"""difftest.py [FIRST_SEED [CASES [SAMPLES]]]: the generic fragment shader against the generated ones.

For each seed, ./gen makes a random valid fragment configuration and prints the shader the
emulator generates for it and the configuration as the generic shader reads it. Both shaders
are turned into C++ (glsl.h), compiled, and run on SAMPLES random pixels with random uniforms;
the outputs (discarded or not, colour, depth) must agree. Textures and lookup tables are
hashes, the same for both. Cases the generic shader says it does not support are skipped.
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
# CANON=1: instead of the generic shader, the generated shader for the configuration made
# canonical (FSConfig::Canonicalize) against the one for the configuration as it is.
CANON = bool(os.environ.get('CANON'))
BATCH = 20

PRELUDE = r'''
#include <cstdio>
#include <cstdlib>
#include <random>
#include "glsl.h"
#define NUM_TEV_STAGES 6
#define NUM_LIGHTS 8
#define NUM_LIGHTING_SAMPLERS 24
#define discard { g_discard = true; return; }
struct LightSrc {
    vec3 specular_0, specular_1, diffuse, ambient, position, spot_direction;
    float dist_atten_bias, dist_atten_scale;
};
// inputs, outputs
vec4 primary_color; vec2 texcoord0, texcoord1, texcoord2; float texcoord0_w; vec4 normquat; vec3 view;
vec4 color; float gl_FragDepth; vec4 gl_FragCoord; bool g_discard;
// the uniform block
int framebuffer_scale, alphatest_ref; float depth_scale, depth_offset, shadow_bias_constant, shadow_bias_linear;
int scissor_x1, scissor_y1, scissor_x2, scissor_y2, fog_lut_offset, proctex_noise_lut_offset,
    proctex_color_map_offset, proctex_alpha_map_offset, proctex_lut_offset, proctex_diff_lut_offset;
float proctex_bias; int shadow_texture_bias;
ivec4 lighting_lut_offset[NUM_LIGHTING_SAMPLERS / 4];
vec3 fog_color; vec2 proctex_noise_f, proctex_noise_a, proctex_noise_p; vec3 lighting_global_ambient;
LightSrc light_src[NUM_LIGHTS]; vec4 const_color[NUM_TEV_STAGES]; vec4 tev_combiner_buffer_color;
vec3 tex_lod_bias; vec4 tex_border_color[3]; vec4 blend_color;
ivec4 hen_cfg[11]; vec4 hen_lut_scale[2];
samplerBuffer texture_buffer_lut_lf{101}, texture_buffer_lut_rg{102}, texture_buffer_lut_rgba{103};
sampler2D tex0{1}, tex1{2}, tex2{3};

static std::mt19937 rng;
static float uni(float lo, float hi) { return lo + (hi - lo) * (rng() & 0xFFFFFF) / float(0xFFFFFF); }
static vec3 v3(float lo, float hi) { return {uni(lo, hi), uni(lo, hi), uni(lo, hi)}; }
static vec4 v4(float lo, float hi) { return {uni(lo, hi), uni(lo, hi), uni(lo, hi), uni(lo, hi)}; }
static void randomize() {
    primary_color = v4(0, 1);
    texcoord0 = {uni(-2, 2), uni(-2, 2)}; texcoord1 = {uni(-2, 2), uni(-2, 2)}; texcoord2 = {uni(-2, 2), uni(-2, 2)};
    texcoord0_w = uni(0.5f, 2);
    normquat = v4(-1, 1); view = v3(-1, 1) + vec3(0.01f, 0.02f, 0.03f);
    gl_FragCoord = {uni(0, 400), uni(0, 240), uni(0, 1), uni(0.2f, 2)};
    alphatest_ref = rng() % 256; depth_scale = uni(-1, 1); depth_offset = uni(-1, 1);
    scissor_x1 = rng() % 200; scissor_y1 = rng() % 120; scissor_x2 = scissor_x1 + rng() % 300; scissor_y2 = scissor_y1 + rng() % 200;
    fog_lut_offset = rng() % 1000;
    for (auto& o : lighting_lut_offset) o = {int(rng() % 4000), int(rng() % 4000), int(rng() % 4000), int(rng() % 4000)};
    fog_color = v3(0, 1); lighting_global_ambient = v3(0, 0.4f);
    for (auto& l : light_src) {
        l.specular_0 = v3(0, 1); l.specular_1 = v3(0, 1); l.diffuse = v3(0, 1); l.ambient = v3(0, 0.3f);
        l.position = v3(-3, 3) + vec3(0.05f, 0.06f, 0.07f); l.spot_direction = normalize(v3(-1, 1) + vec3(0.01f, 0.0f, 0.02f));
        l.dist_atten_bias = uni(-0.5f, 0.5f); l.dist_atten_scale = uni(0, 0.5f);
    }
    for (auto& c : const_color) c = v4(0, 1);
    tev_combiner_buffer_color = v4(0, 1); tex_lod_bias = v3(-1, 1);
}
struct Result { bool discarded; vec4 color; float depth; };
static Result run(void (*shader)()) {
    color = vec4(0.0f); gl_FragDepth = 0; g_discard = false;
    shader();
    return {g_discard, color, gl_FragDepth};
}
'''


def to_cpp(text, namespace):
    lines = text.split('\n')
    out, skipping = [], False
    for line in lines:
        s = line.strip()
        if s.startswith('#define NUM_TEV_STAGES'):
            skipping = True  # (to the end of the uniform block: the prelude declares all of it)
        if skipping:
            if s == '};' and 'blend_color' in ''.join(out_skipped[-4:]):
                skipping = False
            out_skipped.append(s)
            continue
        if s.startswith('#extension') or s.startswith('//') or re.match(r'layout\s*\(', s):
            continue
        out.append(line)
    body = '\n'.join(out)
    body = body.replace('void main()', 'void shader_main()')
    body = re.sub(r'\.(rgb|aaa|rrr|ggg|bbb|rg|xyz|xy|yy)\b(?!\s*\()', r'.\1()', body)
    # (GLSL's literals are floats: 1.0 and 8. become 1.0f and 8.f)
    body = re.sub(r'(?<![\w.])(\d+\.\d*|\.\d+)(?![\w.])', r'\1f', body)
    return f'namespace {namespace} {{\n{body}\n}}\n'


out_skipped = []


def main():
    first = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    cases = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    samples = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    generic = subprocess.run([f'{HERE}/gen', 'generic', '0'], capture_output=True, text=True, check=True).stdout
    # MUTATE='old=>new': a deliberate fault in the generic shader, to see that the test notices.
    if os.environ.get('MUTATE'):
        old, new = os.environ['MUTATE'].split('=>')
        assert old in generic, 'the text to mutate is not in the generic shader'
        generic = generic.replace(old, new)
    work = os.path.join(HERE, 'work'); os.makedirs(work, exist_ok=True)
    totals = {'cases': 0, 'unsupported': 0, 'bad cases': 0}
    for start in range(first, first + cases, BATCH):
        seeds = list(range(start, min(start + BATCH, first + cases)))
        src = [PRELUDE, to_cpp(generic, 'gen')]
        used = []
        for seed in seeds:
            text = subprocess.run([f'{HERE}/gen', 'case', str(seed)], capture_output=True, text=True, check=True).stdout
            head, rest = text.split('\n', 3)[:3], text.split('\n', 3)[3]
            if head[0].strip() != '//SUPPORTED 1':
                totals['unsupported'] += 1
                continue
            cfg = head[1].split()[1:]; scale = head[2].split()[1:]
            src.append(to_cpp(rest, f'c{seed}'))
            if CANON:
                canon = subprocess.run([f'{HERE}/gen', 'case', str(seed), 'canon'], capture_output=True, text=True, check=True).stdout
                src.append(to_cpp(canon.split('\n', 3)[3], f'k{seed}'))
            used.append((seed, cfg, scale))
        driver = ['int main() {', '    int bad_cases = 0; long total_kept = 0;']
        for seed, cfg, scale in used:
            words = ', '.join('{' + ', '.join(cfg[i:i + 4]) + '}' for i in range(0, 44, 4))
            scales = ', '.join('{' + ', '.join(f'{float(x)!r}f' for x in scale[i:i + 4]) + '}' for i in range(0, 8, 4))
            driver.append(f'''    {{
        const ivec4 words[11] = {{{words}}}; const vec4 scales[2] = {{{scales}}};
        for (int i = 0; i < 11; i++) hen_cfg[i] = words[i];
        hen_lut_scale[0] = scales[0]; hen_lut_scale[1] = scales[1];
        rng.seed({seed}); int bad = 0, kept = 0; float worst = 0;
        for (int s = 0; s < {samples}; s++) {{
            randomize();
            const Result a = run(c{seed}::shader_main), b = run({'k' + str(seed) if CANON else 'gen'}::shader_main);
            if (a.discarded != b.discarded) {{ bad++; continue; }}
            if (a.discarded) continue;
            kept++;
            float d = 0;
            for (int k = 0; k < 4; k++) d = max(d, abs(const_cast<vec4&>(a.color)[k] - const_cast<vec4&>(b.color)[k]));
            const float dd = abs(a.depth - b.depth);
            if (!(d <= 0.5f / 255.0f) || !(dd <= 1e-4f * (1.0f + abs(a.depth)))) bad++;
            if (d > worst) worst = d;
        }}
        if (bad) {{ bad_cases++; std::printf("seed {seed}: %d of {samples} samples differ (%d compared colours, worst %.4f)\\n", bad, kept, worst); }}
        total_kept += kept;
    }}''')
        driver += ['    std::fprintf(stderr, "%ld\\n", total_kept);', '    return bad_cases;', '}']
        path = os.path.join(work, f'batch{start}.cpp')
        open(path, 'w').write('\n'.join(src + driver))
        exe = path[:-4]
        c = subprocess.run(['clang++', '-std=c++20', '-O1', '-w', '-I', HERE, path, '-o', exe], capture_output=True, text=True)
        if c.returncode != 0:
            print(f'batch {start}: does not compile:\n' + '\n'.join(c.stderr.split('\n')[:25]))
            sys.exit(1)
        r = subprocess.run([exe], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        totals['cases'] += len(used); totals['bad cases'] += r.returncode
        totals['colours compared'] = totals.get('colours compared', 0) + int(r.stderr.strip() or 0)
        sys.stdout.flush()
    print(totals)


if __name__ == '__main__':
    main()
