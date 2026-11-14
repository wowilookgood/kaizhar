// readv/writev for the Switch 2 build (hen/compat/sys/uio.h): one read or
// write per buffer, stopping at the first short one.
#include <sys/uio.h>

#include <unistd.h>

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
	ssize_t total = 0;
	for (int i = 0; i < iovcnt; i++) {
		const ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
		if (n < 0) {
			return total > 0 ? total : n;
		}
		total += n;
		if ((size_t) n < iov[i].iov_len) {
			break;
		}
	}
	return total;
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
	ssize_t total = 0;
	for (int i = 0; i < iovcnt; i++) {
		const ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
		if (n < 0) {
			return total > 0 ? total : n;
		}
		total += n;
		if ((size_t) n < iov[i].iov_len) {
			break;
		}
	}
	return total;
}
