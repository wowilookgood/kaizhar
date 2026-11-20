// POSIX file semantics for the SD card. libnx's fsdev differs from POSIX in
// ways that code ported from desktops relies on (measured):
//   - read() at or past the end of a file fails with EIO instead of returning 0
//     (SQLite reports EIO as a corrupt database; read-to-end loops see an error);
//   - write() past the end leaves the gap with junk instead of zeros;
//   - ftruncate() to a larger size leaves the new bytes as junk;
//   - stat() fails (EIO) on a file that is open for writing: Horizon won't open
//     a file twice while one opening writes, and libnx's stat opens the file
//     for its size. (SQLite stats its database when it creates the journal.)
// hen_fs_posix_sdmc() replaces the "sdmc" device's table with a copy whose
// read, write, ftruncate and stat do what POSIX says (stat of an open file
// answers from the open descriptor), calling libnx's for the rest.
// Call it after libnx has mounted the SD card (userAppInit is after that).
#include <switch.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/reent.h>
#include <sys/stat.h>
#include <unistd.h>

static devoptab_t s_sdmc;  // the patched copy
static devoptab_t s_orig;  // libnx's

// Files open on the device, by path, for stat (see above).
#define MAX_OPEN_FILES 256
static struct {
	void* fd;
	char path[PATH_MAX];
} s_open[MAX_OPEN_FILES];
static Mutex s_openLock;

// The path as the device sees it: without "sdmc:", absolute (relative paths
// are taken against the working directory).
static void normalize(const char* path, char* out, size_t outSize) {
	if (strncmp(path, "sdmc:", 5) == 0) {
		path += 5;
	}
	if (path[0] == '/') {
		snprintf(out, outSize, "%s", path);
		return;
	}
	char cwd[PATH_MAX];
	const char* dir = getcwd(cwd, sizeof(cwd)) ? cwd : "/";
	if (strncmp(dir, "sdmc:", 5) == 0) {
		dir += 5;
	}
	const size_t len = strlen(dir);
	snprintf(out, outSize, "%s%s%s", dir, len > 0 && dir[len - 1] == '/' ? "" : "/", path);
}

static int fs_open(struct _reent* r, void* fileStruct, const char* path, int flags, int mode) {
	const int rc = s_orig.open_r(r, fileStruct, path, flags, mode);
	if (rc != -1) {
		mutexLock(&s_openLock);
		for (int i = 0; i < MAX_OPEN_FILES; i++) {
			if (s_open[i].fd == NULL) {
				s_open[i].fd = fileStruct;
				normalize(path, s_open[i].path, sizeof(s_open[i].path));
				break;
			}
		}
		mutexUnlock(&s_openLock);
	}
	return rc;
}

static int fs_close(struct _reent* r, void* fd) {
	mutexLock(&s_openLock);
	for (int i = 0; i < MAX_OPEN_FILES; i++) {
		if (s_open[i].fd == fd) {
			s_open[i].fd = NULL;
			break;
		}
	}
	mutexUnlock(&s_openLock);
	return s_orig.close_r(r, fd);
}

static int fs_stat(struct _reent* r, const char* path, struct stat* st) {
	const int rc = s_orig.stat_r(r, path, st);
	if (rc == 0) {
		return 0;
	}
	const int err = r->_errno;
	char want[PATH_MAX];
	normalize(path, want, sizeof(want));
	int result = -1;
	mutexLock(&s_openLock);
	for (int i = 0; i < MAX_OPEN_FILES; i++) {
		if (s_open[i].fd != NULL && strcmp(s_open[i].path, want) == 0) {
			result = s_orig.fstat_r(r, s_open[i].fd, st);
			break;
		}
	}
	mutexUnlock(&s_openLock);
	if (result != 0) {
		r->_errno = err;
	}
	return result;
}

static off_t tell(struct _reent* r, void* fd) { return s_orig.seek_r(r, fd, 0, SEEK_CUR); }

static off_t size_of(struct _reent* r, void* fd) {
	struct stat st;
	return s_orig.fstat_r(r, fd, &st) == 0 ? st.st_size : -1;
}

// Writes zeros over [from, to) and leaves the position at `to`.
static int zero_fill(struct _reent* r, void* fd, off_t from, off_t to) {
	static const char zeros[4096];
	if (s_orig.seek_r(r, fd, from, SEEK_SET) != from) {
		return -1;
	}
	while (from < to) {
		const size_t n = (size_t) (to - from < (off_t) sizeof(zeros) ? to - from : (off_t) sizeof(zeros));
		const ssize_t w = s_orig.write_r(r, fd, zeros, n);
		if (w <= 0) {
			return -1;
		}
		from += w;
	}
	return 0;
}

static ssize_t fs_read(struct _reent* r, void* fd, char* ptr, size_t len) {
	const ssize_t n = s_orig.read_r(r, fd, ptr, len);
	if (n < 0 && r->_errno == EIO) {
		const int err = r->_errno;
		const off_t pos = tell(r, fd);
		const off_t size = size_of(r, fd);
		if (pos >= 0 && size >= 0 && pos >= size) {
			r->_errno = 0;
			return 0;  // end of file
		}
		r->_errno = err;
	}
	return n;
}

static ssize_t fs_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
	const off_t pos = tell(r, fd);
	if (pos > 0) {
		const off_t size = size_of(r, fd);
		if (size >= 0 && pos > size && zero_fill(r, fd, size, pos) != 0) {
			return -1;
		}
	}
	return s_orig.write_r(r, fd, ptr, len);
}

static int fs_ftruncate(struct _reent* r, void* fd, off_t len) {
	const off_t size = size_of(r, fd);
	const int rc = s_orig.ftruncate_r(r, fd, len);
	if (rc == 0 && size >= 0 && len > size) {
		const off_t pos = tell(r, fd);
		if (pos < 0 || zero_fill(r, fd, size, len) != 0 || s_orig.seek_r(r, fd, pos, SEEK_SET) != pos) {
			return -1;
		}
	}
	return rc;
}

int hen_fs_posix_sdmc(void) {
	const devoptab_t* sdmc = GetDeviceOpTab("sdmc");
	if (sdmc == NULL || sdmc->read_r == fs_read) {
		return sdmc == NULL ? -1 : 0;
	}
	memcpy(&s_orig, sdmc, sizeof(s_orig));
	memcpy(&s_sdmc, sdmc, sizeof(s_sdmc));
	if (s_orig.read_r) s_sdmc.read_r = fs_read;
	if (s_orig.write_r && s_orig.seek_r && s_orig.fstat_r) s_sdmc.write_r = fs_write;
	if (s_orig.ftruncate_r && s_orig.write_r && s_orig.seek_r && s_orig.fstat_r) s_sdmc.ftruncate_r = fs_ftruncate;
	if (s_orig.open_r && s_orig.close_r && s_orig.stat_r && s_orig.fstat_r) {
		s_sdmc.open_r = fs_open;
		s_sdmc.close_r = fs_close;
		s_sdmc.stat_r = fs_stat;
		if (s_orig.lstat_r == s_orig.stat_r) {
			s_sdmc.lstat_r = fs_stat;
		}
	}
	return AddDevice(&s_sdmc) < 0 ? -1 : 0;  // same name: replaces "sdmc" in place
}
