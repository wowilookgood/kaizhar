// sys/uio.h for the Switch 2 build (newlib has none): struct iovec comes from
// libnx's sys/_iovec.h; readv/writev are in hen/compat/hen_uio.c.
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
