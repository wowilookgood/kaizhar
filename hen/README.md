# hen/

What the Switch 2 port needs besides `src/citra_switch`. Files here without a header of their own are Copyright 2026 wowilookgood, GPLv2 or any later version (`license.txt`).

- `cmake/`: `Kaizhar.cmake`, included by the top-level `CMakeLists.txt` for Switch builds.
- `compat/`: the few libc, socket and OpenSSL declarations the core expects and the devkit lacks.
- `icon/`: the .nro's icon (`icon.jpg`, 256x256) and its 1024 px master. Original artwork of this port.
- `jit/`: `hen_jit.c`, code memory for dynarmic and the shader JIT, and the port's switches.
- `patches/`: the changes to the dynarmic, mcl, oaknut and cryptopp submodules, as `git apply` patches against the pinned commits.
- `platform/`: POSIX fixes for the SD card, thread cores and stacks, `sys/mman.h` and `dlfcn.h` stubs.
- `tools/`: `configure.sh` (runs CMake), `gen-menu-font.py`, readers for the logs of development builds, and `fsdiff/`, a host test of the generic fragment shader.
- `vktrace/`: Vulkan call tracing for development builds (`-DHEN_DEV=ON`).
