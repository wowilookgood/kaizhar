// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// See switch_platform.h. Also the app's start-up and exit hooks: SD card POSIX
// fixes (hen_fs.c), sockets and the log connection.
#include <stdarg.h>
#include <poll.h>
#include <switch.h>

#include <errno.h>
#include <math.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/lock.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "switch_platform.h"
#if HEN_CHAIN
#include <hen_chain.h>
#endif

#define SW_WIDTH 1920
#define SW_HEIGHT 1080

int hen_fs_posix_sdmc(void);
extern int __system_argc;
extern char** __system_argv;

extern char _start[]; // the first instruction of the program
static PadState s_pad;
static bool s_padReady;

// hen_thread.c: unmaps the stacks of threads nobody joined, so the next app, which
// runs in this same process (the homebrew menu), gets the whole heap back.
int hen_thread_release_stacks(void* (**funcs)(void*), int max, size_t* bytes);
int hen_thread_live(void* (**funcs)(void*), void** args, int max);
unsigned hen_jit_mappings_in_use(void); // hen/jit/hen_jit.c
unsigned hen_jit_release_all(void);
void hen_jit_heap_left(unsigned long long* out);
#if HEN_DEV
void hen_jit_heap_walk(const char* label, char* out, size_t outSize); // hen/jit/hen_jit.c
#endif

// (The lines printed here and in userAppExit mark how far a normal exit got: an error screen
// at exit with all of them in the log happened after this program's own code.)
int hen_thread_stop_live(int wait_ms); // hen_thread.c
extern int hen_thread_left_stats[2];
static int sw_end_waiting_threads(void);
// (The same lines on the card, every build: /switch/Kaizhar/log/exit.txt, the one before it kept as
// exit.old.txt. A fault after this program's own code has ended, in a thread it left running or in
// what is started next, shows as the system's error screen and leaves no crash.txt: the
// fault handler is gone with the program. This file is written before that, and names what was
// still alive.)
static char s_exitNote[3072];
static size_t s_exitNoteLen;
static void exit_note(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    if (s_exitNoteLen < sizeof(s_exitNote) - 1) {
        va_start(args, format);
        const int n = vsnprintf(s_exitNote + s_exitNoteLen, sizeof(s_exitNote) - s_exitNoteLen, format, args);
        va_end(args);
        if (n > 0) {
            s_exitNoteLen += (size_t)n < sizeof(s_exitNote) - s_exitNoteLen ? (size_t)n : sizeof(s_exitNote) - 1 - s_exitNoteLen;
        }
    }
}
/// One more line at the end of exit.txt: how far the last steps of closing got.
static void exit_note_mark(const char* text) {
    FILE* const file = fopen("/switch/Kaizhar/log/exit.txt", "a");
    if (file != NULL) {
        fputs(text, file);
        fclose(file);
    }
}
static void exit_note_store(void) {
    remove("/switch/Kaizhar/log/exit.old.txt");
    rename("/switch/Kaizhar/log/exit.txt", "/switch/Kaizhar/log/exit.old.txt");
    FILE* const file = fopen("/switch/Kaizhar/log/exit.txt", "w");
    if (file != NULL) {
        fprintf(file, "%s\n(what this program left behind as it closed, for a problem report: offsets in the program, "
                      "nothing that names the user or the network)\nconsole %s at exit\n",
                SW_VERSION_TEXT, sw_docked() ? "docked" : "handheld");
        fwrite(s_exitNote, 1, s_exitNoteLen, file);
        fclose(file);
    }
}
static void release_thread_stacks(void) {
    void* (*funcs[8])(void*);
    size_t bytes = 0;
    // First: threads still running get a second to end by themselves. Then those waiting on a
    // condition variable are woken and end inside the wait (see __wrap___syscall_cond_wait
    // below). What is left after that is paused for good.
    void* (*waiting[16])(void*);
    void* waiting_args[16];
    int running = 0;
    for (int waited = 0; waited < 1000 && (running = hen_thread_live(waiting, waiting_args, 16)) != 0; waited += 10) {
        svcSleepThread(10000000);
    }
    if (running != 0) {
        // Which they are, before they are woken (a std::thread's argument is its state object,
        // freed when the thread ends: its vtable names what the thread runs).
        exit_note("hen: exit: still running:");
        for (int i = 0; i < running && i < 16; i++) {
            exit_note(" +0x%lx", (unsigned long)((uintptr_t)waiting[i] - (uintptr_t)_start));
            if (waiting_args[i] != NULL && ((uintptr_t)waiting_args[i] & 7) == 0) {
                exit_note(" (arg vtable +0x%lx)", (unsigned long)(*(uintptr_t*)waiting_args[i] - (uintptr_t)_start));
            }
        }
        exit_note("\n");
        const int woken = sw_end_waiting_threads();
        for (int waited = 0; waited < 500 && hen_thread_live(waiting, waiting_args, 16) != 0; waited += 10) {
            svcSleepThread(10000000);
        }
        svcSleepThread(20000000); // (a thread that has just left the list is on its last instructions)
        exit_note("hen: exit: %d thread(s) were still running; %d condition variable(s) signalled, %d thread(s) left\n",
               running, woken, hen_thread_live(waiting, waiting_args, 16));
    }
    const int stopped = hen_thread_stop_live(300);
    if (stopped != 0) {
        exit_note("hen: exit: %d thread(s) were still running and have been paused, their stacks unmapped\n", stopped);
    }
    const int count = hen_thread_release_stacks(funcs, 8, &bytes);
    exit_note("hen: exit: %d thread(s) nobody joined, %zu KiB of stacks released", count, bytes >> 10);
    for (int i = 0; i < count && i < 8; i++) {
        exit_note("%s+0x%lx", i == 0 ? ": " : ", ", (unsigned long)((uintptr_t)funcs[i] - (uintptr_t)_start));
    }
    exit_note("\n");
    if (hen_thread_left_stats[0] != 0 || hen_thread_left_stats[1] != 0) {
        exit_note("hen: exit: of the threads ended from inside a wait, %d had ended before their stacks were released; "
               "%d had not after 500 ms (their stacks are left mapped)\n",
               hen_thread_left_stats[0], hen_thread_left_stats[1]);
    }
    // Threads still running: they keep running this program's code after it has exited.
    void* (*live[16])(void*);
    void* args[16];
    const int alive = hen_thread_live(live, args, 16);
    const unsigned left = hen_jit_mappings_in_use();
    const unsigned released = hen_jit_release_all();
    exit_note("hen: exit: %u code mapping(s) left, %u released now; %d thread(s) still running", left, released, alive);
    for (int i = 0; i < alive && i < 16; i++) {
        exit_note("%s+0x%lx", i == 0 ? ": " : ", ", (unsigned long)((uintptr_t)live[i] - (uintptr_t)_start));
        // (a std::thread's argument is its state object: the vtable names what it runs)
        if (args[i] != NULL && ((uintptr_t)args[i] & 7) == 0) {
            exit_note(" (arg vtable +0x%lx)", (unsigned long)(*(uintptr_t*)args[i] - (uintptr_t)_start));
        }
    }
    exit_note("\n");
    // Heap the kernel still shows as referred to by a request or a device, or
    // with an attribute, after all of the above: the next program in this process cannot map
    // code from it. Printed when there is any (development builds: always).
    {
        unsigned long long heap_left[4];
        hen_jit_heap_left(heap_left);
#if HEN_DEV
        // (each block and whose it is; still to come after this point: this file's
        // userAppExit, where the sockets close, and then the runtime's own services)
        hen_jit_heap_walk("exit: heap still held", NULL, 0);
#else
        if (heap_left[3] != 0)
#endif
            exit_note("hen: exit: heap still held: %llu KiB with an attribute, %llu KiB referred to by a request, "
                   "%llu KiB by a device (%llu block(s))\n",
                   heap_left[0] >> 10, heap_left[1] >> 10, heap_left[2] >> 10, heap_left[3]);
    }
    fflush(stdout);
    exit_note_store();
}

// The user folder was /switch/Azahar in the test builds before the port had a name of its own.
// Once, at start, before anything opens a file in it: an Azahar folder is renamed to Kaizhar
// when there is no Kaizhar folder yet. With both, Kaizhar is used and Azahar is left alone.
// Returns the line for the log (the log isn't open yet), or NULL when there was nothing to do.
static const char* migrate_user_folder(void) {
    static char line[160];
    struct stat st;
    if (stat("/switch/Azahar", &st) != 0 || !S_ISDIR(st.st_mode)) {
        return NULL;
    }
    if (stat("/switch/Kaizhar", &st) == 0) {
        return "hen: user folder: /switch/Azahar and /switch/Kaizhar both exist; using /switch/Kaizhar";
    }
    if (rename("/switch/Azahar", "/switch/Kaizhar") == 0) {
        return "hen: user folder: renamed /switch/Azahar to /switch/Kaizhar";
    }
    snprintf(line, sizeof(line),
             "hen: user folder: /switch/Azahar could not be renamed to /switch/Kaizhar (errno %d)", errno);
    return line;
}

#if HEN_DEV
static char s_heap_on_entry[6144];
#endif

static int s_log_socket = -1; // the log connection, if one was made

void userAppInit(void) {
#if HEN_DEV
    // (before anything of ours is allocated or opened; there is no log yet: printed below)
    hen_jit_heap_walk("start: heap held on entry", s_heap_on_entry, sizeof(s_heap_on_entry));
#endif
    atexit(release_thread_stacks); // registered first: runs last
    const int fs = hen_fs_posix_sdmc();
    const char* const folder_note = migrate_user_folder();
    const Result romfs = romfsInit();
    const Result sockets = socketInitializeDefault();
#if HEN_CHAIN
    // The optional test library (not included in this repository: HEN_CHAIN_DIR in
    // CMakeLists.txt here). It is started at this point, once the card and the sockets are
    // up and before the log connection below is made, and is given the program's folder
    // on the card and the result of starting the sockets. Nothing of it is compiled
    // unless HEN_CHAIN is 1.
    char chain_note[32];
    snprintf(chain_note, sizeof(chain_note), "sockets 0x%x", sockets);
    const struct hen_chain_options chain = {.dir = "sdmc:/switch/Kaizhar", .note_extra = chain_note};
    hen_chain_init(&chain);
#endif
    // The log connection to the computer that sent the program. The runtime's connect waits one
    // second and gives up (a start without any log: about one start in 25),
    // so it is tried again a few times here.
    int log_tries = 0;
    if (R_SUCCEEDED(sockets) && __nxlink_host.s_addr != 0) {
        while (log_tries < 4 && s_log_socket < 0) {
            if (log_tries++ != 0) {
                svcSleepThread(250000000ull);
            }
            s_log_socket = nxlinkStdio();
        }
        setvbuf(stdout, NULL, _IOLBF, 0);
    }
    printf("hen: " SW_VERSION_TEXT "\n");
    if (log_tries > 1) {
        printf("hen: log: connected to the computer at try %d of 4\n", log_tries);
    }
    printf("hen: starting%s%s; romfs 0x%x, sockets 0x%x, sdmc POSIX fixes %s\n",
           __system_argc > 0 ? " " : "", __system_argc > 0 ? __system_argv[0] : "", romfs, sockets,
           fs == 0 ? "on" : "FAILED");
    if (folder_note != NULL) {
        printf("%s\n", folder_note);
    }
#if HEN_DEV
    fputs(s_heap_on_entry, stdout);
#endif
}

// --cpu-boost N (experiment): the system's CPU boost mode, the one games ask for while they
// load. Says what the calls returned and what the performance configuration is before and
// after; set back to normal at exit, because the next program runs in this same process.
static int s_cpu_boost = 0;
#if HEN_DEV
void sw_cpu_boost(int mode) {
    ApmPerformanceMode perf_mode = ApmPerformanceMode_Invalid;
    u32 before = 0, after = 0;
    const Result rc_apm = apmInitialize();
    const Result rc_mode = R_SUCCEEDED(rc_apm) ? apmGetPerformanceMode(&perf_mode) : rc_apm;
    if (R_SUCCEEDED(rc_mode)) {
        apmGetPerformanceConfiguration(perf_mode, &before);
    }
    const Result rc = appletSetCpuBoostMode((ApmCpuBoostMode)mode);
    if (R_SUCCEEDED(rc_mode)) {
        apmGetPerformanceConfiguration(perf_mode, &after);
    }
    printf("hen: cpu boost %d: set rc 0x%x; apm rc 0x%x, performance mode %d (rc 0x%x), configuration 0x%08x -> 0x%08x\n",
           mode, rc, rc_apm, (int)perf_mode, rc_mode, before, after);
    if (R_SUCCEEDED(rc_apm)) {
        apmExit();
    }
    s_cpu_boost = R_SUCCEEDED(rc) ? mode : 0;
    if (mode == 0) {
        printf("hen: cpu boost: normal mode requested, rc 0x%x\n", rc);
    }
}
#endif

// Called by main before "hen: exiting" (the runtime's exit hook is too late for the log, and
// was not seen to run here).
void sw_cpu_boost_end(void) {
    if (s_cpu_boost != 0) {
        // (the cancel command is what clears the request)
        const Result rc = appletCancelCpuBoostMode();
        printf("hen: exit: cpu boost back to normal (cancel), rc 0x%x\n", rc);
        s_cpu_boost = 0;
    }
}

// The loading boost's CPU clock (hen/jit/hen_jit.c, docked only) and --clock-info: the clock
// controller service. The CPU rate can be set directly,
// without the system's boost mode, which raises the CPU but lowers the GPU. Running the CPU
// faster with the GPU at full speed is outside what the system's own software does (more heat,
// more power), so: only docked, only up to the rate asked for and never above the range the
// service reports, the starting rate restored at exit, and every step in the log.
static ClkrstSession s_clk_cpu, s_clk_gpu;
static bool s_clk_ready;
static u32 s_clk_cpu_start, s_clk_cpu_set;
// For the session report (sw_clock_record): the last raise of the CPU clock and its undoing.
static u32 s_clk_asked, s_clk_set_rc, s_clk_read_back, s_clk_end_rc, s_clk_refusals;
static bool clock_open(void) {
    if (s_clk_ready) {
        return true;
    }
    // (a line before the first call: a launch that stops here would otherwise leave no word
    // about where)
    printf("hen: clock: opening the clock controller\n");
    fflush(stdout);
    Result rc = clkrstInitialize();
    if (R_FAILED(rc)) {
        printf("hen: clock: no clock controller service (rc 0x%x)\n", rc);
        return false;
    }
    rc = clkrstOpenSession(&s_clk_cpu, PcvModuleId_CpuBus, 3);
    const Result rc_gpu = clkrstOpenSession(&s_clk_gpu, PcvModuleId_GPU, 3);
    if (R_FAILED(rc) || R_FAILED(rc_gpu)) {
        printf("hen: clock: sessions refused (CPU rc 0x%x, GPU rc 0x%x)\n", rc, rc_gpu);
        return false;
    }
    s_clk_ready = true;
    return true;
}
#if HEN_DEV
static void clock_one(ClkrstSession* session, const char* name) {
    u32 hz = 0, rates[2] = {0, 0};
    PcvClockRatesListType type = PcvClockRatesListType_Invalid;
    s32 count = 0;
    const Result rc = clkrstGetClockRate(session, &hz);
    const Result rc_range = clkrstGetPossibleClockRates(session, rates, 2, &type, &count);
    printf(" %s %u.%03u MHz (rc 0x%x), offered %u.%03u to %u.%03u MHz (list type %d, %d entries, rc 0x%x);",
           name, hz / 1000000, hz / 1000 % 1000, rc, rates[0] / 1000000, rates[0] / 1000 % 1000,
           rates[1] / 1000000, rates[1] / 1000 % 1000, (int)type, (int)count, rc_range);
}
void sw_clock_report(const char* when) {
    if (!clock_open()) {
        return;
    }
    printf("hen: clock (%s): %s;", when, appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
    clock_one(&s_clk_cpu, "CPU");
    clock_one(&s_clk_gpu, "GPU");
    s32 milli = 0;
    Result rc = tsInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = tsGetTemperatureMilliC(TsLocation_Internal, &milli);
        if (R_FAILED(rc)) {
            s32 whole = 0;
            rc = tsGetTemperature(TsLocation_Internal, &whole);
            milli = whole * 1000;
        }
        tsExit();
    }
    printf(" temperature %d.%d C (rc 0x%x)\n", milli / 1000, milli / 100 % 10, rc);
}
#endif
// Returns 1 if the rate was set (and read back), 0 if refused or failed. verbose: a line in
// the log every time; otherwise only the first time and on failure (the loading boost switches
// many times a session).
static int cpu_clock_set(unsigned hz, bool verbose) {
    static bool told;
    if (!clock_open()) {
        return 0;
    }
    u32 now = 0, rates[2] = {0, 0};
    PcvClockRatesListType type = PcvClockRatesListType_Invalid;
    s32 count = 0;
    if (R_FAILED(clkrstGetClockRate(&s_clk_cpu, &now)) ||
        R_FAILED(clkrstGetPossibleClockRates(&s_clk_cpu, rates, 2, &type, &count)) ||
        type != PcvClockRatesListType_Range || count != 2 || hz < rates[0] || hz > rates[1]) {
        printf("hen: clock: %u Hz is not in the range the service offers (%u to %u): left alone\n", hz, rates[0], rates[1]);
        s_clk_refusals++;
        return 0;
    }
    if (s_clk_cpu_set == 0) {
        s_clk_cpu_start = now; // (what to go back to: the rate found, each time it is raised)
    }
    const Result rc = clkrstSetClockRate(&s_clk_cpu, hz);
    u32 after = 0;
    const Result rc_read = clkrstGetClockRate(&s_clk_cpu, &after);
    if (verbose || !told || R_FAILED(rc) || after != hz) {
        told = true;
        printf("hen: clock: CPU %u -> %u Hz asked, rc 0x%x; reads back %u (rc 0x%x)\n", now, hz, rc, after, rc_read);
    }
    if (R_SUCCEEDED(rc)) {
        s_clk_cpu_set = hz;
    }
    s_clk_asked = hz;
    s_clk_set_rc = rc;
    s_clk_read_back = after;
    return R_SUCCEEDED(rc) && after == hz;
}
// out[0]: 1 if the clock controller is open (it is opened by the first raise only: the rates
// are not asked for otherwise); [1], [2]: the CPU's and the GPU's rate now, Hz, 0 if not
// readable; [3]: the rate last asked for; [4]: that call's result; [5]: what the rate read
// back as; [6]: the result of the last return to the starting rate; [7]: the starting rate;
// [8]: times a rate was outside what the service offers.
void sw_clock_record(unsigned* out) {
    memset(out, 0, 9 * sizeof(out[0]));
    out[0] = s_clk_ready;
    if (s_clk_ready) {
        u32 hz = 0;
        if (R_SUCCEEDED(clkrstGetClockRate(&s_clk_cpu, &hz))) {
            out[1] = hz;
        }
        if (R_SUCCEEDED(clkrstGetClockRate(&s_clk_gpu, &hz))) {
            out[2] = hz;
        }
    }
    out[3] = s_clk_asked;
    out[4] = s_clk_set_rc;
    out[5] = s_clk_read_back;
    out[6] = s_clk_end_rc;
    out[7] = s_clk_cpu_start;
    out[8] = s_clk_refusals;
}
bool sw_docked(void) {
    return appletGetOperationMode() == AppletOperationMode_Console;
}
// The CPU's rate as the clock controller reads it now, Hz; 0 if it cannot be read.
unsigned sw_cpu_clock_now(void) {
    u32 hz = 0;
    return s_clk_ready && R_SUCCEEDED(clkrstGetClockRate(&s_clk_cpu, &hz)) ? hz : 0;
}
int sw_cpu_clock_quiet(unsigned hz) {
    return cpu_clock_set(hz, false);
}
// --n3ds-clock-high (hen_jit.c, boost_tier): another rate asked for while the clock is already
// raised. Same checks as any raise (the offered range is read again each time). 1 if the rate
// was set and reads back. out[0]: 0 asked, 1 outside what the service offers, 2 not asked at
// all (no service, or handheld without the clock); [1]: the request's result; [2]: what the
// rate read back as (both only when asked).
int sw_cpu_clock_step(unsigned hz, unsigned* out) {
    const u32 refusals = s_clk_refusals;
    s_clk_asked = 0;
    const int ok = cpu_clock_set(hz, false);
    out[0] = s_clk_refusals != refusals ? 1 : s_clk_asked != hz ? 2 : 0;
    out[1] = out[0] == 0 ? s_clk_set_rc : 0;
    out[2] = out[0] == 0 ? s_clk_read_back : 0;
    return ok;
}
// The console was docked or undocked: the system has set its own rates for the new mode, so
// the rate found before is no longer what to go back to (if it has not, it is put back).
void sw_cpu_clock_forget(void) {
    u32 now = 0;
    if (s_clk_cpu_set == 0 || R_FAILED(clkrstGetClockRate(&s_clk_cpu, &now))) {
        return;
    }
    printf("hen: clock: mode changed: CPU reads %u Hz (was set to %u, found at %u)\n", now, s_clk_cpu_set, s_clk_cpu_start);
    if (now != s_clk_cpu_set) {
        s_clk_cpu_set = 0; // (the system's rate for the new mode: nothing of ours to undo)
    }
    // (still our rate: sw_cpu_clock_end, called next, puts back what was found)
}
// Handheld, while the rate is raised (hen_jit.c, once a second): a rate set through the clock
// controller lasts until the system applies its own configuration again. If the CPU no longer
// reads what was set, it is set again; the first time is in the log, and the count at the end.
static unsigned s_clk_kept;
void sw_cpu_clock_keep(void) {
    u32 now = 0;
    if (!s_clk_ready || s_clk_cpu_set == 0 || R_FAILED(clkrstGetClockRate(&s_clk_cpu, &now)) ||
        now == s_clk_cpu_set) {
        return;
    }
    const Result rc = clkrstSetClockRate(&s_clk_cpu, s_clk_cpu_set);
    u32 after = 0;
    clkrstGetClockRate(&s_clk_cpu, &after);
    if (s_clk_kept++ == 0 || R_FAILED(rc) || after != s_clk_cpu_set) {
        printf("hen: clock: CPU read %u Hz, not the %u Hz set: the system changed it; set again, rc 0x%x, "
               "reads back %u (said the first time and on failure)\n", now, s_clk_cpu_set, rc, after);
    }
}
void sw_cpu_clock_end(void) {
    static bool told;
    if (s_clk_cpu_set != 0 && s_clk_cpu_start != 0) {
        const Result rc = clkrstSetClockRate(&s_clk_cpu, s_clk_cpu_start);
        u32 after = 0;
        clkrstGetClockRate(&s_clk_cpu, &after);
        if (!told || R_FAILED(rc) || after != s_clk_cpu_start) {
            told = true;
            printf("hen: clock: CPU back to %u Hz, rc 0x%x; reads back %u\n", s_clk_cpu_start, rc, after);
        }
        s_clk_end_rc = rc;
        s_clk_cpu_set = 0;
    }
    if (s_clk_kept != 0) {
        printf("hen: clock: the CPU rate was set again %u time(s) after the system had changed it\n", s_clk_kept);
        s_clk_kept = 0;
    }
}

#if HEN_CHAIN
// The optional test library's last call (not included in this repository: HEN_CHAIN_DIR in
// CMakeLists.txt here), once: at the end of main, or from userAppExit when the program
// ended some other way (abort() after a failed start, an uncaught exception). A second
// thread arriving waits for the first to finish (and the same thread arriving again,
// should the call itself fail, passes).
static RMutex s_chain_lock;
static bool s_chain_asked;
void sw_chain_next(void) {
    rmutexLock(&s_chain_lock);
    if (!s_chain_asked) {
        s_chain_asked = true;
        hen_chain_next();
    }
    rmutexUnlock(&s_chain_lock);
}
#endif

#if HEN_DEV
// ---- --power-log [SECONDS] (development builds) ----------------------------------------
// One "hen: power:" line per period with the battery's raw charge, the charger, the temperatures
// that answer and the clocks, and the totals at the end: for comparing what settings cost in
// battery. Everything but the request runs on the program's first thread (sw_power_log_poll, in
// its 100 ms loop: no thread of its own), so a service lookup that never returns stops the
// watchdog and the both-sticks exit, not the emulator; a line goes out before each lookup.
// Read-only: nothing here changes charging, the fan or a clock.
// The two services it opens, "psm" and "ts", are known to answer on this system.
static volatile unsigned s_pw_period_ms; // 0: off
static volatile bool s_pw_finish, s_pw_done;
static bool s_pw_open, s_pw_psm, s_pw_ts, s_pw_ts_int, s_pw_ts_ext, s_pw_have_first, s_pw_charger_seen;
static Result s_pw_psm_rc, s_pw_ts_rc, s_pw_ts_int_rc, s_pw_ts_ext_rc;
static TsSession s_pw_ts_int_session, s_pw_ts_ext_session;
static u64 s_pw_start_ms, s_pw_next_ms, s_pw_first_ms, s_pw_last_ms;
static double s_pw_first, s_pw_last;
static unsigned s_pw_samples;

void sw_power_log_request(unsigned seconds) {
    if (seconds == 0) {
        seconds = 30;
    }
    // (the clock controller is opened here, on the caller's thread, the one that sets rates:
    // the first thread only reads the two rates)
    const bool clocks = clock_open();
    printf("hen: power: --power-log: a line every %u s; clock controller %s\n",
           seconds, clocks ? "open" : "not available");
    s_pw_period_ms = seconds * 1000;
}

static void power_say(const char* what) {
    printf("hen: power: %s\n", what);
    fflush(stdout);
}

// Once: every source is opened and tried, and each one's result is in the log.
static void power_open(void) {
    s_pw_open = true;
    s_pw_start_ms = sw_time_ms();
    power_say("looking up psm");
    s_pw_psm_rc = psmInitialize();
    s_pw_psm = R_SUCCEEDED(s_pw_psm_rc);
    double age = 0;
    const Result rc_age = s_pw_psm ? psmGetBatteryAgePercentage(&age) : s_pw_psm_rc;
    printf("hen: power: psm rc 0x%x; battery age %.3f%% (rc 0x%x)\n", s_pw_psm_rc, age, rc_age);
    if (s_pw_psm) {
        // The charge information as the service returns it, whole, once: the layout is the
        // Switch 1's newer one, so what each word is here is for the log to show.
        PsmBatteryChargeInfoFields fields;
        memset(&fields, 0, sizeof(fields));
        bool enough = false;
        const Result rc_fields = psmGetBatteryChargeInfoFields(&fields);
        const Result rc_enough = psmIsEnoughPowerSupplied(&enough);
        printf("hen: power: psm charge information rc 0x%x, %u bytes:", rc_fields, (unsigned)sizeof(fields));
        for (size_t i = 0; i < sizeof(fields); i += 4) {
            u32 word = 0;
            memcpy(&word, (const u8*)&fields + i, sizeof(fields) - i < 4 ? sizeof(fields) - i : 4);
            printf(" %08x", word);
        }
        printf("; enough power supplied %d (rc 0x%x)\n", (int)enough, rc_enough);
    }

    power_say("looking up ts");
    s_pw_ts_rc = tsInitialize();
    s_pw_ts = R_SUCCEEDED(s_pw_ts_rc);
    printf("hen: power: ts rc 0x%x\n", s_pw_ts_rc);
    if (s_pw_ts) {
        // The session calls (the newer of the library's two ways).
        float c = 0;
        s_pw_ts_int_rc = tsOpenSession(&s_pw_ts_int_session, TsDeviceCode_LocationInternal);
        s_pw_ts_int = R_SUCCEEDED(s_pw_ts_int_rc);
        Result rc = s_pw_ts_int ? tsSessionGetTemperature(&s_pw_ts_int_session, &c) : s_pw_ts_int_rc;
        printf("hen: power: ts session Internal: open rc 0x%x, read rc 0x%x, %d milliC\n", s_pw_ts_int_rc, rc,
               (int)(c * 1000.0f));
        c = 0;
        s_pw_ts_ext_rc = tsOpenSession(&s_pw_ts_ext_session, TsDeviceCode_LocationExternal);
        s_pw_ts_ext = R_SUCCEEDED(s_pw_ts_ext_rc);
        rc = s_pw_ts_ext ? tsSessionGetTemperature(&s_pw_ts_ext_session, &c) : s_pw_ts_ext_rc;
        printf("hen: power: ts session External: open rc 0x%x, read rc 0x%x, %d milliC\n", s_pw_ts_ext_rc, rc,
               (int)(c * 1000.0f));
        // The calls by location. The library answers these itself on a newer system
        // (0x4b59: its "wrong system version", nothing is sent).
        for (int location = 0; location < 2; location++) {
            s32 whole = 0, milli = 0;
            const Result rc_whole = tsGetTemperature((TsLocation)location, &whole);
            const Result rc_milli = tsGetTemperatureMilliC((TsLocation)location, &milli);
            printf("hen: power: ts by location %s: tsGetTemperature rc 0x%x (%d C), tsGetTemperatureMilliC rc 0x%x (%d milliC)\n",
                   location == 0 ? "Internal" : "External", rc_whole, (int)whole, rc_milli, (int)milli);
        }
        // The same two commands (1 and 3) sent anyway, on a session of their own that is closed
        // right after: whether the service still has them.
        Service probe;
        power_say("looking up ts again for the two commands by location");
        rc = smGetService(&probe, "ts");
        if (R_FAILED(rc)) {
            printf("hen: power: ts second session rc 0x%x\n", rc);
        } else {
            for (int location = 0; location < 2; location++) {
                const u8 in = (u8)location;
                s32 whole = 0, milli = 0;
                const Result rc_whole = serviceDispatchInOut(&probe, 1, in, whole);
                const Result rc_milli = serviceDispatchInOut(&probe, 3, in, milli);
                printf("hen: power: ts commands sent, %s: command 1 rc 0x%x (%d C), command 3 rc 0x%x (%d milliC)\n",
                       location == 0 ? "Internal" : "External", rc_whole, (int)whole, rc_milli, (int)milli);
            }
            serviceClose(&probe);
        }
    }
}

// (a temperature in milliC, or the result that came instead)
static void power_temp(char* out, size_t size, Result rc, int milli) {
    if (R_SUCCEEDED(rc)) {
        snprintf(out, size, "%d", milli);
    } else {
        snprintf(out, size, "rc 0x%x", rc);
    }
}

static void power_sample(u64 now) {
    double raw = 0;
    u32 percent = 0, state = 0, cpu = 0, gpu = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    PsmBatteryChargeInfoFields fields;
    memset(&fields, 0, sizeof(fields));
    Result rc_raw = s_pw_psm_rc, rc_charger = s_pw_psm_rc, rc_state = s_pw_psm_rc, rc_fields = s_pw_psm_rc;
    if (s_pw_psm) {
        rc_raw = psmGetRawBatteryChargePercentage(&raw);
        psmGetBatteryChargePercentage(&percent);
        rc_charger = psmGetChargerType(&charger);
        rc_state = psmGetBatteryVoltageState((PsmBatteryVoltageState*)&state);
        rc_fields = psmGetBatteryChargeInfoFields(&fields);
    }
    // (the 16 bytes at the end of the charge information that have no name in the header)
    u32 tail[4];
    memcpy(tail, fields.unk_x44, sizeof(tail));
    float c = 0;
    char temp[24], temp_ext[24], battery_temp[24];
    Result rc = s_pw_ts ? s_pw_ts_int_rc : s_pw_ts_rc;
    if (s_pw_ts_int) {
        rc = tsSessionGetTemperature(&s_pw_ts_int_session, &c);
    }
    power_temp(temp, sizeof(temp), rc, (int)(c * 1000.0f));
    c = 0;
    rc = s_pw_ts ? s_pw_ts_ext_rc : s_pw_ts_rc;
    if (s_pw_ts_ext) {
        rc = tsSessionGetTemperature(&s_pw_ts_ext_session, &c);
    }
    power_temp(temp_ext, sizeof(temp_ext), rc, (int)(c * 1000.0f));
    power_temp(battery_temp, sizeof(battery_temp), rc_fields, (int)fields.temperature_celcius);
    if (s_clk_ready) {
        clkrstGetClockRate(&s_clk_cpu, &cpu);
        clkrstGetClockRate(&s_clk_gpu, &gpu);
    }
    // (the first fields as asked for; after "mode": the whole percentage, the battery's voltage
    // and temperature from the charge information, the second sensor)
    printf("hen: power: t=%llu battery %.5f%% (rc 0x%x) charger %d voltage-state %u temp %s cpu %u gpu %u mode %s"
           " percent %u battery-mv %u battery-temp %s temp-ext %s"
           " psm (rc 0x%x) charging %d input-limit %u mA %u mV charge-limit %u mA %u mV usb-role %u usb-charger %u"
           " word-2c %u tail %08x %08x %08x %08x\n",
           (unsigned long long)((now - s_pw_start_ms + 500) / 1000), raw, rc_raw,
           R_SUCCEEDED(rc_charger) ? (int)charger : -1, R_SUCCEEDED(rc_state) ? state : 99u, temp, cpu / 1000000,
           gpu / 1000000, appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
           percent, (unsigned)fields.battery_charge_milli_voltage, battery_temp, temp_ext,
           rc_fields, (int)fields.battery_charging, (unsigned)fields.charger_input_current_limit,
           (unsigned)fields.charger_input_voltage_limit, (unsigned)fields.fast_charge_current_limit,
           (unsigned)fields.charge_voltage_limit, (unsigned)fields.usb_power_role, (unsigned)fields.usb_charger_type,
           (unsigned)fields.unk_x2c, tail[0], tail[1], tail[2], tail[3]);
    s_pw_samples++;
    if (R_SUCCEEDED(rc_charger) && charger != PsmChargerType_Unconnected) {
        s_pw_charger_seen = true;
    }
    if (R_SUCCEEDED(rc_raw)) {
        if (!s_pw_have_first) {
            s_pw_have_first = true;
            s_pw_first = raw;
            s_pw_first_ms = now;
        }
        s_pw_last = raw;
        s_pw_last_ms = now;
    }
}

// The last sample, the totals, and the sessions closed (every program runs in the same
// process: what is left open stays open for the next one).
static void power_end(void) {
    if (s_pw_done) {
        return;
    }
    s_pw_done = true;
    if (!s_pw_open) {
        return;
    }
    power_sample(sw_time_ms());
    const double seconds = (double)(s_pw_last_ms - s_pw_first_ms) / 1000.0;
    const double dropped = s_pw_first - s_pw_last;
    if (!s_pw_have_first) {
        printf("hen: power: totals: no battery reading in %u sample(s) (psm rc 0x%x)\n", s_pw_samples, s_pw_psm_rc);
    } else {
        printf("hen: power: totals: dropped %.5f%% in %.0f s: %.3f %%/hour (%.5f%% -> %.5f%%, %u samples; a charger was %s)\n",
               dropped, seconds, seconds > 0 ? dropped * 3600.0 / seconds : 0.0, s_pw_first, s_pw_last, s_pw_samples,
               s_pw_charger_seen ? "CONNECTED at some sample: not a discharge figure" : "never seen");
    }
    if (s_pw_ts_int) {
        tsSessionClose(&s_pw_ts_int_session);
    }
    if (s_pw_ts_ext) {
        tsSessionClose(&s_pw_ts_ext_session);
    }
    if (s_pw_ts) {
        tsExit();
    }
    if (s_pw_psm) {
        psmExit();
    }
    s_pw_ts_int = s_pw_ts_ext = s_pw_ts = s_pw_psm = false;
    fflush(stdout);
}

// The first thread, every 100 ms.
void sw_power_log_poll(void) {
    const unsigned period = s_pw_period_ms;
    if (period == 0 || s_pw_done) {
        return;
    }
    if (s_pw_finish) {
        power_end();
        return;
    }
    const u64 now = sw_time_ms();
    if (!s_pw_open) {
        power_open();
        s_pw_next_ms = sw_time_ms();
    }
    if (now >= s_pw_next_ms) {
        power_sample(now);
        s_pw_next_ms += period;
        if (s_pw_next_ms <= now) {
            s_pw_next_ms = now + period; // (the thread was held up for longer than a period)
        }
    }
}

// The emulator thread, before "hen: exiting": the first thread prints the totals; a second for
// it to do so.
void sw_power_log_finish(void) {
    if (s_pw_period_ms == 0 || s_pw_done) {
        return;
    }
    s_pw_finish = true;
    for (int waited = 0; waited < 1000 && !s_pw_done; waited += 10) {
        svcSleepThread(10000000);
    }
    if (!s_pw_done) {
        printf("hen: power: the first thread did not print the totals within 1 s\n");
    }
}
#endif

void userAppExit(void) {
#if HEN_DEV
    // (the program ended without its normal exit: this is the first thread, as in the poll)
    if (s_pw_period_ms != 0) {
        power_end();
    }
#endif
    sw_cpu_boost_end();
    sw_cpu_clock_end();
    if (s_clk_ready) {
        // (every program runs in the same process: sessions left open stay open for
        // the next program, and the service only has so many)
        clkrstCloseSession(&s_clk_cpu);
        clkrstCloseSession(&s_clk_gpu);
        clkrstExit();
        s_clk_ready = false;
    }
    printf("hen: exit: runtime exit reached\n");
    fflush(stdout);
    exit_note_mark("runtime exit reached\n");
#if HEN_CHAIN
    if (!s_chain_asked && hen_chain_port() > 0) {
        printf("hen: exit: the program ended without its normal exit: the test library's last call now\n");
        fflush(stdout);
    }
    sw_chain_next(); // (does nothing after a normal exit, or when the test library is not in use)
#endif
    // The log's last lines: closing the sockets at once drops what is still queued (the tail
    // of about one log in 25). The sending side is ended and the computer's
    // close of its side waited for, half a second at most; the descriptor itself stays open.
    if (s_log_socket >= 0) {
        fflush(stdout);
        fflush(stderr);
        if (shutdown(s_log_socket, SHUT_WR) == 0) {
            struct pollfd wait = {.fd = s_log_socket, .events = POLLIN};
            char rest[64];
            for (int n = 0; n < 5 && poll(&wait, 1, 100) >= 0; n++) {
                if ((wait.revents & (POLLIN | POLLHUP | POLLERR)) != 0 && recv(s_log_socket, rest, sizeof(rest), 0) <= 0) {
                    break;
                }
            }
        }
    }
    socketExit();
    romfsExit();
    // (the last thing this program does: an error screen after this line is not in its own code)
    exit_note_mark("sockets and program files closed: this program's own exit is complete\n");
}

// ---- Display rate -------------------------------------------------------------------------
// libounce: nwindowSetFrameRates(window, fps at 120 Hz, fps at 60 Hz, vrr) stores a policy for
// the default window. The Vulkan driver reads the resulting interval at every present
// (nwindowGetPresentInterval) and hands it to the display service
// with the image; the 120 Hz and VRR requests go to the display service the next time the
// interval is read after the window's display exists. So the call can be made before the
// Vulkan surface exists, and it holds until nwindowSetSwapInterval (nobody here calls it).
// 120 Hz is only asked for when the 120 Hz rate is 0, above 60, or does not divide 60.
// The default: an image may change at every refresh of
// a 120 Hz display, 60 a second at a 60 Hz one, variable refresh declined. libounce keeps the
// policy and works the interval out again from the display's present rate at every present
// (nwindowGetPresentInterval: the rate is reread on the display service's refresh-rate event),
// so docking and undocking while a title runs needs nothing here; sw_display_poll (development
// builds) says the new interval when it changes. --no-display-120: no call at all.
static bool s_displaySet = true;
#if HEN_DEV
static u16 s_displayNum, s_displayDen;
static bool s_displaySaid;
#endif

#if HEN_DEV
void sw_display_request_none(void) {
    s_displaySet = false;
}

void sw_display_poll(const char* when) {
    u16 numerator = 0, denominator = 0;
    const Result rc = nwindowGetPresentInterval(nwindowGetDefault(), &numerator, &denominator);
    if (s_displaySaid && numerator == s_displayNum && denominator == s_displayDen) {
        return;
    }
    s_displaySaid = true;
    s_displayNum = numerator;
    s_displayDen = denominator;
    printf("hen: display: present interval %u/%u s%s (rc 0x%x), %s; console %s%s\n", numerator, denominator,
           denominator != 0 && numerator != 0 ? "" : " (?)", rc, when, sw_docked() ? "docked" : "handheld",
           s_displaySet ? "" : "; no display rate asked for (the system's default)");
    fflush(stdout);
}
#endif

void* sw_window(void) {
    NWindow* win = nwindowGetDefault();
    nwindowSetDimensions(win, SW_WIDTH, SW_HEIGHT);
    if (s_displaySet) {
        const Result rc = nwindowSetFrameRates(win, 120, 60, false);
        printf("hen: display: nwindowSetFrameRates(120 at 120 Hz, 60 at 60 Hz, vrr declined): rc 0x%x; console %s%s\n",
               rc,
               sw_docked() ? "docked" : "handheld",
               sw_docked() ? " (a 60 Hz TV: nothing should change)" : "");
#if HEN_DEV
        s_displaySaid = false; // (the interval is said again for this title)
        sw_display_poll("before the first frame (the display's rate is not known yet: 60 Hz assumed)");
    } else {
        // (--no-display-120: said all the same, so that a log shows which rate it got)
        s_displaySaid = false;
        sw_display_poll("before the first frame");
#endif
    }
    return win;
}

void sw_window_size(unsigned* width, unsigned* height) {
    *width = SW_WIDTH;
    *height = SW_HEIGHT;
}

// A line in the log for each system message (HOME pressed, focus lost, exit asked for): a run
// whose log stops after one of these was closed by the user, and one that stops without was
// a crash. Nothing else tells the two apart here.
void hen_room_step(const char* text); // room_link.cpp: also kept on the card with a room in use
static void applet_event(AppletHookType type, void* param) {
    (void)param;
    printf("hen: applet: event %d, focus %d\n", (int)type, (int)appletGetFocusState());
    char text[64];
    snprintf(text, sizeof(text), "applet event %d, focus %d", (int)type, (int)appletGetFocusState());
    hen_room_step(text);
}
#if HEN_DEV
int sw_focus_state(void) {
    return (int)appletGetFocusState();
}
#endif
bool sw_main_loop(void) {
    static AppletHookCookie cookie;
    static bool hooked;
    if (!hooked) {
        hooked = true;
        appletHook(&cookie, applet_event, NULL);
    }
    static int last_focus = -1;
    const int focus = (int)appletGetFocusState();
    if (focus != last_focus) {
        if (last_focus != -1) {
            printf("hen: applet: focus %d -> %d\n", last_focus, focus);
            char text[64];
            snprintf(text, sizeof(text), "applet focus %d -> %d", last_focus, focus);
            hen_room_step(text);
        }
        last_focus = focus;
    }
    const bool go_on = appletMainLoop();
    if (!go_on) {
        printf("hen: applet: the system asked the program to end\n");
    }
    return go_on;
}

int hen_pointer = 1; // (--no-pointer)
static int s_pointerSide;

// The Settings page's pointer rows (title_menu.cpp), said before a title starts. The values
// here are the defaults.
static u64 s_pointerShownNs = 3000000000ull; // how long a pointer stays after its last use
static float s_mouseSpeed = 1.0f;            // Joy-Con mouse: pointer steps per sensor count
static float s_stickTravel = 0.7f;           // stick pointer: seconds for the screen's width at full tilt
static float s_pointerSize = 1.0f;
static int s_pointerStyle;

void sw_pointer_settings(unsigned shown_ms, float mouse_speed, float stick_travel_s, float size, int style) {
    s_pointerShownNs = shown_ms * 1000000ull;
    s_mouseSpeed = mouse_speed;
    s_stickTravel = stick_travel_s;
    s_pointerSize = size;
    s_pointerStyle = style;
}

float sw_pointer_size(void) {
    return s_pointerSize;
}

int sw_pointer_style(void) {
    return s_pointerStyle;
}

// Short clicks of the left stick alone, counted (sw_input_update). A click is acted on when the
// stick comes up again, and only if the right stick's button was not down from 100 ms before
// the press until then: the way into "both sticks" never counts as one, whichever goes first.
static volatile unsigned s_stickClicks;

unsigned sw_stick_clicks(void) {
    return s_stickClicks;
}

// The right stick as a second stylus (--no-stick-pointer): sw_input_update.
// What the stick is comes from the emulation thread (main.cpp, RightStickRole): the C-Stick
// while the title reads one, the pointer otherwise. A new title starts as the C-Stick
// until the first answer, with the pointer back at the middle.
int hen_stick_pointer = 1;
static volatile int s_stickIsCStick = 1;
static volatile int s_stickReset;
void sw_right_stick_role(bool c_stick) {
    s_stickIsCStick = c_stick;
}
void sw_stick_pointer_reset(void) {
    s_stickIsCStick = 1;
    s_stickReset = 1;
}

static bool s_upright, s_flipped;

void sw_set_orientation(bool upright, bool flipped) {
    s_upright = upright;
    s_flipped = upright && flipped;
}

bool sw_flipped(void) {
    return s_flipped;
}

// A sensor reading of the console as the 3DS in the picture feels it: a quarter turn about the
// screen's axis. Upright, the 3DS's right is the console's top edge (y) and its far side the
// console's left (-x); flipped, the opposite.
static void turn_with_picture(float v[3]) {
    const float x = v[0];
    v[0] = s_flipped ? -v[1] : v[1];
    v[1] = s_flipped ? x : -x;
}

void sw_input_update(struct sw_input* out) {
    if (!s_padReady) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&s_pad);
        hidInitializeTouchScreen();
        s_padReady = true;
    }
    padUpdate(&s_pad);
    const u64 held = padGetButtons(&s_pad);
    static const struct {
        u64 nx;
        int bit;
    } map[] = {
        {HidNpadButton_A, SW_BTN_A},         {HidNpadButton_B, SW_BTN_B},
        {HidNpadButton_X, SW_BTN_X},         {HidNpadButton_Y, SW_BTN_Y},
        {HidNpadButton_Up, SW_BTN_UP},       {HidNpadButton_Down, SW_BTN_DOWN},
        {HidNpadButton_Left, SW_BTN_LEFT},   {HidNpadButton_Right, SW_BTN_RIGHT},
        {HidNpadButton_L, SW_BTN_L},         {HidNpadButton_R, SW_BTN_R},
        {HidNpadButton_Plus, SW_BTN_START},  {HidNpadButton_Minus, SW_BTN_SELECT},
        {HidNpadButton_ZL, SW_BTN_ZL},       {HidNpadButton_ZR, SW_BTN_ZR},
    };
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (held & map[i].nx) {
            out->buttons |= 1u << map[i].bit;
        }
    }
    out->menu_combo = (held & HidNpadButton_StickL) && (held & HidNpadButton_StickR);
    for (int i = 0; i < 2; i++) {
        const HidAnalogStickState stick = padGetStickPos(&s_pad, i);
        out->sticks[i][0] = (float)stick.x / JOYSTICK_MAX;
        out->sticks[i][1] = (float)stick.y / JOYSTICK_MAX;
    }
    {
        // L3 only: R3 is the stick pointer's tap.
        static const u64 CLICK_NS = 400000000ull, CLEAR_NS = 100000000ull, AGAIN_NS = 500000000ull;
        static u64 pressed_at, right_at, not_before;
        static bool pressed, spoiled;
        const u64 now = armTicksToNs(armGetSystemTick());
        const bool left = held & HidNpadButton_StickL, right = held & HidNpadButton_StickR;
        if (right) {
            right_at = now;
            spoiled = true;
        }
        if (left && !pressed) {
            // (a press that begins with the stick pushed far over is play, not a click)
            const float x = out->sticks[0][0], y = out->sticks[0][1];
            pressed = true;
            pressed_at = now;
            spoiled = right || (right_at != 0 && now - right_at < CLEAR_NS) || now < not_before || x * x + y * y > 0.7f * 0.7f;
        } else if (!left && pressed) {
            pressed = false;
            if (!spoiled && now - pressed_at <= CLICK_NS) {
                s_stickClicks++;
                not_before = now + AGAIN_NS;
            }
        }
    }
    HidTouchScreenState touch = {0};
    if (hidGetTouchScreenStates(&touch, 1) > 0 && touch.count > 0) {
        // The touch panel reports 1280x720 whatever the window size.
        out->touching = true;
        out->touch_x = touch.touches[0].x * (SW_WIDTH / 1280.0f);
        out->touch_y = touch.touches[0].y * (SW_HEIGHT / 720.0f);
        if (s_flipped) {
            out->touch_x = SW_WIDTH - 1 - out->touch_x;
            out->touch_y = SW_HEIGHT - 1 - out->touch_y;
        }
    }
    // A Joy-Con slid on a surface is the stylus. Its own trigger (ZL or ZR) is the stylus
    // touching, so it is not also the 3DS's ZL/ZR while the pointer is in use; its L or R stays
    // the 3DS's (were L and ZL, or R and ZR, both to touch, both would be
    // taken away). The mouse report cannot tell the two apart (nmouse.h: "By default a Joy-Con's
    // L/ZL or R/ZR give HidMouseButton_Left"), so the touch is read from the pad's own ZL/ZR.
    // (against random jumps of the pointer. The devkit's helper follows
    // whichever Joy-Con last moved, scrolled or changed a button, and each Joy-Con has a
    // position of its own: a touch of the other one's stick moved the pointer to that one's
    // position. Now one Joy-Con is the stylus until the pointer has been still for 3 s, the
    // position is ours (its movement added up), and samples the sensor marks as not tracking,
    // as when it is lifted, are left out.)
    int pointer_side = 0; // 1 left, 2 right: its motion sensor is not the 3DS's meanwhile
    {
        enum { POINTER_W = 1280, POINTER_H = 960 };
        static PadPointer pointer;
        static int state; // 0 not tried, 1 working, -1 refused
        static u64 used_at; // when the stylus last moved or touched; 0: no stylus in use
        const u64 now = armTicksToNs(armGetSystemTick());
        static u32 stylus; // (the handle of the Joy-Con that is the stylus)
        static bool has_stylus;
        static float x = POINTER_W / 2, y = POINTER_H / 2;
        static struct {
            u32 handle;
            u64 seen;
        } last[PAD_POINTER_MAX_DEVICES];
        if (state == 0 && hen_pointer) {
            const Result rc = padPointerInitialize(&pointer, POINTER_W, POINTER_H);
            state = R_SUCCEEDED(rc) ? 1 : -1;
            printf("hen: pointer: Joy-Con mouse %s (rc 0x%x)\n", state > 0 ? "ready" : "refused", rc);
        }
        if (state > 0) {
            padPointerUpdate(&pointer, &s_pad); // (for the list of Joy-Cons)
            if (used_at == 0 || now - used_at >= s_pointerShownNs) {
                has_stylus = false;
                used_at = 0;
            }
            bool moved = false, down = false, present = false;
            for (u32 i = 0; i < pointer.device_count && i < PAD_POINTER_MAX_DEVICES; i++) {
                const HidNmouseHandle handle = pointer.devices[i].handle;
                HidNmouseStateWithDelta states[16];
                size_t total = 0;
                if (R_FAILED(hidGetNmouseStatesWithDelta(handle, states, 16, &total)) || total == 0) {
                    continue;
                }
                const bool known = last[i].handle == handle.handle;
                int dx = 0, dy = 0;
                for (size_t k = 0; known && k < total; k++) {
                    if (states[k].sampling_number <= last[i].seen) {
                        break; // (newest first)
                    }
                    if (states[k].flags & 2) {
                        dx += states[k].delta_x;
                        dy += states[k].delta_y;
                    }
                }
                last[i].handle = handle.handle;
                last[i].seen = states[0].sampling_number;
                if (!has_stylus && (dx || dy)) {
                    has_stylus = true;
                    stylus = handle.handle;
                    u32 types = 0;
                    hidGetNmouseTypeSet(handle, &types);
                    s_pointerSide = types & HidNmouseType_JoyLeft ? 1 : 2;
                    printf("hen: pointer: in use (%s Joy-Con)\n", s_pointerSide == 1 ? "left" : "right");
                }
                if (has_stylus && stylus == handle.handle) {
                    present = true;
                    x += dx * s_mouseSpeed;
                    y += dy * s_mouseSpeed;
                    x = x < 0 ? 0 : x > POINTER_W - 1 ? POINTER_W - 1 : x;
                    y = y < 0 ? 0 : y > POINTER_H - 1 ? POINTER_H - 1 : y;
                    moved = dx || dy;
                    down = held & (s_pointerSide == 1 ? HidNpadButton_ZL : HidNpadButton_ZR);
                }
            }
            if (!present) {
                has_stylus = false;
                used_at = 0;
            } else if (moved || down) {
                used_at = now;
            }
            if (has_stylus && used_at != 0) {
                pointer_side = s_pointerSide;
                out->pointer = true;
                out->pointer_down = down;
                out->pointer_x = (x + 0.5f) / POINTER_W;
                out->pointer_y = (y + 0.5f) / POINTER_H;
                out->buttons &= ~(1u << (pointer_side == 1 ? SW_BTN_ZL : SW_BTN_ZR));
            }
        }
    }
    // The right stick is a second stylus while the title reads no C-Stick: it moves a pointer
    // over the bottom screen, and the stick's click (not a 3DS button) is the stylus touching;
    // held while the stick moves, it drags. The finger and the Joy-Con mouse come first: while
    // either is in use this pointer is hidden and does nothing.
    // Speed, in widths of the bottom screen a second (so the same in every layout and at every
    // picture size): nothing inside the dead zone, then the square of the tilt, up to the whole
    // width in s_stickTravel seconds at full tilt (the height in three quarters of that: the same
    // speed on the picture both ways). A third of the way out is a ninth of full speed.
    {
        static const float STICK_DEAD = 0.15f;
        static float x = 0.5f, y = 0.5f;
        static u64 before, last_used;
        static bool shown, armed;
        const u64 now = armTicksToNs(armGetSystemTick());
        // (a frame that took long does not throw the pointer across the screen)
        const float dt = before == 0 ? 0.0f : now - before > 50000000ull ? 0.05f : (float)(now - before) * 1e-9f;
        before = now;
        if (s_stickReset) {
            s_stickReset = 0;
            x = y = 0.5f;
            shown = false;
        }
        if (!hen_stick_pointer || s_stickIsCStick) {
            shown = false; // (and the stick goes to the 3DS as it is)
        } else {
            float sx = out->sticks[1][0], sy = out->sticks[1][1];
            out->sticks[1][0] = out->sticks[1][1] = 0.0f;
            // (Held upright in handheld mode, the stick turns with the picture, as the motion
            // sensor below; a controller off the console is held the usual way.)
            if (s_upright && (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld)) {
                const float t = sx;
                sx = s_flipped ? -sy : sy;
                sy = s_flipped ? t : -t;
            }
            // (both stick buttons are the way back to the titles, not a touch)
            const bool click = (held & HidNpadButton_StickR) && !(held & HidNpadButton_StickL);
            const float tilt = sqrtf(sx * sx + sy * sy);
            if (out->touching || out->pointer) {
                shown = false;
            } else {
                if (tilt > STICK_DEAD) {
                    float t = (tilt - STICK_DEAD) / (1.0f - STICK_DEAD);
                    t = t > 1.0f ? 1.0f : t;
                    const float step = t * t / s_stickTravel * dt / tilt;
                    x += sx * step;
                    y -= sy * step * (320.0f / 240.0f);
                    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
                    y = y < 0.0f ? 0.0f : y > 1.0f ? 1.0f : y;
                }
                if (tilt > STICK_DEAD || click) {
                    if (!shown) {
                        shown = true;
                        // (the click that brings a hidden pointer back shows where it is; the
                        // touch is the next click)
                        armed = !click;
                    }
                    last_used = now;
                }
                if (!click) {
                    armed = true;
                }
                if (shown && now - last_used > s_pointerShownNs) {
                    shown = false;
                }
                if (shown) {
                    out->pointer = true;
                    out->pointer_down = click && armed;
                    out->pointer_x = x * (1.0f - 1.0f / 320.0f) + 0.5f / 320.0f; // (pixel centres)
                    out->pointer_y = y * (1.0f - 1.0f / 240.0f) + 0.5f / 240.0f;
                }
            }
        }
    }
    // Motion: the sensor of whichever controller is player one.
    static HidSixAxisSensorHandle handles[4];
    static bool started;
    if (!started) {
        started = true;
        hidGetSixAxisSensorHandles(&handles[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        hidGetSixAxisSensorHandles(&handles[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
        hidGetSixAxisSensorHandles(&handles[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
        unsigned ok = 0;
        for (int i = 0; i < 4; i++) {
            ok += R_SUCCEEDED(hidStartSixAxisSensor(handles[i]));
        }
        printf("hen: motion: %u of 4 sensors started\n", ok);
    }
    const u32 handheld = hidGetNpadStyleSet(HidNpadIdType_Handheld), first = hidGetNpadStyleSet(HidNpadIdType_No1);
    int which = -1;
    if (handheld & HidNpadStyleTag_NpadHandheld) {
        which = 0;
    } else if (first & HidNpadStyleTag_NpadFullKey) {
        which = 1;
    } else if (first & HidNpadStyleTag_NpadJoyDual) {
        which = pointer_side == 2 ? 2 : 3; // (the right Joy-Con, or the left while the right is the stylus)
    }
    HidSixAxisSensorState state = {0};
    if (which >= 0 && hidGetSixAxisSensorStates(handles[which], &state, 1) > 0) {
        out->motion = true;
        out->accel[0] = state.acceleration.x;
        out->accel[1] = state.acceleration.y;
        out->accel[2] = state.acceleration.z;
        out->gyro[0] = state.angular_velocity.x;
        out->gyro[1] = state.angular_velocity.y;
        out->gyro[2] = state.angular_velocity.z;
        // (Only the console's own sensor: a controller off the console is held the usual way.)
        if (which == 0 && s_upright) {
            turn_with_picture(out->accel);
            turn_with_picture(out->gyro);
        }
        static int reported = -2;
        if (reported != which) {
            reported = which;
            printf("hen: motion: reading sensor %d: accel %.2f %.2f %.2f\n", which, out->accel[0], out->accel[1], out->accel[2]);
        }
    }
}

uint64_t sw_time_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000;
}

// ---- Hang watchdog -------------------------------------------------------------------------
// The emulator thread beats once per run-loop pass. If the beats stop for a few seconds while
// a title runs, the process's first thread prints where the emulator thread is: addresses
// relative to the start of the program, for addr2line on the .elf.

extern char _start[]; // the first instruction of the program
static Handle s_watchThread;
static volatile u32 s_watchBeats;
static volatile bool s_watchEnabled;
static u32 s_watchSeen;
static u64 s_watchSince;
static bool s_watchReported;

void sw_watch_register(void) {
    s_watchThread = threadGetCurHandle();
}

void sw_watch_enable(bool enabled) {
    s_watchBeats++;
    s_watchEnabled = enabled;
}

void sw_watch_beat(void) {
    s_watchBeats++;
}

void sw_watch_poll(void) {
    svcSleepThread(100000000ull);
    const u64 now = armTicksToNs(armGetSystemTick());
    const u32 beats = s_watchBeats;
    if (!s_watchEnabled || beats != s_watchSeen) {
        s_watchSeen = beats;
        s_watchSince = now;
        s_watchReported = false;
        return;
    }
    if (s_watchReported || now - s_watchSince < 4000000000ull || s_watchThread == 0) {
        return;
    }
    s_watchReported = true;
    const u64 base = (u64)(uintptr_t)_start;
    Result rc = svcSetThreadActivity(s_watchThread, ThreadActivity_Paused);
    ThreadContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    const Result rc2 = R_SUCCEEDED(rc) ? svcGetThreadContext3(&ctx, s_watchThread) : rc;
    // (the same lines to stdout and to hang.txt)
    static char text[2400];
    size_t n = (size_t)snprintf(text, sizeof(text),
                                "hen: watchdog: no run-loop pass for 4 s (pause 0x%x, context 0x%x); program base 0x%lx\n", rc,
                                rc2, (unsigned long)base);
    if (R_SUCCEEDED(rc2)) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "hen: watchdog: pc 0x%lx (+0x%lx) lr 0x%lx (+0x%lx) sp 0x%lx\n",
                              (unsigned long)ctx.pc.x, (unsigned long)(ctx.pc.x - base), (unsigned long)ctx.lr,
                              (unsigned long)(ctx.lr - base), (unsigned long)ctx.sp);
        u64 fp = ctx.fp;
        const u64 low = ctx.sp, high = ctx.sp + 64ull * 1024 * 1024;
        for (int depth = 0; depth < 40 && fp >= low && fp < high && (fp & 7) == 0 && n < sizeof(text) - 64; depth++) {
            const u64* frame = (const u64*)(uintptr_t)fp;
            n += (size_t)snprintf(text + n, sizeof(text) - n, "hen: watchdog: #%d +0x%lx\n", depth,
                                  (unsigned long)(frame[1] - base));
            if (frame[0] <= fp) {
                break;
            }
            fp = frame[0];
        }
    }
    fputs(text, stdout);
    if (R_SUCCEEDED(rc)) {
        svcSetThreadActivity(s_watchThread, ThreadActivity_Runnable);
    }
    fflush(stdout);
    sw_hang_record(text); // (after the thread runs again: writing to the card takes long)
}

bool sw_menu_combo_now(void) {
    u64 held = 0;
    const u32 handheld = hidGetNpadStyleSet(HidNpadIdType_Handheld);
    const u32 first = hidGetNpadStyleSet(HidNpadIdType_No1);
    if (handheld & HidNpadStyleTag_NpadHandheld) {
        HidNpadHandheldState state;
        if (hidGetNpadStatesHandheld(HidNpadIdType_Handheld, &state, 1) > 0) {
            held |= state.buttons;
        }
    }
    if (first & HidNpadStyleTag_NpadFullKey) {
        HidNpadFullKeyState state;
        if (hidGetNpadStatesFullKey(HidNpadIdType_No1, &state, 1) > 0) {
            held |= state.buttons;
        }
    }
    if (first & HidNpadStyleTag_NpadJoyDual) {
        HidNpadJoyDualState state;
        if (hidGetNpadStatesJoyDual(HidNpadIdType_No1, &state, 1) > 0) {
            held |= state.buttons;
        }
    }
    return (held & HidNpadButton_StickL) && (held & HidNpadButton_StickR);
}

// This console's address in the network it is connected to, as text. False without one.
// Asked for again every two seconds at most (the menu wants it for every frame).
bool sw_own_address(char* out, size_t out_size) {
    static u32 s_address;
    static u64 s_askedAt;
    const u64 now = armTicksToNs(armGetSystemTick());
    if (s_askedAt == 0 || now - s_askedAt > 2000000000ull) {
        s_askedAt = now != 0 ? now : 1;
        u32 address = (u32)gethostid(); // (network byte order; the loopback address without a network)
        if (address == 0 || (address & 0xff) == 127) {
            // The address a datagram to the outside would leave from (nothing is sent).
            address = 0;
            const int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd >= 0) {
                struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(9)};
                to.sin_addr.s_addr = htonl(0xc0000201u); // 192.0.2.1, an address kept for examples
                struct sockaddr_in from;
                socklen_t length = sizeof(from);
                if (connect(fd, (const struct sockaddr*)&to, sizeof(to)) == 0 &&
                    getsockname(fd, (struct sockaddr*)&from, &length) == 0) {
                    address = from.sin_addr.s_addr;
                }
                close(fd);
            }
        }
        s_address = address;
    }
    const unsigned char* b = (const unsigned char*)&s_address;
    if (s_address == 0 || b[0] == 127) {
        return false;
    }
    snprintf(out, out_size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return true;
}

bool sw_own_netmask(unsigned char out[4]) {
    if (R_FAILED(nifmInitialize(NifmServiceType_User))) {
        return false;
    }
    u32 address = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
    const Result rc = nifmGetCurrentIpConfigInfo(&address, &mask, &gateway, &dns1, &dns2);
    nifmExit();
    if (R_FAILED(rc) || mask == 0) {
        return false;
    }
    memcpy(out, &mask, 4);
    return true;
}

// ---- System keyboard -----------------------------------------------------------------------

int sw_keyboard(const char* hint, const char* initial, unsigned max_length, char* out, size_t out_size) {
    SwkbdConfig keyboard;
    Result rc = swkbdCreate(&keyboard, 0);
    if (R_FAILED(rc)) {
        printf("hen: keyboard: create failed 0x%x\n", rc);
        return -1;
    }
    swkbdConfigMakePresetDefault(&keyboard);
    if (hint != NULL && hint[0] != 0) {
        swkbdConfigSetGuideText(&keyboard, hint);
    }
    if (initial != NULL && initial[0] != 0) {
        swkbdConfigSetInitialText(&keyboard, initial);
    }
    if (max_length > 0) {
        swkbdConfigSetStringLenMax(&keyboard, max_length);
    }
    out[0] = 0;
    rc = swkbdShow(&keyboard, out, out_size);
    swkbdClose(&keyboard);
    if (R_FAILED(rc)) {
        // (Cancelling also arrives as a failure code.)
        printf("hen: keyboard: closed without text (0x%x)\n", rc);
        return 0;
    }
    return 1;
}

// ---- Heap account (development builds) -------------------------------------------------
// Where the heap is: every allocation call of the program goes through the wrappers below
// (linked with --wrap in development builds only) and is counted by the size the allocator
// gave it, in four classes; blocks of 1 MiB and more are also kept in a table with the address
// they were asked from. The C library's own internal allocations (its _malloc_r) are not seen,
// so a block it made and the program frees counts down without having counted up: small blocks
// only. Cost: one size lookup and two atomic additions a call. Left out of a development build
// configured with HEN_HEAP_ACCOUNT=OFF (CMakeLists.txt), which says so once in the log.
#if HEN_HEAP_ACCOUNT
void* __real_malloc(size_t);
void __real_free(void*);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void* __real_memalign(size_t, size_t);
void* __real_aligned_alloc(size_t, size_t);
int __real_posix_memalign(void**, size_t, size_t);
size_t malloc_usable_size(void*);

enum { HEAP_CLASSES = 4, HEAP_BIG = 768 };
static long long s_heapBytes[HEAP_CLASSES], s_heapCount[HEAP_CLASSES];
static struct {
    void* at;
    size_t size;
    void* caller;
    void* above; // who called the caller (operator new and the like say little by themselves)
} s_heapBig[HEAP_BIG];
static Mutex s_heapLock;
static unsigned s_heapBigLost;

static int heap_class(size_t size) {
    return size < ((size_t)64 << 10) ? 0 : size < ((size_t)1 << 20) ? 1 : size < ((size_t)32 << 20) ? 2 : 3;
}

// The return address one frame further up, by the frame records; NULL if the chain looks wrong.
static inline void* heap_above(void** frame) {
    void** const up = frame != NULL ? (void**)frame[0] : NULL;
    return up != NULL && up > frame && (uintptr_t)up - (uintptr_t)frame < (1u << 20) && ((uintptr_t)up & 15) == 0 ? up[1] : NULL;
}
#define HEAP_FROM __builtin_return_address(0), heap_above((void**)__builtin_frame_address(0))

static void heap_note(void* p, void* caller, void* above) {
    if (p == NULL) {
        return;
    }
    const size_t size = malloc_usable_size(p);
    const int c = heap_class(size);
    __atomic_fetch_add(&s_heapBytes[c], (long long)size, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_heapCount[c], 1, __ATOMIC_RELAXED);
    if (c >= 2) {
        mutexLock(&s_heapLock);
        unsigned i = 0;
        while (i < HEAP_BIG && s_heapBig[i].at != NULL) {
            i++;
        }
        if (i < HEAP_BIG) {
            s_heapBig[i].at = p;
            s_heapBig[i].size = size;
            s_heapBig[i].caller = caller;
            s_heapBig[i].above = above;
        } else {
            s_heapBigLost++;
        }
        mutexUnlock(&s_heapLock);
    }
}

static void heap_forget(void* p) {
    if (p == NULL) {
        return;
    }
    const size_t size = malloc_usable_size(p);
    const int c = heap_class(size);
    __atomic_fetch_sub(&s_heapBytes[c], (long long)size, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&s_heapCount[c], 1, __ATOMIC_RELAXED);
    if (c >= 2) {
        mutexLock(&s_heapLock);
        for (unsigned i = 0; i < HEAP_BIG; i++) {
            if (s_heapBig[i].at == p) {
                s_heapBig[i].at = NULL;
                break;
            }
        }
        mutexUnlock(&s_heapLock);
    }
}

void* __wrap_malloc(size_t size) {
    void* const p = __real_malloc(size);
    heap_note(p, HEAP_FROM);
    return p;
}
void __wrap_free(void* p) {
    heap_forget(p);
    __real_free(p);
}
void* __wrap_calloc(size_t count, size_t size) {
    void* const p = __real_calloc(count, size);
    heap_note(p, HEAP_FROM);
    return p;
}
void* __wrap_realloc(void* old, size_t size) {
    heap_forget(old);
    void* const p = __real_realloc(old, size);
    if (p == NULL && size != 0) {
        heap_note(old, HEAP_FROM); // (the old block stands)
    } else {
        heap_note(p, HEAP_FROM);
    }
    return p;
}
void* __wrap_memalign(size_t alignment, size_t size) {
    void* const p = __real_memalign(alignment, size);
    heap_note(p, HEAP_FROM);
    return p;
}
void* __wrap_aligned_alloc(size_t alignment, size_t size) {
    void* const p = __real_aligned_alloc(alignment, size);
    heap_note(p, HEAP_FROM);
    return p;
}
int __wrap_posix_memalign(void** out, size_t alignment, size_t size) {
    const int rc = __real_posix_memalign(out, alignment, size);
    if (rc == 0) {
        heap_note(*out, HEAP_FROM);
    }
    return rc;
}

void sw_heap_report(void) {
    const unsigned long long total = heapGetTotalSize(), free_now = heapGetFreeSize();
    long long bytes[HEAP_CLASSES], count[HEAP_CLASSES];
    for (int c = 0; c < HEAP_CLASSES; c++) {
        bytes[c] = __atomic_load_n(&s_heapBytes[c], __ATOMIC_RELAXED);
        count[c] = __atomic_load_n(&s_heapCount[c], __ATOMIC_RELAXED);
    }
    // The blocks of 1 MiB and more by who asked for them; what those of 32 MiB and more take
    // of the allocator's 32 MiB units (each with its 64 KiB header, rounded up to whole units).
    static struct {
        void* caller;
        void* above;
        unsigned blocks;
        unsigned long long bytes;
    } owner[96];
    unsigned owners = 0, lost = 0;
    unsigned long long huge_units = 0;
    mutexLock(&s_heapLock);
    for (unsigned i = 0; i < HEAP_BIG; i++) {
        if (s_heapBig[i].at == NULL) {
            continue;
        }
        if (s_heapBig[i].size >= ((size_t)32 << 20)) {
            huge_units += (s_heapBig[i].size + ((size_t)64 << 10) + ((size_t)32 << 20) - 1) >> 25;
        }
        unsigned k = 0;
        while (k < owners && (owner[k].caller != s_heapBig[i].caller || owner[k].above != s_heapBig[i].above)) {
            k++;
        }
        if (k == owners) {
            if (owners == 96) {
                continue;
            }
            owner[owners].caller = s_heapBig[i].caller;
            owner[owners].above = s_heapBig[i].above;
            owner[owners].blocks = 0;
            owner[owners].bytes = 0;
            owners++;
        }
        owner[k].blocks++;
        owner[k].bytes += s_heapBig[i].size;
    }
    lost = s_heapBigLost;
    mutexUnlock(&s_heapLock);
    const long long live = bytes[0] + bytes[1] + bytes[2] + (long long)(huge_units << 25);
    printf("hen: heap: %llu MiB; free by the allocator's count %llu MiB; allocated: blocks of 32 MiB and more %lld, "
           "%.0f MiB, taking %llu MiB of whole 32 MiB units; 1 to 32 MiB: %lld, %.0f MiB; 64 KiB to 1 MiB: %lld, "
           "%.0f MiB; smaller: %lld, %.0f MiB; together %.0f MiB; the rest, %.0f MiB, is the allocator's own (its "
           "pool and tables, page headers, free space inside pages in use) and the C library's%s\n",
           total >> 20, free_now >> 20, count[3], bytes[3] / 1048576.0, (huge_units << 25) >> 20, count[2],
           bytes[2] / 1048576.0, count[1], bytes[1] / 1048576.0, count[0], bytes[0] / 1048576.0, live / 1048576.0,
           ((long long)total - (long long)free_now - live) / 1048576.0, lost != 0 ? "; the table of large blocks ran over" : "");
    // (largest first: a selection sort of at most 96)
    char line[1500];
    size_t n = 0;
    const u64 base = (u64)(uintptr_t)_start;
    for (unsigned shown = 0; shown < 20 && shown < owners; shown++) {
        unsigned best = shown;
        for (unsigned k = shown + 1; k < owners; k++) {
            if (owner[k].bytes > owner[best].bytes) {
                best = k;
            }
        }
        const __typeof__(owner[0]) one = owner[best];
        owner[best] = owner[shown];
        owner[shown] = one;
        n += (size_t)snprintf(line + n, sizeof(line) - n, " +0x%lx < +0x%lx %u x, %.0f MiB;", (unsigned long)((u64)(uintptr_t)one.caller - base),
                              one.above != NULL ? (unsigned long)((u64)(uintptr_t)one.above - base) : 0ul, one.blocks, one.bytes / 1048576.0);
        if (n >= sizeof(line) - 64) {
            break;
        }
    }
    printf("hen: heap: blocks of 1 MiB and more by who asked (%u places; address in the program < its caller's, blocks, MiB):%s\n", owners, line);
}
#elif HEN_DEV
void sw_heap_report(void) {
    static bool said;
    if (!said) {
        said = true;
        printf("hen: heap: account not built\n");
    }
}
#endif

// ---- Crash report ---------------------------------------------------------------------------
// A fault in the emulator itself (not in the emulated program): where it happened, as offsets
// for addr2line on the .elf, printed to the log before the process ends. libnx calls this on
// its own small stack.

alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

// The same on the card, in every build: without it a release build that dies leaves
// nothing about where (the lines below go to stdout, which a release build started from
// the menu does not keep). Written from the handler, so: text put together in a fixed buffer
// by the three small functions below (no printf, nothing allocated), and the file made with the
// file service's own calls on the card's file system found at start (no stdio, no file table of
// the C library). Old record kept as crash.old.txt. Nothing in it names the user or the network.
#define CRASH_FILE "/switch/Kaizhar/log/crash.txt"
#define CRASH_OLD "/switch/Kaizhar/log/crash.old.txt"
const char* hen_thread_name_self_nolock(void); // hen/platform/hen_thread.c
static FsFileSystem* s_crashFs;
static volatile u64 s_crashTitle, s_crashTitleTick;
static volatile int s_crashInside;
volatile int hen_crash_flush;
volatile unsigned hen_crash_seconds;
static char s_crashText[8192];
static size_t s_crashLen;

void sw_crash_prepare(void) {
    if (s_crashFs == NULL) {
        s_crashFs = fsdevGetDeviceFileSystem("sdmc");
    }
}

void sw_crash_title(unsigned long long id) {
    sw_crash_prepare();
    s_crashTitleTick = armGetSystemTick();
    s_crashTitle = id;
}

static void crash_text(const char* s) {
    while (*s != 0 && s_crashLen + 1 < sizeof(s_crashText)) {
        s_crashText[s_crashLen++] = *s++;
    }
}

static void crash_hex(u64 value, int digits) { // digits 0: as many as it takes
    char out[17];
    int n = 0;
    do {
        out[n++] = "0123456789abcdef"[value & 15];
        value >>= 4;
    } while (n < 16 && (value != 0 || n < digits));
    while (n > 0 && s_crashLen + 1 < sizeof(s_crashText)) {
        s_crashText[s_crashLen++] = out[--n];
    }
}

static void crash_dec(u64 value) {
    char out[21];
    int n = 0;
    do {
        out[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n > 0 && s_crashLen + 1 < sizeof(s_crashText)) {
        s_crashText[s_crashLen++] = out[--n];
    }
}

static void crash_record(const ThreadExceptionDump* ctx) {
    const u64 base = (u64)(uintptr_t)_start;
    const u64 title = s_crashTitle;
    const u64 seconds = title != 0 ? armTicksToNs(armGetSystemTick() - s_crashTitleTick) / 1000000000ull : 0;
    hen_crash_seconds = (unsigned)seconds;
    s_crashLen = 0;
    crash_text(SW_VERSION_TEXT " (platform file built " __DATE__ " " __TIME__ ")\n");
    crash_text("(where the program died, for a problem report; nothing here names the user or the network.\n"
               " +offsets are places in the program: they mean something only with this exact build)\n");
    crash_text("title ");
    if (title != 0) {
        crash_hex(title, 16);
        crash_text(", ");
        crash_dec(seconds);
        crash_text(" s after it started");
    } else {
        crash_text("none running");
    }
    crash_text("; console ");
    crash_text(sw_docked() ? "docked" : "handheld");
    crash_text("\nthread: ");
    const char* name = hen_thread_name_self_nolock();
    crash_text(name[0] != 0 ? name : "(no name)");
    crash_text("\nfault kind 0x");
    crash_hex(ctx->error_desc, 0);
    crash_text(", pc +0x");
    crash_hex(ctx->pc.x - base, 0);
    crash_text(" (0x");
    crash_hex(ctx->pc.x, 0);
    crash_text("), lr +0x");
    crash_hex(ctx->lr.x - base, 0);
    crash_text(", fault address 0x");
    crash_hex(ctx->far.x, 0);
    crash_text(", esr 0x");
    crash_hex(ctx->esr, 0);
    crash_text(", sp 0x");
    crash_hex(ctx->sp.x, 0);
    crash_text(", program base 0x");
    crash_hex(base, 0);
    crash_text("\nregisters:");
    for (int r = 0; r < 29; r++) {
        crash_text(r % 4 == 0 ? "\n  x" : "  x");
        crash_dec((u64)r);
        crash_text(" ");
        crash_hex(ctx->cpu_gprs[r].x, 16);
    }
    crash_text("\n  fp ");
    crash_hex(ctx->fp.x, 16);
    crash_text("  pstate 0x");
    crash_hex(ctx->pstate, 0);
    crash_text("\ncalled from:\n");
    u64 fp = ctx->fp.x;
    const u64 low = ctx->sp.x, high = ctx->sp.x + 64ull * 1024 * 1024;
    for (int depth = 0; depth < 40 && fp >= low && fp < high && (fp & 7) == 0; depth++) {
        const u64* frame = (const u64*)(uintptr_t)fp;
        crash_text("  #");
        crash_dec((u64)depth);
        crash_text(" +0x");
        crash_hex(frame[1] - base, 0);
        crash_text("\n");
        if (frame[0] <= fp) {
            break;
        }
        fp = frame[0];
    }
    FsFileSystem* fs = s_crashFs;
    if (fs == NULL) {
        return;
    }
    fsFsDeleteFile(fs, CRASH_OLD);
    fsFsRenameFile(fs, CRASH_FILE, CRASH_OLD);
    fsFsDeleteFile(fs, CRASH_FILE); // (if the old one could not be moved)
    FsFile file;
    if (R_SUCCEEDED(fsFsCreateFile(fs, CRASH_FILE, (s64)s_crashLen, 0)) &&
        R_SUCCEEDED(fsFsOpenFile(fs, CRASH_FILE, FsOpenMode_Write, &file))) {
        fsFileWrite(&file, 0, s_crashText, s_crashLen, FsWriteOption_Flush);
        fsFileClose(&file);
    }
}

// A hang's record on the card, every build: /switch/Kaizhar/log/hang.txt (the one of the
// run before as hang.old.txt). The watchdog below and main.cpp's "hen: hang:" lines print to
// stdout, which a release build started from the menu does not keep. Both run on threads
// that are well, so this is ordinary file writing; each call adds to the run's file.
void sw_hang_record(const char* text) {
    static bool begun;
    FILE* file;
    if (!begun) {
        begun = true;
        remove("/switch/Kaizhar/log/hang.old.txt");
        rename("/switch/Kaizhar/log/hang.txt", "/switch/Kaizhar/log/hang.old.txt");
        file = fopen("/switch/Kaizhar/log/hang.txt", "w");
        if (file != NULL) {
            fputs(SW_VERSION_TEXT "\n(where a title stood when it stopped moving, for a problem report; nothing here names "
                                  "the user or the network. +offsets are places in the program of this exact build)\n",
                  file);
        }
    } else {
        file = fopen("/switch/Kaizhar/log/hang.txt", "a");
    }
    if (file != NULL) {
        const u64 title = s_crashTitle;
        fprintf(file, "\ntitle %016llx, %llu s after it started; console %s\n%s", (unsigned long long)title,
                (unsigned long long)(title != 0 ? armTicksToNs(armGetSystemTick() - s_crashTitleTick) / 1000000000ull : 0),
                sw_docked() ? "docked" : "handheld", text);
        fclose(file);
    }
}

void sw_fatal_record(const char* text) {
    if (__atomic_exchange_n(&s_crashInside, 1, __ATOMIC_SEQ_CST) != 0) {
        return;
    }
    const u64 title = s_crashTitle;
    const u64 seconds = title != 0 ? armTicksToNs(armGetSystemTick() - s_crashTitleTick) / 1000000000ull : 0;
    hen_crash_seconds = (unsigned)seconds;
    s_crashLen = 0;
    crash_text(SW_VERSION_TEXT " (platform file built " __DATE__ " " __TIME__ ")\n");
    crash_text("(why the program ended itself, for a problem report; nothing here names the user or the network)\n");
    crash_text("title ");
    if (title != 0) {
        crash_hex(title, 16);
        crash_text(", ");
        crash_dec(seconds);
        crash_text(" s after it started");
    } else {
        crash_text("none running");
    }
    crash_text("; console ");
    crash_text(sw_docked() ? "docked" : "handheld");
    crash_text("\nthread: ");
    const char* name = hen_thread_name_self_nolock();
    crash_text(name[0] != 0 ? name : "(no name)");
    crash_text("\n");
    crash_text(text);
    crash_text("\n");
    FsFileSystem* fs = s_crashFs;
    if (fs != NULL) {
        fsFsDeleteFile(fs, CRASH_OLD);
        fsFsRenameFile(fs, CRASH_FILE, CRASH_OLD);
        fsFsDeleteFile(fs, CRASH_FILE);
        FsFile file;
        if (R_SUCCEEDED(fsFsCreateFile(fs, CRASH_FILE, (s64)s_crashLen, 0)) &&
            R_SUCCEEDED(fsFsOpenFile(fs, CRASH_FILE, FsOpenMode_Write, &file))) {
            fsFileWrite(&file, 0, s_crashText, s_crashLen, FsWriteOption_Flush);
            fsFileClose(&file);
        }
    }
    hen_crash_flush = 1;
    for (int waited = 0; waited < 100 && hen_crash_flush != 2; waited++) {
        svcSleepThread(10000000ull);
    }
}

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    const u64 base = (u64)(uintptr_t)_start;
    // (the card first: of everything here it is the part that must not be lost. A second fault,
    // in here or on another thread meanwhile, skips it.)
    if (__atomic_exchange_n(&s_crashInside, 1, __ATOMIC_SEQ_CST) == 0) {
        crash_record(ctx);
        // The logs' tails: Azahar's log file sits in a stdio buffer and the session table in
        // memory, both of which another thread may be in the middle of changing, so neither is
        // touched from here. The session report's thread looks at this flag four times a second
        // and writes both; a second at most is waited for it (it may be the thread that died,
        // or wait for a lock the dead thread holds).
        hen_crash_flush = 1;
        for (int waited = 0; waited < 100 && hen_crash_flush != 2; waited++) {
            svcSleepThread(10000000ull);
        }
    }
    printf("hen: crash: kind 0x%x, pc 0x%lx (+0x%lx), lr +0x%lx, fault address 0x%lx, esr 0x%x\n",
           ctx->error_desc, (unsigned long)ctx->pc.x, (unsigned long)(ctx->pc.x - base),
           (unsigned long)(ctx->lr.x - base), (unsigned long)ctx->far.x, ctx->esr);
    u64 fp = ctx->fp.x;
    const u64 low = ctx->sp.x, high = ctx->sp.x + 64ull * 1024 * 1024;
    for (int depth = 0; depth < 40 && fp >= low && fp < high && (fp & 7) == 0; depth++) {
        const u64* frame = (const u64*)(uintptr_t)fp;
        printf("hen: crash: #%d +0x%lx\n", depth, (unsigned long)(frame[1] - base));
        if (frame[0] <= fp) {
            break;
        }
        fp = frame[0];
    }
    fflush(stdout);
    svcSleepThread(200000000ull); // let the log leave before the process ends
}


// Every condition-variable "wake one" becomes "wake all".
// Homebrew programs run one after another in the same process here, and a thread a program
// leaves behind at exit can only be paused (hen_thread.c), not ended: if it was waiting on a
// condition variable, the kernel still has it queued under that variable's address. The next
// program's allocator hands out the same addresses, so a "wake one" for a new variable there
// can go to the paused leftover, and the thread that should have run sleeps for good. Seen
// as the boot hang: the file service's worker never took its first job, and each hung run
// left one more such thread, so every later launch hung too. Waking all is always allowed
// (waits may wake spuriously, every waiter rechecks its condition) and takes the leftovers
// off the kernel's queue; they stay paused.
int __syscall_cond_broadcast(_COND_T* cond);
int __wrap___syscall_cond_signal(_COND_T* cond) {
    return __syscall_cond_broadcast(cond);
}

// Threads left waiting on a condition variable at exit. A thread nobody stops (a worker whose
// owner was never destroyed) would stay queued in the kernel under its variable's address after
// this program is gone, and take the wake-ups of whatever runs next in this process
// (see above). So every wait notes its variable here, and at exit the notes are signalled: each
// thread woken that way ends inside the wait, never returning to this program's code.
#define SW_WAIT_SLOTS 256
static _COND_T* volatile s_wait_slot[SW_WAIT_SLOTS];
static volatile int s_wait_slots_used;
static volatile int s_exit_started;
static __thread int t_wait_slot = -1;
static __thread int t_exit_thread;

void hen_thread_leave(void); // hen_thread.c
int __real___syscall_cond_wait(_COND_T* cond, _LOCK_T* lock, uint64_t timeout_ns);
int __wrap___syscall_cond_wait(_COND_T* cond, _LOCK_T* lock, uint64_t timeout_ns) {
    if (t_wait_slot < 0) {
        const int slot = __atomic_fetch_add(&s_wait_slots_used, 1, __ATOMIC_RELAXED);
        t_wait_slot = slot < SW_WAIT_SLOTS ? slot : SW_WAIT_SLOTS;
    }
    if (s_exit_started && !t_exit_thread) {
        hen_thread_leave();
    }
    if (t_wait_slot < SW_WAIT_SLOTS) {
        s_wait_slot[t_wait_slot] = cond;
    }
    const int rc = __real___syscall_cond_wait(cond, lock, timeout_ns);
    if (t_wait_slot < SW_WAIT_SLOTS) {
        s_wait_slot[t_wait_slot] = NULL;
    }
    if (s_exit_started && !t_exit_thread) {
        hen_thread_leave();
    }
    return rc;
}

static int sw_end_waiting_threads(void) {
    int signalled = 0;
    t_exit_thread = 1;
    s_exit_started = 1;
    const int used = s_wait_slots_used < SW_WAIT_SLOTS ? s_wait_slots_used : SW_WAIT_SLOTS;
    for (int i = 0; i < used; i++) {
        _COND_T* const cond = s_wait_slot[i];
        if (cond != NULL) {
            __syscall_cond_broadcast(cond);
            signalled++;
        }
    }
    return signalled;
}

// While a title runs the console must not dim the screen or go to sleep for lack of controller
// input: an opening movie, a demo or a replayed input recording has none. (A 3DS does not
// sleep while it is open either.) Off again in the title list.
void sw_keep_awake(bool on) {
    const Result media = appletSetMediaPlaybackState(on);
    const Result sleep = appletSetAutoSleepDisabled(on);
    printf("hen: keep awake %s (rc 0x%x, 0x%x)\n", on ? "on" : "off", media, sleep);
}
