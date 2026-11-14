# Kaizhar, the Switch 2 homebrew port of Azahar: the frontend and the .nro (kaizhar.elf, kaizhar.nro).
add_executable(kaizhar
    emu_window_switch.cpp
    emu_window_switch.h
    input_switch.cpp
    input_switch.h
    main.cpp
    switch_audio.c
    switch_menu.c
    switch_mic.c
    switch_menu.h
    title_menu.cpp
    title_menu.h
    switch_file_defer.c
    switch_platform.c
    switch_platform.h
    ${PROJECT_SOURCE_DIR}/hen/jit/hen_jit.c
    ${PROJECT_SOURCE_DIR}/hen/compat/hen_libc.c
    ${PROJECT_SOURCE_DIR}/hen/compat/hen_rand.c
    ${PROJECT_SOURCE_DIR}/hen/compat/hen_uio.c
    # hen/platform: SD card POSIX fixes, thread cores and stacks, mmap/dlfcn stubs.
    ${HEN_PLATFORM_DIR}/hen_fs.c
    ${HEN_PLATFORM_DIR}/hen_posix.c
    ${HEN_PLATFORM_DIR}/hen_thread.c
)
# Development builds (HEN_DEV, hen/cmake/Kaizhar.cmake): --crumbs and --vk-trace.
if(HEN_DEV)
    target_sources(kaizhar PRIVATE
        ${PROJECT_SOURCE_DIR}/hen/vktrace/hen_crumb.c
        ${PROJECT_SOURCE_DIR}/hen/vktrace/hen_vktrace.c
        ${PROJECT_SOURCE_DIR}/hen/vktrace/hen_vktrace_thunks.S)
endif()
# The chained test launcher (--director PORT) is hen-chain, a library that is not part of this
# repository. Optional: used only with HEN_DEV and only when HEN_CHAIN_DIR names its folder
# (built first: make -C <HEN_CHAIN_DIR>). Unset, the program builds without any chain code.
set(HEN_CHAIN_DIR "" CACHE PATH "hen-chain (chained test launcher, development builds); empty: not built")
if(HEN_DEV AND HEN_CHAIN_DIR AND EXISTS "${HEN_CHAIN_DIR}/include/hen_chain.h")
    if(NOT EXISTS "${HEN_CHAIN_DIR}/lib/libhen_chain.a")
        message(FATAL_ERROR "hen-chain is not built: run  make -C ${HEN_CHAIN_DIR}  (with DEVKITPRO set)")
    endif()
    target_compile_definitions(kaizhar PRIVATE HEN_CHAIN=1)
    target_include_directories(kaizhar PRIVATE ${HEN_CHAIN_DIR}/include)
    target_link_libraries(kaizhar PRIVATE ${HEN_CHAIN_DIR}/lib/libhen_chain.a)
    message(STATUS "hen: chained test launcher (hen-chain) ON")
else()
    target_compile_definitions(kaizhar PRIVATE HEN_CHAIN=0)
    message(STATUS "hen: chained test launcher (hen-chain) OFF")
endif()
# The allocator is the one libounce links (mimalloc): nothing of ours.

target_link_libraries(kaizhar PRIVATE citra_common citra_core video_core audio_core input_common network fmt vulkan-headers)
# The HEN's hooks for libounce's Jit API (kaihen-jit.o; devkits before 20261113f have the same
# code as ouncehen-jit.o), and the Vulkan driver.
set(HEN_JIT_HOOKS $ENV{DEVKITPRO}/portlibs/switch2/lib/kaihen-jit.o)
if(NOT EXISTS ${HEN_JIT_HOOKS})
    set(HEN_JIT_HOOKS $ENV{DEVKITPRO}/portlibs/switch2/lib/ouncehen-jit.o)
endif()
if(NOT EXISTS ${HEN_JIT_HOOKS})
    message(FATAL_ERROR "neither kaihen-jit.o nor ouncehen-jit.o in $ENV{DEVKITPRO}/portlibs/switch2/lib")
endif()
target_link_libraries(kaizhar PRIVATE
    ${HEN_JIT_HOOKS}
    -Wl,--start-group ouncevk -Wl,--end-group)
target_link_options(kaizhar PRIVATE -Wl,--wrap=__syscall_thread_create -Wl,--wrap=rename
    -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=fread -Wl,--wrap=fclose
    -Wl,--wrap=fopen -Wl,--wrap=fseek -Wl,--wrap=ftell
    -Wl,--wrap=__syscall_cond_signal -Wl,--wrap=__syscall_cond_wait)

# The name, author and version the homebrew menu shows (the version field holds 15 characters).
nx_generate_nacp(kaizhar.nacp NAME "Kaizhar" AUTHOR "wowilookgood" VERSION "2126.2-kaizhar2")
# The icon: one variable, so that another picture can be given at configure time.
set(KAIZHAR_ICON "${PROJECT_SOURCE_DIR}/hen/icon/icon.jpg" CACHE FILEPATH "Icon of the .nro (JPEG, 256x256)")
# HEN_ROMFS_DIR: a folder of test programs (.3dsx) that a development build carries as romfs:/.
# Nothing is bundled by default, and never in a release build (HEN_DEV off). A file
# romfs:/hbmenu.3dsx is listed by the title menu as "3DS Homebrew Launcher".
set(HEN_ROMFS_DIR "" CACHE PATH "Folder bundled as romfs:/ in development builds; empty: none")
if (HEN_DEV AND HEN_ROMFS_DIR AND EXISTS "${HEN_ROMFS_DIR}")
    nx_create_nro(kaizhar NACP kaizhar.nacp ICON ${KAIZHAR_ICON} ROMFS ${HEN_ROMFS_DIR})
    message(STATUS "hen: romfs from ${HEN_ROMFS_DIR}")
else()
    nx_create_nro(kaizhar NACP kaizhar.nacp ICON ${KAIZHAR_ICON})
endif()
