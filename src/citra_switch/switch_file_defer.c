// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// (Azahar's Switch 2 port) The Vulkan driver's shader cache file, written on a thread.
//
// The driver keeps what it compiles in one file under /config/ouncevk/shader-cache/ and
// writes each new entry while holding the lock that every lookup takes: a seek, two writes
// and a flush, then the same for the file's header, at about 30 ms a write on the SD card.
// A pipeline lookup on the emulation thread waited behind those: in a first launch of
// Pokemon Omega Ruby, 700 ms at a time while the workers stored what they had just compiled.
// The driver is a library, so this is done from outside it: fopen, fseek, ftell, fread,
// fwrite, fflush and fclose are wrapped at link time (CMakeLists.txt), and for that one file
// a write only queues a copy, which a thread writes in order. A read of something still
// queued is answered from the queue; closing the file, or the program's exit, waits for the
// queue. Every other file goes straight through, timed: a call over 30 ms prints a line.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE* __real_fopen(const char*, const char*);
int __real_fseek(FILE*, long, int);
long __real_ftell(FILE*);
size_t __real_fread(void*, size_t, size_t, FILE*);
size_t __real_fwrite(const void*, size_t, size_t, FILE*);
int __real_fflush(FILE*);
int __real_fclose(FILE*);
extern unsigned long long hen_jit_now_ns(void);
extern char _start[];
int hen_file_defer = 1; // --no-file-defer: the driver writes its cache file itself (A/B on a new driver)

enum { MAX_FILES = 4 };
struct deferred {
    FILE* f;
    long pos, size; // (as the caller sees them; the caller serialises its own calls)
};
struct pending {
    struct pending* next;
    FILE* f;
    long off;
    size_t len;
    unsigned char data[];
};
static struct deferred s_files[MAX_FILES];
static struct pending *s_head, *s_tail, *s_current; // (s_current: the one being written)
static size_t s_pendingBytes;
static int s_busy, s_stop, s_started, s_finished;
static unsigned long long s_written, s_writtenBytes, s_skipped, s_fromQueue;
static pthread_mutex_t s_queue = PTHREAD_MUTEX_INITIALIZER; // the queue and the counters
static pthread_mutex_t s_io = PTHREAD_MUTEX_INITIALIZER;    // the real file's position
static pthread_cond_t s_wake = PTHREAD_COND_INITIALIZER, s_idle = PTHREAD_COND_INITIALIZER;
static pthread_t s_thread;

static struct deferred* find(FILE* f) {
    if (f != NULL) {
        for (int i = 0; i < MAX_FILES; i++) {
            if (s_files[i].f == f) {
                return &s_files[i];
            }
        }
    }
    return NULL;
}

// ("hen: slow:" lines: development builds only)
#if HEN_DEV
#define SLOW_START() hen_jit_now_ns()
static __thread int s_reporting;
static void slow(const char* what, unsigned long long start, void* caller, FILE* f) {
    const unsigned long long ns = hen_jit_now_ns() - start;
    if (ns > 30000000ull && !s_reporting && f != stdout && f != stderr) {
        s_reporting = 1;
        printf("hen: slow: %llu ms in %s, called from +%lx\n", ns / 1000000, what,
               (unsigned long) ((char*) caller - _start));
        s_reporting = 0;
    }
}
#else
#define SLOW_START() 0ull
#define slow(what, start, caller, f) ((void)(start))
#endif

static void* writer(void* arg) {
    (void) arg;
    pthread_mutex_lock(&s_queue);
    for (;;) {
        while (s_head == NULL && !s_stop) {
            pthread_cond_wait(&s_wake, &s_queue);
        }
        if (s_head == NULL) {
            break;
        }
        struct pending* e = s_head;
        s_head = e->next;
        if (s_head == NULL) {
            s_tail = NULL;
        }
        s_busy = 1;
        s_current = e;
        // (the file's header is rewritten after every entry: a write that a later one in the
        // queue replaces whole is skipped)
        int skip = 0;
        for (const struct pending* l = s_head; l != NULL; l = l->next) {
            if (l->f == e->f && l->off == e->off && l->len == e->len) {
                skip = 1;
                break;
            }
        }
        const int last = s_head == NULL;
        pthread_mutex_unlock(&s_queue);
        if (!skip) {
            pthread_mutex_lock(&s_io);
            if (__real_fseek(e->f, e->off, SEEK_SET) == 0) {
                __real_fwrite(e->data, 1, e->len, e->f);
            }
            if (last) {
                __real_fflush(e->f);
            }
            pthread_mutex_unlock(&s_io);
        }
        pthread_mutex_lock(&s_queue);
        if (skip) {
            s_skipped++;
        } else {
            s_written++;
            s_writtenBytes += e->len;
        }
        s_pendingBytes -= e->len;
        s_busy = 0;
        s_current = NULL;
        free(e);
        if (s_head == NULL) {
            pthread_cond_broadcast(&s_idle);
        }
    }
    pthread_mutex_unlock(&s_queue);
    return NULL;
}

// (with s_queue held) Waits until everything queued is in the file.
static void drain_locked(void) {
    while (s_head != NULL || s_busy) {
        if (!s_started) {
            break;
        }
        pthread_cond_wait(&s_idle, &s_queue);
    }
}

FILE* __wrap_fopen(const char* path, const char* mode) {
    FILE* f = __real_fopen(path, mode);
    if (hen_file_defer && f != NULL && path != NULL && mode != NULL && !s_finished && strstr(path, "ouncevk/shader-cache") != NULL &&
        strchr(mode, '+') != NULL && mode[0] != 'a') {
        long size = -1;
        if (__real_fseek(f, 0, SEEK_END) == 0) {
            size = __real_ftell(f);
        }
        if (size >= 0 && __real_fseek(f, 0, SEEK_SET) == 0) {
            pthread_mutex_lock(&s_queue);
            for (int i = 0; i < MAX_FILES; i++) {
                if (s_files[i].f == NULL) {
                    s_files[i].pos = 0;
                    s_files[i].size = size;
                    s_files[i].f = f;
                    printf("hen: file: writes to %s (%ld KiB) happen on a thread\n", path, size >> 10);
                    break;
                }
            }
            pthread_mutex_unlock(&s_queue);
        }
    }
    return f;
}

int __wrap_fseek(FILE* f, long off, int whence) {
    struct deferred* d = find(f);
    if (d == NULL) {
        return __real_fseek(f, off, whence);
    }
    const long to = whence == SEEK_SET ? off : whence == SEEK_CUR ? d->pos + off : d->size + off;
    if (to < 0) {
        return -1;
    }
    d->pos = to;
    return 0;
}

long __wrap_ftell(FILE* f) {
    const struct deferred* d = find(f);
    return d != NULL ? d->pos : __real_ftell(f);
}

size_t __wrap_fwrite(const void* p, size_t s, size_t n, FILE* f) {
    struct deferred* d = find(f);
    if (d == NULL) {
        const unsigned long long t = SLOW_START();
        const size_t r = __real_fwrite(p, s, n, f);
        slow("fwrite", t, __builtin_return_address(0), f);
        return r;
    }
    const size_t len = s * n;
    if (len == 0) {
        return 0;
    }
    struct pending* e = s_finished ? NULL : malloc(sizeof(*e) + len);
    if (e == NULL) {
        // (after the exit's drain, or out of memory: written here)
        pthread_mutex_lock(&s_queue);
        drain_locked();
        pthread_mutex_unlock(&s_queue);
        pthread_mutex_lock(&s_io);
        size_t r = 0;
        if (__real_fseek(f, d->pos, SEEK_SET) == 0) {
            r = __real_fwrite(p, 1, len, f);
        }
        pthread_mutex_unlock(&s_io);
        d->pos += (long) r;
        if (d->pos > d->size) {
            d->size = d->pos;
        }
        return r / s;
    }
    e->next = NULL;
    e->f = f;
    e->off = d->pos;
    e->len = len;
    memcpy(e->data, p, len);
    pthread_mutex_lock(&s_queue);
    while (s_started && s_pendingBytes > ((size_t) 64 << 20)) {
        pthread_cond_wait(&s_idle, &s_queue);
    }
    if (!s_started) {
        s_started = pthread_create(&s_thread, NULL, writer, NULL) == 0;
    }
    if (!s_started) {
        pthread_mutex_unlock(&s_queue);
        free(e);
        pthread_mutex_lock(&s_io);
        size_t r = 0;
        if (__real_fseek(f, d->pos, SEEK_SET) == 0) {
            r = __real_fwrite(p, 1, len, f);
        }
        pthread_mutex_unlock(&s_io);
        d->pos += (long) r;
        if (d->pos > d->size) {
            d->size = d->pos;
        }
        return r / s;
    }
    if (s_tail != NULL) {
        s_tail->next = e;
    } else {
        s_head = e;
    }
    s_tail = e;
    s_pendingBytes += len;
    pthread_cond_signal(&s_wake);
    pthread_mutex_unlock(&s_queue);
    d->pos += (long) len;
    if (d->pos > d->size) {
        d->size = d->pos;
    }
    return n;
}

size_t __wrap_fread(void* p, size_t s, size_t n, FILE* f) {
    struct deferred* d = find(f);
    if (d == NULL) {
        const unsigned long long t = SLOW_START();
        const size_t r = __real_fread(p, s, n, f);
        slow("fread", t, __builtin_return_address(0), f);
        return r;
    }
    const long len = (long) (s * n);
    if (len == 0) {
        return 0;
    }
    pthread_mutex_lock(&s_queue);
    const struct pending* hit = NULL;
    int partial = 0;
    for (const struct pending* e = s_current != NULL ? s_current : s_head; e != NULL;
         e = e == s_current ? s_head : e->next) {
        if (e->f != f || e->off >= d->pos + len || e->off + (long) e->len <= d->pos) {
            continue;
        }
        if (e->off <= d->pos && e->off + (long) e->len >= d->pos + len) {
            hit = e;
            partial = 0;
        } else {
            hit = NULL;
            partial = 1;
        }
    }
    if (hit != NULL) {
        memcpy(p, hit->data + (d->pos - hit->off), (size_t) len);
        s_fromQueue++;
        pthread_mutex_unlock(&s_queue);
        d->pos += len;
        return n;
    }
    if (partial) {
        drain_locked();
    }
    pthread_mutex_unlock(&s_queue);
    pthread_mutex_lock(&s_io);
    size_t r = 0;
    if (__real_fseek(f, d->pos, SEEK_SET) == 0) {
        r = __real_fread(p, 1, (size_t) len, f);
    }
    pthread_mutex_unlock(&s_io);
    d->pos += (long) r;
    return r / s;
}

int __wrap_fflush(FILE* f) {
    if (find(f) != NULL && !s_finished) {
        return 0;
    }
    const unsigned long long t = SLOW_START();
    const int r = __real_fflush(f);
    slow("fflush", t, __builtin_return_address(0), f);
    return r;
}

int __wrap_fclose(FILE* f) {
    struct deferred* d = find(f);
    if (d != NULL) {
        pthread_mutex_lock(&s_queue);
        drain_locked();
        d->f = NULL;
        pthread_mutex_unlock(&s_queue);
    }
    const unsigned long long t = SLOW_START();
    const int r = __real_fclose(f);
    slow("fclose", t, __builtin_return_address(0), NULL);
    return r;
}

// At exit: everything queued is written and the thread ends (its stack would otherwise stay
// locked for the next program). Later writes, if any, happen where they are called.
void hen_file_defer_finish(void) {
    pthread_mutex_lock(&s_queue);
    s_finished = 1;
    s_stop = 1;
    const int started = s_started;
    pthread_cond_signal(&s_wake);
    pthread_mutex_unlock(&s_queue);
    if (started) {
        pthread_join(s_thread, NULL);
        pthread_mutex_lock(&s_queue);
        s_started = 0;
        pthread_mutex_unlock(&s_queue);
        printf("hen: file: the driver's cache: %llu writes (%llu KiB) on the thread, %llu skipped as replaced, %llu reads from the queue\n",
               s_written, s_writtenBytes >> 10, s_skipped, s_fromQueue);
    }
}
