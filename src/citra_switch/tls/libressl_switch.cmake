# LibreSSL (externals/libressl) for the Switch 2 build. Included from externals/CMakeLists.txt
# in place of the plain add_subdirectory the other platforms use; sets OPENSSL_LIBRARIES.
#
# What the console's C library lacks and LibreSSL's own configuration can't find out:
# - The random numbers come from the C library's arc4random, which the port already feeds from
#   the system's generator (hen/compat/hen_libc.c, _getentropy_r). LibreSSL's own arc4random has
#   no hooks for this platform, and its link tests can't see ours.
# - LibreSSL takes its locks (pthread mutexes) and socket helpers from files it builds for
#   UNIX only, a variable this platform doesn't set: they are added below. The two other UNIX
#   files aren't: the system log BIO, and the passphrase prompt (termios), which tls_port.c
#   replaces with LibreSSL's null prompt.
# - syslog.h, getuid, sysconf: tls/include and tls_port.c.
# - asprintf and SSIZE_MAX are behind _GNU_SOURCE in newlib.
set(KAIZHAR_TLS_DIR ${CMAKE_CURRENT_LIST_DIR})

set(HAVE_ARC4RANDOM_BUF 1 CACHE INTERNAL "")
set(HAVE_ARC4RANDOM_UNIFORM 1 CACHE INTERNAL "")
set(HAVE_GETENTROPY 1 CACHE INTERNAL "")
set(ENABLE_ASM OFF CACHE BOOL "" FORCE)
set(LIBRESSL_SKIP_INSTALL ON CACHE BOOL "")
# The certificate store is a file the frontend names (room_list.cpp); nothing is read from here.
set(OPENSSLDIR "/switch/Kaizhar/ssl")

add_subdirectory(${KAIZHAR_LIBRESSL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libressl EXCLUDE_FROM_ALL)

target_sources(crypto_obj PRIVATE
    ${KAIZHAR_LIBRESSL_DIR}/crypto/crypto_lock.c
    ${KAIZHAR_LIBRESSL_DIR}/crypto/bio/b_posix.c)

foreach(target crypto_obj ssl_obj crypto ssl)
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE HAVE_INET_NTOP)
    target_include_directories(${target} PRIVATE ${KAIZHAR_TLS_DIR}/include)
    target_compile_options(${target} PRIVATE -w -include ${KAIZHAR_TLS_DIR}/include/tls_port.h)
endforeach()
target_include_directories(ssl INTERFACE ${KAIZHAR_LIBRESSL_DIR}/include)
target_include_directories(crypto INTERFACE ${KAIZHAR_LIBRESSL_DIR}/include)

add_library(kaizhar_tls_port STATIC ${KAIZHAR_TLS_DIR}/tls_port.c)
target_include_directories(kaizhar_tls_port PRIVATE ${KAIZHAR_LIBRESSL_DIR}/include)
# (libcrypto refers to the port file and the port file to libcrypto)
target_link_libraries(crypto INTERFACE kaizhar_tls_port)
target_link_libraries(kaizhar_tls_port INTERFACE crypto)
set(OPENSSL_LIBRARIES ssl crypto kaizhar_tls_port)
