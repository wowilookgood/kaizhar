#!/bin/bash
# Configures Kaizhar (Azahar for Switch 2 homebrew) with the options it is tested with.
#
#   DEVKITPRO=/path/to/devkit hen/tools/configure.sh [BUILD_DIR] [extra cmake arguments]
#
# DEVKITPRO   the Switch 2 devkit (it has libounce/cmake/Switch2.cmake). Required.
# BUILD_DIR   where to build; default: build, in the repository. Also read from the
#             environment (BUILD_DIR=...).
# Extra arguments go to cmake, for example:
#   -DHEN_DEV=ON              development build: test hooks and logging (default here: OFF)
#   -DKAIZHAR_ICON=file.jpg   another icon (256x256 JPEG)
#   -DHEN_ROMFS_DIR=folder    test programs for romfs:/ (development builds)
#   -DHEN_CHAIN_DIR=folder    an optional test library, not included (development builds)
#
# Before the first build, fetch the submodules and apply hen/patches (see the README):
#   git submodule update --init --recursive
#   git -C externals/dynarmic apply ../../hen/patches/dynarmic.patch
#   git -C externals/dynarmic/externals/mcl apply ../../../../hen/patches/mcl.patch
#   git -C externals/oaknut apply ../../hen/patches/oaknut.patch
#   git -C externals/cryptopp apply ../../hen/patches/cryptopp.patch
# Then: cmake --build BUILD_DIR --target kaizhar_nro
# The result is BUILD_DIR/src/citra_switch/kaizhar.nro.
set -e

if [ -z "$DEVKITPRO" ] || [ ! -f "$DEVKITPRO/libounce/cmake/Switch2.cmake" ]; then
    echo "Set DEVKITPRO to the Switch 2 devkit (no libounce/cmake/Switch2.cmake in '$DEVKITPRO')" >&2
    exit 1
fi
export DEVKITPRO

SOURCE_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
case "$1" in
    ""|-*) ;;
    *) BUILD_DIR="$1"; shift ;;
esac
BUILD_DIR="${BUILD_DIR:-$SOURCE_DIR/build}"

# The CPU flags the measurements in the README were taken with: devkitPro's Switch flags.
# The devkit's own default is -mcpu=cortex-a78c+crypto; KAIZHAR_CPU_FLAGS chooses others.
# _DEFAULT_SOURCE: newlib hides POSIX and BSD declarations in strict modes like -std=c++20.
CPU="${KAIZHAR_CPU_FLAGS:--march=armv8-a+crc+crypto -mtune=cortex-a57}"
BASE="$CPU -mtp=soft -ftls-model=local-exec -ffunction-sections -fdata-sections -D__SWITCH__"

exec cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/libounce/cmake/Switch2.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$BASE -D__SWITCH2__ -D_DEFAULT_SOURCE" \
    -DCMAKE_CXX_FLAGS="$BASE -D__SWITCH2__ -D_DEFAULT_SOURCE" \
    -DCMAKE_ASM_FLAGS="$BASE -x assembler-with-cpp -D__SWITCH2__" \
    -DHEN_DEV=OFF \
    -DENABLE_QT=OFF -DENABLE_QT_TRANSLATION=OFF -DENABLE_TESTS=OFF \
    -DENABLE_ROOM=OFF -DENABLE_ROOM_STANDALONE=OFF -DENABLE_WEB_SERVICE=OFF \
    -DENABLE_SCRIPTING=OFF -DENABLE_GDBSTUB=OFF -DENABLE_CUBEB=OFF \
    -DENABLE_OPENAL=OFF -DENABLE_LIBUSB=OFF -DENABLE_OPENGL=OFF \
    -DENABLE_VULKAN=ON -DENABLE_SOFTWARE_RENDERER=ON -DENABLE_SDL2=OFF \
    -DENABLE_LTO=OFF -DCITRA_USE_PRECOMPILED_HEADERS=OFF \
    -DCITRA_WARNINGS_AS_ERRORS=OFF -DENABLE_DISCORD_RPC=OFF \
    "$@"
