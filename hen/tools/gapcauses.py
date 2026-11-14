#!/usr/bin/env python3
"""gapcauses.py [--detail TEXT] ELF LOG...: what the emulator was doing during gaps between frames shown.

For every "hen: slow: N ms between two frames shown" the log carries two sampler lines (one
sample every 10 ms): the emulation thread and the GPU thread. Each sample's call chain is
put in a class by the first rule that matches any of its frames; the output is 10 ms units
per class, per thread, over all gaps of the logs given.
"""
import collections, os, re, subprocess, sys

RULES = [  # (class, pattern over the symbolised call chain)
    ('limiter sleep (the emulator is waiting for real time)', r'FrameLimiter::DoFrameLimiting'),
    ('waiting for the GPU thread', r'HenGpu::(Poll|Wait|Sync|Flush)'),
    ('driver: compiling a shader', r'nak_|nir_|nvk_compile_shaders|vk_compile_shaders|spirv_to_nir|vtn_'),
    ('driver: creating a pipeline (other)', r'CreateGraphicsPipelines|vk_create_graphics_pipeline|vk_graphics_pipeline'),
    ('driver: waiting for the GPU / memory', r'native_ctx_|native_gsv_|nvkmd_|native_owner|native_chunked|nvk_queue|vk_queue|QueueSubmit|WaitForFences|vmaCreate|BindImageMemory'),
    ('our shader generation (GLSL text)', r'Generator::GLSL|GLSLGenerator|ShaderWriter|GenerateVertexShader|GenerateFragmentShader'),
    ('glslang (GLSL to SPIR-V)', r'glslang|CompileGLSL|TGlslang|spv::'),
    ('texture decode / upload', r'MortonCopy|UploadTexture|RasterizerCache.*Upload|DecodeTexture|ETC1|ConvertABGR|texture_codec'),
    ('texture cache lookups', r'RasterizerCache|GetSurface|GetTexture|SurfaceBase|icl::'),
    ('vertex loading / CPU shading', r'LoadVertices|ShaderEngine|JitShader|PicaCore::Draw|AnalyzeVertex'),
    ('stream buffer / copies', r'memcpy|StreamBuffer|vk_stream_buffer'),
    ('scheduler worker wait', r'Scheduler::WaitWorker|Scheduler::Flush|Scheduler::Finish'),
    ('file I/O', r'fwrite|fread|fflush|fclose|fopen|IOFile|fsFile|fsdev'),
    ('generated code (JIT)', r'^J\d'),
    ('JIT compile / lookup', r'Dynarmic|AddressSpace|EmitArm64|oaknut|HenInterpret|InterpreterMainLoop'),
    ('condition wait (idle worker)', r'cond_wait|condvarWait'),
]


def main():
    detail = None  # --detail TEXT: the most common call chains of the classes whose name has TEXT
    if '--detail' in sys.argv:
        i = sys.argv.index('--detail'); detail = sys.argv[i + 1]; del sys.argv[i:i + 2]
    elf, logs = sys.argv[1], sys.argv[2:]
    detailed = collections.Counter()
    tool = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line'
    gaps, samples = [], []  # samples: (thread, token)
    for log in logs:
        t = open(log, 'rb').read().decode('latin-1').replace('\r', '')
        for m in re.finditer(r'hen: slow: (\d+) ms between two frames shown\n(?:.*\n){0,2}?hen: bb: the last \d+ ms:(.*)\n'
                             r'(?:hen: bb: the renderer.s thread in those \d+ ms:(.*)\n)?', t):
            gaps.append(int(m[1]))
            samples += [('emulation thread', x) for x in m[2].split()]
            samples += [('GPU thread', x) for x in (m[3] or '').split()]
    addrs = sorted({a for _, tok in samples if tok.startswith('+') for a in tok[1:].split('/')})
    names = {}
    for i in range(0, len(addrs), 400):
        chunk = addrs[i:i + 400]
        out = subprocess.run([tool, '-f', '-C', '-e', elf] + ['0x' + a for a in chunk], capture_output=True, text=True).stdout.split('\n')
        names.update({a: out[2 * j] for j, a in enumerate(chunk)})
    count = collections.defaultdict(collections.Counter)
    unknown = collections.Counter()
    for thread, tok in samples:
        if tok == '-':
            continue
        chain = tok if not tok.startswith('+') else ' < '.join(names.get(a, '?') for a in tok[1:].split('/'))
        for cls, rx in RULES:
            if re.search(rx, chain):
                count[thread][cls] += 1
                if detail and detail in cls:
                    detailed[thread + ': ' + ' < '.join(x[:60] for x in chain.split(' < ')[:10])] += 1
                break
        else:
            count[thread]['other'] += 1
            unknown[chain[:150]] += 1
    print(f'{len(gaps)} gaps over 70 ms in {len(logs)} log(s): {sum(gaps)} ms in all, longest {max(gaps, default=0)} ms')
    for thread in ('GPU thread', 'emulation thread'):
        total = sum(count[thread].values())
        print(f'-- {thread}: {total * 10} ms sampled')
        for cls, n in count[thread].most_common():
            print(f'   {n * 10:5d} ms  {100 * n // max(total, 1):3d}%  {cls}')
    if detail:
        print(f'-- most common chains in classes with "{detail}":')
        for chain, n in detailed.most_common(14):
            print(f'   {n * 10:5d} ms  {chain}')
    if unknown:
        print('-- most common "other" chains:')
        for chain, n in unknown.most_common(6):
            print(f'   {n:3d}  {chain}')


if __name__ == '__main__':
    main()
