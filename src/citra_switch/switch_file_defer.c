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
//
// Reads. A lookup that hits reads its record on the caller, which is the
// renderer's thread asking for a pipeline it sees for the first time; on the card that read
// waited behind the thread's writes and behind every other file call of that moment (up to
// 535 ms in one frame). So the file is also kept whole in memory: every write copied in as it
// is queued, and every read answered from that copy. The card then sees the thread's writes only.
//
// The copy is read by a thread of its own after the file is opened, 64 KiB at a time
// under the lock of the real file's position: read inside fopen it held the renderer's start
// for 340 to 400 ms at every launch (the card is busy with other start-up reads then). Until
// the copy is there, reads go to the card and every write is also noted, to be
// laid over the copy when it arrives.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // (fopencookie)
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "common/hen_gap.h"

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
int hen_driver_cache_memory = 1; // --no-driver-cache-memory: reads of that file go to the card

enum { MEM_LIMIT = 32 << 20, MEM_STEP = 4 << 20 }; // (a larger file is read from the card)

enum { MAX_FILES = 4 };
struct deferred {
    FILE* f;
    long pos, size; // (as the caller sees them; the caller serialises its own calls)
    // The file's bytes [0, size), or NULL. The driver makes every call on this file under its
    // one file lock, so this is read and changed by one thread at a time and needs no lock
    // here; the thread that writes to the card works from the queue's copies, never from this.
    // mem, memRoom, loading and the noted writes change under s_mem: the loader thread
    // hands the copy over while the driver's thread may be reading or writing.
    unsigned char* mem;
    long memRoom;
    int loading; // the loader thread is reading this file's copy
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
static unsigned long long s_memReads, s_memBytes; // (changed under the driver's file lock)
static int s_memHeld;
// The loader: one file at a time (the driver has one cache file).
struct noted {
    struct noted* next;
    long off, len;
    unsigned char data[];
};
static pthread_mutex_t s_mem = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_loader;
static int s_loaderStarted, s_loaderStop;
static struct deferred* s_loadFile;
static FILE* s_loadHandle;
static long s_loadSize;
static struct noted *s_notedHead, *s_notedTail;
static size_t s_notedBytes;
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

// (with s_mem held) From here on this file's reads go to the card.
static void mem_drop_locked(struct deferred* d, const char* why) {
    if (d->mem != NULL || d->loading) {
        free(d->mem);
        d->mem = NULL;
        d->loading = 0; // (the loader finds this and throws its copy away)
        printf("hen: files: driver cache no longer held in memory: %s\n", why);
    }
}
static void mem_drop(struct deferred* d, const char* why) {
    pthread_mutex_lock(&s_mem);
    mem_drop_locked(d, why);
    pthread_mutex_unlock(&s_mem);
}

// (with s_mem held) Lays one write over a copy that holds *have bytes; 0 when it cannot.
static int mem_lay(unsigned char** mem, long* room, long* have, long off, const void* p, long len, const char** why) {
    const long end = off + len;
    if (end > MEM_LIMIT) {
        *why = "over 32 MiB";
        return 0;
    }
    if (end > *room) {
        unsigned char* grown = realloc(*mem, (size_t) end + MEM_STEP);
        if (grown == NULL) {
            *why = "out of memory";
            return 0;
        }
        *mem = grown;
        *room = end + MEM_STEP;
    }
    if (off > *have) {
        memset(*mem + *have, 0, (size_t) (off - *have)); // (a gap reads as zeros, as in a file)
    }
    memcpy(*mem + off, p, (size_t) len);
    if (end > *have) {
        *have = end;
    }
    return 1;
}

// Copies a write at the caller's position into the memory copy, before it is queued; while the
// copy is still being read, notes it for the loader instead.
static void mem_write(struct deferred* d, const void* p, long len) {
    pthread_mutex_lock(&s_mem);
    const char* why = NULL;
    if (d->loading) {
        struct noted* e = s_notedBytes + (size_t) len > (size_t) MEM_LIMIT ? NULL : malloc(sizeof(*e) + (size_t) len);
        if (e == NULL) {
            mem_drop_locked(d, "too much written before the copy was read");
        } else {
            e->next = NULL;
            e->off = d->pos;
            e->len = len;
            memcpy(e->data, p, (size_t) len);
            if (s_notedTail != NULL) {
                s_notedTail->next = e;
            } else {
                s_notedHead = e;
            }
            s_notedTail = e;
            s_notedBytes += (size_t) len;
        }
    } else if (d->mem != NULL) {
        long have = d->size;
        if (!mem_lay(&d->mem, &d->memRoom, &have, d->pos, p, len, &why)) {
            mem_drop_locked(d, why);
        }
    }
    pthread_mutex_unlock(&s_mem);
}

// (with s_mem held)
static void noted_free_locked(void) {
    while (s_notedHead != NULL) {
        struct noted* e = s_notedHead;
        s_notedHead = e->next;
        free(e);
    }
    s_notedTail = NULL;
    s_notedBytes = 0;
}

// The loader thread: the file's first s_loadSize bytes (its size when it was opened), then the
// writes made since, in their order, whether or not the card had them yet, and the copy is
// handed over.
static void* loader(void* arg) {
    (void) arg;
    const unsigned long long t = hen_jit_now_ns();
    const long size = s_loadSize;
    long room = size + MEM_STEP, got = 0;
    unsigned char* mem = malloc((size_t) room);
    const char* why = mem == NULL ? "out of memory" : NULL;
    while (why == NULL && got < size && !__atomic_load_n(&s_loaderStop, __ATOMIC_RELAXED)) {
        const long want = size - got < (64 << 10) ? size - got : (64 << 10);
        long r = 0;
        pthread_mutex_lock(&s_io);
        if (__real_fseek(s_loadHandle, got, SEEK_SET) == 0) {
            r = (long) __real_fread(mem + got, 1, (size_t) want, s_loadHandle);
        }
        pthread_mutex_unlock(&s_io);
        if (r <= 0) {
            why = "the read came back short";
        }
        got += r > 0 ? r : 0;
    }
    pthread_mutex_lock(&s_mem);
    struct deferred* d = s_loadFile;
    long have = size, notes = 0;
    if (why == NULL && (got < size || d == NULL || !d->loading)) {
        why = "stopped";
    }
    for (const struct noted* e = s_notedHead; why == NULL && e != NULL; e = e->next) {
        notes++;
        mem_lay(&mem, &room, &have, e->off, e->data, e->len, &why);
    }
    if (why == NULL) {
        d->mem = mem;
        d->memRoom = room;
        d->loading = 0;
        s_memHeld = 1;
        printf("hen: files: driver cache held in memory: %ld KiB read on a thread in %llu ms, %ld write(s) laid over it\n",
               size >> 10, (hen_jit_now_ns() - t) / 1000000, notes);
    } else {
        free(mem);
        if (d != NULL && d->loading) {
            d->loading = 0;
            printf("hen: files: driver cache not held in memory: %s\n", why);
        }
    }
    noted_free_locked();
    pthread_mutex_unlock(&s_mem);
    return NULL;
}

// Ends the loader thread, if any (its stack would otherwise stay locked for the next program).
static void loader_end(void) {
    pthread_mutex_lock(&s_mem);
    const int started = s_loaderStarted;
    s_loaderStarted = 0;
    if (s_loadFile != NULL) {
        s_loadFile->loading = 0;
    }
    pthread_mutex_unlock(&s_mem);
    if (started) {
        __atomic_store_n(&s_loaderStop, 1, __ATOMIC_RELAXED);
        pthread_join(s_loader, NULL);
        __atomic_store_n(&s_loaderStop, 0, __ATOMIC_RELAXED);
    }
    pthread_mutex_lock(&s_mem);
    s_loadFile = NULL;
    noted_free_locked();
    pthread_mutex_unlock(&s_mem);
}

// ("hen: slow:" lines: development builds only)
#if HEN_DEV
#define SLOW_START() hen_jit_now_ns()
static __thread int s_reporting;
static void slow(const char* what, unsigned long long start, void* caller, FILE* f) {
    const unsigned long long ns = hen_jit_now_ns() - start;
    if (f != stdout && f != stderr) {
        hen_gap_add(HEN_GAP_FILE_CALLS, ns); // (common/hen_gap.h: the line about a gap between frames)
        hen_gap_longest(HEN_GAP_LONGEST_FILE, ns, what, 0);
    }
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

#if HEN_DEV
// (--nir-timing) The driver's per-pass compile timer appends its report to the file named by
// NIR_PASS_TIMING, one fopen, some fprintf and one fclose per compile, while holding the lock
// every compiling thread takes at the end of every pass. Given the name "hen:nir-timing" it gets
// a stream that prints each line to the log instead: no card time under that lock.
static pthread_mutex_t s_nirLock = PTHREAD_MUTEX_INITIALIZER;
static char s_nirLine[512];
static size_t s_nirLen;
static ssize_t nir_write(void* cookie, const char* data, size_t size) {
    (void) cookie;
    pthread_mutex_lock(&s_nirLock);
    for (size_t i = 0; i < size; i++) {
        if (data[i] == '\n' || s_nirLen == sizeof(s_nirLine) - 1) {
            s_nirLine[s_nirLen] = 0;
            printf("hen: nir: %s\n", s_nirLine);
            s_nirLen = 0;
            if (data[i] == '\n') {
                continue;
            }
        }
        s_nirLine[s_nirLen++] = data[i];
    }
    pthread_mutex_unlock(&s_nirLock);
    return (ssize_t) size;
}
static int nir_close(void* cookie) {
    (void) cookie;
    return 0;
}
#endif

FILE* __wrap_fopen(const char* path, const char* mode) {
#if HEN_DEV
    if (path != NULL && strcmp(path, "hen:nir-timing") == 0) {
        const cookie_io_functions_t functions = {.read = NULL, .write = nir_write, .seek = NULL, .close = nir_close};
        return fopencookie(NULL, "w", functions);
    }
#endif
    FILE* f = __real_fopen(path, mode);
    if (hen_file_defer && f != NULL && path != NULL && mode != NULL && !s_finished && strstr(path, "ouncevk/shader-cache") != NULL &&
        strchr(mode, '+') != NULL && mode[0] != 'a') {
        long size = -1;
        if (__real_fseek(f, 0, SEEK_END) == 0) {
            size = __real_ftell(f);
        }
        if (size >= 0 && __real_fseek(f, 0, SEEK_SET) == 0) {
            // (the whole file into memory, by the loader thread; nothing is queued for it yet)
            struct deferred* adopted = NULL;
            pthread_mutex_lock(&s_queue);
            for (int i = 0; i < MAX_FILES; i++) {
                if (s_files[i].f == NULL) {
                    s_files[i].pos = 0;
                    s_files[i].size = size;
                    s_files[i].mem = NULL;
                    s_files[i].memRoom = 0;
                    s_files[i].loading = 0;
                    s_files[i].f = f;
                    adopted = &s_files[i];
                    printf("hen: file: writes to %s (%ld KiB) happen on a thread\n", path, size >> 10);
                    break;
                }
            }
            pthread_mutex_unlock(&s_queue);
            if (adopted != NULL) {
                const char* why = NULL;
                pthread_mutex_lock(&s_mem);
                if (!hen_driver_cache_memory) {
                    why = "--no-driver-cache-memory";
                } else if (size > MEM_LIMIT) {
                    why = "over 32 MiB";
                } else if (s_loaderStarted || s_loadFile != NULL) {
                    why = "another file's copy is being read";
                } else {
                    s_loadHandle = f;
                    s_loadSize = size;
                    s_loadFile = adopted;
                    adopted->loading = 1;
                    s_loaderStarted = pthread_create(&s_loader, NULL, loader, NULL) == 0;
                    if (!s_loaderStarted) {
                        adopted->loading = 0;
                        s_loadFile = NULL;
                        why = "the loader thread did not start";
                    }
                }
                pthread_mutex_unlock(&s_mem);
                if (why != NULL) {
                    printf("hen: files: driver cache not held in memory: %s\n", why);
                }
            }
            __real_fseek(f, 0, SEEK_SET);
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
    mem_write(d, p, (long) len);
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
        if (r != len) {
            mem_drop(d, "a write came back short");
        }
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
        if (r != len) {
            mem_drop(d, "a write came back short");
        }
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
    pthread_mutex_lock(&s_mem);
    if (d->mem != NULL) {
        // (no card. Past the end it is a short read, as in a file)
        const long have = d->pos < d->size ? d->size - d->pos : 0;
        const long r = len < have ? len : have;
        memcpy(p, d->mem + d->pos, (size_t) r);
        pthread_mutex_unlock(&s_mem);
        d->pos += r;
        __atomic_fetch_add(&s_memReads, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_memBytes, (unsigned long long) r, __ATOMIC_RELAXED);
        return (size_t) r / s;
    }
    pthread_mutex_unlock(&s_mem);
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
        if (s_loadFile == d) {
            loader_end();
        }
        pthread_mutex_lock(&s_queue);
        drain_locked();
        d->f = NULL;
        pthread_mutex_lock(&s_mem);
        free(d->mem);
        d->mem = NULL;
        pthread_mutex_unlock(&s_mem);
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
    // (the loader thread ends here; a copy it already handed over stays in use)
    pthread_mutex_lock(&s_mem);
    const int loaderStarted = s_loaderStarted;
    s_loaderStarted = 0;
    pthread_mutex_unlock(&s_mem);
    if (loaderStarted) {
        __atomic_store_n(&s_loaderStop, 1, __ATOMIC_RELAXED);
        pthread_join(s_loader, NULL);
        __atomic_store_n(&s_loaderStop, 0, __ATOMIC_RELAXED);
        pthread_mutex_lock(&s_mem);
        s_loadFile = NULL;
        pthread_mutex_unlock(&s_mem);
    }
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
#if HEN_DEV
    if (s_memHeld) {
        printf("hen: files: driver cache: %llu reads (%llu KiB) answered from memory\n",
               __atomic_load_n(&s_memReads, __ATOMIC_RELAXED), __atomic_load_n(&s_memBytes, __ATOMIC_RELAXED) >> 10);
    }
#endif
}
