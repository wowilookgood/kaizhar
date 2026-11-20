# Kaizhar: the Switch 2 homebrew build of Azahar. Included from the top-level CMakeLists.txt
# when NINTENDO_SWITCH is set (the devkit's toolchain file sets it); hen/tools/configure.sh
# runs CMake with the options this build was tested with.
#
# The platform shims (sys/mman.h and dlfcn.h stubs, POSIX fixes for the SD card, thread
# cores and stacks) are in hen/platform.
get_filename_component(_hen_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(HEN_PLATFORM_DIR "${_hen_dir}/platform" CACHE PATH
    "hen_platform sources")
if(NOT EXISTS "${HEN_PLATFORM_DIR}/hen_thread.c")
    message(FATAL_ERROR "hen_platform not found in ${HEN_PLATFORM_DIR}")
endif()
include_directories(BEFORE SYSTEM "${HEN_PLATFORM_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/../compat")

# KAIZHAR_TLS: HTTPS. LibreSSL (externals/libressl) is built for the console
# (src/citra_switch/tls/libressl_switch.cmake); the public room list, the emulated console's
# HTTPS requests (http:C) and with them the system setup from a real console need it. OFF: no
# TLS library; HTTPS requests fail.
option(KAIZHAR_TLS "Build LibreSSL: HTTPS for the room list and the emulated console" ON)

# HEN_DEV: the test hooks and the default logging of the development builds (unattended test
# runs, movies, screenshots to the log, the bench programs, traces, the once-a-second
# samples). OFF is the release build: none of them are compiled in.
option(HEN_DEV "Build the development test hooks and logging" ON)
if(HEN_DEV)
    add_compile_definitions(HEN_DEV=1)
else()
    add_compile_definitions(HEN_DEV=0)
endif()

# No absolute path of the build machine in the program (__FILE__ in assertions and log
# lines, debug information): the source tree becomes ".", the build folder "build" and the
# devkit "devkitpro".
add_compile_options(
    "-ffile-prefix-map=${PROJECT_SOURCE_DIR}=."
    "-ffile-prefix-map=${PROJECT_BINARY_DIR}=build")
if(DEFINED ENV{DEVKITPRO})
    get_filename_component(_hen_devkit "$ENV{DEVKITPRO}" REALPATH)
    add_compile_options("-ffile-prefix-map=${_hen_devkit}=devkitpro")
    if(NOT _hen_devkit STREQUAL "$ENV{DEVKITPRO}")
        add_compile_options("-ffile-prefix-map=$ENV{DEVKITPRO}=devkitpro")
    endif()
endif()

# dynarmic: code memory is writable or executable, never both (hen_jit.c).
set(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT ON CACHE BOOL "" FORCE)
