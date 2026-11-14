// Switch 2 build: there is no OpenSSL (externals/CMakeLists.txt). The core
// uses RAND_bytes outside TLS; hen/compat/hen_rand.c implements it with the
// system's random number generator.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// Fills buf with num random bytes. Returns 1 (success), as OpenSSL's does.
int RAND_bytes(unsigned char* buf, int num);

#ifdef __cplusplus
}
#endif
