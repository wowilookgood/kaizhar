# fsdiff: the generic fragment shader against the generated ones, on the host

The generic fragment shader (`GenerateGenericFragmentShader` in
`src/video_core/shader/generator/glsl_fs_shader_gen.cpp`) must give the same colour, depth and
discards as the shader `FragmentModule` generates for the same configuration. These tools
check that on the computer, before a build goes to the console.

- `build.sh`: compiles the generator for the host (`gen`); `stubs.cpp` is all it needs from
  the rest of the emulator.
- `gen generic 0|1`: the generic shader's text (1: the barycentric variant). Compile it with
  a host glslang (built from `externals/glslang`, target `glslang-standalone`):
  `(echo '#version 450 core'; ./gen generic 1) > g.frag; glslang -V -S frag g.frag`.
- `gen case SEED`: a random valid configuration, its generated shader, and the configuration
  as the generic shader reads it.
- `difftest.py FIRST CASES SAMPLES`: turns both shaders into C++ (`glsl.h`), runs them on
  random pixels with random uniforms and compares. `MUTATE='old=>new'` plants a fault in
  the generic shader to see that the test notices.

2026-11-13: 2000 configurations, 288671 compared colours, no difference; of twelve planted
faults eleven were reported and the twelfth (fresnel taken from every light and not only the
last) is no fault: the last light's value wins either way.

Not covered: the barycentric variant's quaternion interpolation, the SPIR-V generator
(`spv_fs_shader_gen.cpp`, the default on the console: assumed equal to the GLSL one), what a
driver does with the shader, and texture and table contents (hashes here).
A new shader is compiled and tested here before it is sent to the console.

`CANON=1 difftest.py ...` compares, instead, the generated shader for a configuration made
canonical (`FSConfig::Canonicalize`, default on since 2026-11-13) with the one for the
configuration as it is: 600 configurations, 87478 compared colours, no difference
(2026-11-13). The random configurations vary the texture stages, lighting, alpha test, fog,
scissor and depth; the blend, logic-op and wrap fields that Canonicalize also clears are
left at zero here (nothing reads them on the console's profile).

`build.sh` leaves `gen`, object files and `work/` next to the sources: delete them after use.
