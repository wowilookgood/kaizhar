#!/bin/bash
# Builds the generic fragment shader's generator for this computer and validates its output.
cd "$(dirname "$0")"
A=$(cd ../../.. && pwd)  # the repository
INC="-std=c++23 -DFMT_HEADER_ONLY -I $A/src -I $A/externals/fmt/include -I $A/externals/boost -I $A/externals/microprofile -I $A/externals/xxHash"
clang++ $INC -c $A/src/video_core/shader/generator/glsl_fs_shader_gen.cpp -o gen.o 2>&1 | grep "error" | head -12
clang++ $INC -c main.cpp -o main.o 2>&1 | grep error | head
clang++ $INC -c stubs.cpp -o stubs.o 2>&1 | grep error | head
clang++ $INC -c $A/src/video_core/shader/generator/pica_fs_config.cpp -o cfg.o 2>&1 | grep error | head
clang++ gen.o main.o stubs.o cfg.o -o gen 2>&1 | grep -A2 "Undefined\|\"_" | head -40
