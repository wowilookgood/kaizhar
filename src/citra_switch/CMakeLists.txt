# Kaizhar, the Switch 2 homebrew port of Azahar: the frontend and the .nro (kaizhar.elf, kaizhar.nro).
add_executable(kaizhar
    emu_window_switch.cpp
    emu_window_switch.h
    input_switch.cpp
    input_switch.h
    main.cpp
    guest_dns.cpp
    guest_dns.h
    link_3ds.cpp
    link_3ds.h
    online_menu.cpp
    online_menu.h
    room_link.cpp
    room_link.h
    room_list.cpp
    room_list.h
    cache_clear.cpp
    cache_clear.h
    session_report.cpp
    session_report.h
    switch_audio.c
    switch_menu.c
    switch_mic.c
    switch_menu.h
    tool_scan.cpp
    tool_scan.h
    title_menu.cpp
    title_menu.h
    switch_file_defer.c
    switch_platform.c
    switch_platform.h
    switch_room_io.c
    ${PROJECT_SOURCE_DIR}/hen/jit/hen_jit.c
    ${PROJECT_SOURCE_DIR}/hen/compat/hen_libc.c
    ${PROJECT_SOURCE_DIR}/hen/compat/hen_uio.c
    # hen/platform: SD card POSIX fixes, thread cores and stacks, mmap/dlfcn stubs.
    ${HEN_PLATFORM_DIR}/hen_fs.c
    ${HEN_PLATFORM_DIR}/hen_posix.c
    ${HEN_PLATFORM_DIR}/hen_thread.c
)
# HTTPS (KAIZHAR_TLS, hen/cmake/Kaizhar.cmake). With it, the certificate authorities that
# servers are checked against are built in (LibreSSL's list). Without it, RAND_bytes, the one
# OpenSSL function the core uses outside TLS, comes from hen/compat.
if(KAIZHAR_TLS)
    set(KAIZHAR_CA_BUNDLE "${PROJECT_SOURCE_DIR}/externals/libressl/cert.pem")
    target_sources(kaizhar PRIVATE tls/ca_bundle.cpp tls/ca_bundle.h tls/ca_bundle_data.S)
    # (--https-check, development builds)
    target_sources(kaizhar PRIVATE https_check.cpp https_check.h)
    target_compile_definitions(kaizhar PRIVATE KAIZHAR_TLS=1)
    set_source_files_properties(tls/ca_bundle_data.S PROPERTIES
        COMPILE_DEFINITIONS "KAIZHAR_CA_BUNDLE=\"${KAIZHAR_CA_BUNDLE}\""
        OBJECT_DEPENDS "${KAIZHAR_CA_BUNDLE}")
else()
    target_sources(kaizhar PRIVATE ${PROJECT_SOURCE_DIR}/hen/compat/hen_rand.c)
endif()
# Development builds (HEN_DEV, hen/cmake/Kaizhar.cmake): --crumbs and --vk-trace.
if(HEN_DEV)
    target_sources(kaizhar PRIVATE
        ${PROJECT_SOURCE_DIR}/hen/vktrace/hen_crumb.c
        ${PROJECT_SOURCE_DIR}/hen/vktrace/hen_vktrace.c
        ${PROJECT_SOURCE_DIR}/hen/vktrace/hen_vktrace_thunks.S)
endif()
# An optional test library that is not included in this repository. It is used only with
# HEN_DEV and only when HEN_CHAIN_DIR names its folder (already built). Unset, which is the
# default, HEN_CHAIN is 0 and the program builds without any of its code.
set(HEN_CHAIN_DIR "" CACHE PATH "an optional test library, not included (development builds); empty: not used")
if(HEN_DEV AND HEN_CHAIN_DIR AND EXISTS "${HEN_CHAIN_DIR}/include/hen_chain.h")
    if(NOT EXISTS "${HEN_CHAIN_DIR}/lib/libhen_chain.a")
        message(FATAL_ERROR "the test library in HEN_CHAIN_DIR is not built: no ${HEN_CHAIN_DIR}/lib/libhen_chain.a")
    endif()
    target_compile_definitions(kaizhar PRIVATE HEN_CHAIN=1)
    target_include_directories(kaizhar PRIVATE ${HEN_CHAIN_DIR}/include)
    target_link_libraries(kaizhar PRIVATE ${HEN_CHAIN_DIR}/lib/libhen_chain.a)
    message(STATUS "hen: optional test library ON")
else()
    target_compile_definitions(kaizhar PRIVATE HEN_CHAIN=0)
    message(STATUS "hen: optional test library OFF")
endif()
# The allocator is the one libounce links (mimalloc): nothing of ours.

# (enet: room_link.cpp resolves the room's address with it before the room client connects)
target_link_libraries(kaizhar PRIVATE citra_common citra_core video_core audio_core input_common network enet fmt vulkan-headers)
# (room_list.cpp: the lobby's answer is JSON over HTTP or HTTPS)
target_link_libraries(kaizhar PRIVATE httplib json-headers)
# The HEN's hooks for libounce's Jit API (kaihen-jit.o; older devkits have the same
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
    -Wl,--wrap=__syscall_cond_signal -Wl,--wrap=__syscall_cond_wait
    -Wl,--wrap=sendmsg -Wl,--wrap=recvmsg)
# Development builds count what is allocated, by size and for blocks of 1 MiB and more by
# who asked ("hen: heap:" lines, switch_platform.c): the allocation calls go through there.
# HEN_HEAP_ACCOUNT=OFF builds a development build without it (to measure what the counting itself
# costs): the calls are not wrapped and one "hen: heap: account not built" line is printed instead.
option(HEN_HEAP_ACCOUNT "Count the heap by size and by who asked (development builds)" OFF)
if(HEN_DEV AND HEN_HEAP_ACCOUNT)
    target_compile_definitions(kaizhar PRIVATE HEN_HEAP_ACCOUNT=1)
    target_link_options(kaizhar PRIVATE -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc
        -Wl,--wrap=realloc -Wl,--wrap=memalign -Wl,--wrap=aligned_alloc -Wl,--wrap=posix_memalign)
else()
    target_compile_definitions(kaizhar PRIVATE HEN_HEAP_ACCOUNT=0)
endif()

# The version: Azahar's, then -kaizhar and the number of the Kaizhar release on it. Set here only: the
# homebrew menu shows it (the name, author and version below; the version field holds 15
# characters), and so do the title list and the log (KAIZHAR_VERSION, switch_platform.h).
set(KAIZHAR_VERSION "2126.2-kaizhar3")
target_compile_definitions(kaizhar PRIVATE KAIZHAR_VERSION="${KAIZHAR_VERSION}")
nx_generate_nacp(kaizhar.nacp NAME "Kaizhar" AUTHOR "wowilookgood" VERSION "${KAIZHAR_VERSION}")
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
