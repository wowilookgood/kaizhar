// POSIX functions that newlib and libounce leave out but libraries call:
// fchown (SQLite's unix VFS, only when running as root), dlfcn, pread
// (borealis::io), execv (Dusklight's restart) and a rename that replaces.
#include <errno.h>
#include <limits.h>
#include <sys/types.h>

int fchown(int fd, uid_t owner, gid_t group) {
	(void) fd;
	(void) owner;
	(void) group;
	errno = ENOSYS;
	return -1;
}

// dlfcn (include/dlfcn.h): no dynamic loading on Switch.
#include <dlfcn.h>
#include <stddef.h>

static const char* s_dlError = NULL;

void* dlopen(const char* path, int mode) {
	(void) path;
	(void) mode;
	s_dlError = "dlopen: loading native libraries isn't supported on Switch";
	return NULL;
}

void* dlsym(void* handle, const char* name) {
	(void) handle;
	(void) name;
	s_dlError = "dlsym: no dynamic symbols on Switch";
	return NULL;
}

int dlclose(void* handle) {
	(void) handle;
	return 0;
}

char* dlerror(void) {
	const char* e = s_dlError;
	s_dlError = NULL;
	return (char*) e;
}

int dladdr(const void* addr, Dl_info* info) {
	(void) addr;
	(void) info;
	return 0;
}

// pread: newlib has none. Seek, read and seek back, under one lock so that
// concurrent preads (on any descriptor) don't move each other's position.
#include <switch.h>
#include <unistd.h>

static Mutex s_preadLock;

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
	mutexLock(&s_preadLock);
	const off_t saved = lseek(fd, 0, SEEK_CUR);
	ssize_t n = -1;
	if (saved >= 0 && lseek(fd, offset, SEEK_SET) == offset) {
		n = read(fd, buf, count);
		const int err = errno;
		lseek(fd, saved, SEEK_SET);
		errno = err;
	}
	mutexUnlock(&s_preadLock);
	return n;
}

// execv: no processes to replace, but loaders that support it can start an
// .nro after this one exits (envSetNextLoad). The .nro is this one
// (__system_argv[0]); `path` is ignored, as callers derive it from argv[0]
// with desktop path rules. Returns only on failure, like execv.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char** __system_argv;

int execv(const char* path, char* const argv[]) {
	(void) path;
	if (!envHasNextLoad() || __system_argv == NULL || __system_argv[0] == NULL) {
		errno = ENOSYS;
		return -1;
	}
	// The loader splits the argument string on spaces, honouring double quotes.
	char args[1024];
	size_t len = 0;
	const char* self = __system_argv[0];
	for (int i = 0; i == 0 || (argv != NULL && argv[i] != NULL); ++i) {
		const char* arg = i == 0 ? self : argv[i];
		const int n = snprintf(args + len, sizeof(args) - len, "%s\"%s\"", i == 0 ? "" : " ", arg);
		if (n < 0 || (size_t) n >= sizeof(args) - len) {
			errno = E2BIG;
			return -1;
		}
		len += (size_t) n;
	}
	if (R_FAILED(envSetNextLoad(self, args))) {
		errno = EIO;
		return -1;
	}
	exit(0);
}

// rename: POSIX replaces an existing destination; Horizon's file system
// refuses (EEXIST), which breaks write-to-temp-then-rename saves (Dusklight's
// config, borealis::io::atomic_replace). On EEXIST: move the destination aside,
// rename, then delete it, putting it back if the rename fails. Not atomic, but
// the old file survives a failed rename. Linked with -Wl,--wrap=rename.
#include <sys/stat.h>

int __real_rename(const char* from, const char* to);

int __wrap_rename(const char* from, const char* to) {
	if (__real_rename(from, to) == 0) {
		return 0;
	}
	if (errno != EEXIST) {
		return -1;
	}
	struct stat st;
	if (stat(to, &st) != 0 || S_ISDIR(st.st_mode)) {
		errno = EEXIST;
		return -1;
	}
	char aside[PATH_MAX];
	if (snprintf(aside, sizeof(aside), "%s.hen-old", to) >= (int) sizeof(aside)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	unlink(aside);
	if (__real_rename(to, aside) != 0) {
		return -1;
	}
	if (__real_rename(from, to) != 0) {
		const int err = errno;
		__real_rename(aside, to);
		errno = err;
		return -1;
	}
	unlink(aside);
	return 0;
}

// mmap/munmap: not available (see include/sys/mman.h).
#include <sys/mman.h>

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
	(void) addr;
	(void) length;
	(void) prot;
	(void) flags;
	(void) fd;
	(void) offset;
	errno = ENOSYS;
	return MAP_FAILED;
}

int munmap(void* addr, size_t length) {
	(void) addr;
	(void) length;
	errno = EINVAL;
	return -1;
}
