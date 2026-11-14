// HEN_CRUMB (src/common/hen_crumb.h): prints where execution is, at once, when
// enabled. A repeat of the last crumb on the same thread is counted instead.
#include <switch.h>

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

bool hen_crumbs_enabled;

static Mutex s_lock;
static const char* s_last;
static void* s_lastThread;
static unsigned s_repeats;

void hen_crumb(const char* where) {
	if (!hen_crumbs_enabled) {
		return;
	}
	void* thread = armGetTls();
	mutexLock(&s_lock);
	if (where == s_last && thread == s_lastThread) {
		s_repeats++;
	} else {
		char line[160];
		int n = 0;
		if (s_repeats > 0) {
			n = snprintf(line, sizeof(line), "at:   (x%u more)\n", s_repeats);
			s_repeats = 0;
		}
		n += snprintf(line + n, sizeof(line) - n, "at: %s [core %d]\n", where, svcGetCurrentProcessorNumber());
		write(STDERR_FILENO, line, n);
		s_last = where;
		s_lastThread = thread;
	}
	mutexUnlock(&s_lock);
}
