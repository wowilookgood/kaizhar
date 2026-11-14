// C library functions newlib/libnx lack that the Switch 2 build links against.
#include <switch.h>

#include <errno.h>
#include <pwd.h>
#include <reent.h>
#include <signal.h>

// newlib's arc4random (cryptopp's random source here) gets its seed from this.
int _getentropy_r(struct _reent* r, void* buf, size_t len) {
	(void) r;
	randomGet(buf, len);
	return 0;
}

// No signals on Horizon. cryptopp's SIGILL-based CPU probes reference this;
// they aren't called (cpu.cpp sets the features directly).
int sigprocmask(int how, const sigset_t* set, sigset_t* old) {
	(void) how;
	(void) set;
	if (old != NULL) {
		sigemptyset(old);
	}
	return 0;
}

// No user database. (FileUtil::GetHomeDirectory falls back to it without
// $HOME; the frontend sets the user directory itself.)
struct passwd* getpwuid(uid_t uid) {
	(void) uid;
	errno = ENOENT;
	return NULL;
}
