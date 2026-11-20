// sys/uio.h for LibreSSL (newlib has none). The same declarations as hen/compat/sys/uio.h, which
// can't be on LibreSSL's include path: its openssl/rand.h would hide the real one.
#pragma once

#include <sys/types.h>
#include <sys/_iovec.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t readv(int fd, const struct iovec* iov, int iovcnt);
ssize_t writev(int fd, const struct iovec* iov, int iovcnt);

#ifdef __cplusplus
}
#endif
