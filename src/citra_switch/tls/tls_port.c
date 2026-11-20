// What LibreSSL asks of a POSIX system and the console's C library doesn't have.
// (src/citra_switch/tls/libressl_switch.cmake)
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

#include <openssl/ui.h>

#include "include/syslog.h"

// The system log: there is none. LibreSSL writes its failed assertions there.
void openlog(const char* ident, int option, int facility) {
    (void)ident;
    (void)option;
    (void)facility;
}

void vsyslog(int priority, const char* format, va_list args) {
    (void)priority;
    (void)format;
    (void)args;
}

void syslog(int priority, const char* format, ...) {
    (void)priority;
    (void)format;
}

void closelog(void) {}

// One user. (The same failed-assertion message names it.)
uid_t getuid(void) {
    return 0;
}

// The page size is the one thing LibreSSL asks for.
long sysconf(int name) {
    if (name == _SC_PAGESIZE) {
        return 0x1000;
    }
    errno = EINVAL;
    return -1;
}

// The prompt for a key's passphrase reads a terminal. There is none: the prompt that answers
// nothing takes its place, so an encrypted key fails to load instead of waiting for input.
UI_METHOD* UI_OpenSSL(void) {
    return (UI_METHOD*)UI_null();
}
