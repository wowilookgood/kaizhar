// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// See switch_platform.h. Also the app's start-up and exit hooks: SD card POSIX
// fixes (hen_fs.c), sockets and the nxlink log.
#include <switch.h>

#include <errno.h>
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

// hen_thread.c: unmaps the stacks of threads nobody joined, so the next app the
// HEN loader runs in this process (hbmenu) gets the whole heap back.
int hen_thread_release_stacks(void* (**funcs)(void*), int max, size_t* bytes);
int hen_thread_live(void* (**funcs)(void*), void** args, int max);
unsigned hen_jit_mappings_in_use(void); // hen/jit/hen_jit.c
unsigned hen_jit_release_all(void);

// (The lines printed here and in userAppExit mark how far a normal exit got: an error screen
// at exit with all of them in the log happened after this program's own code.)
int hen_thread_stop_live(int wait_ms); // hen_thread.c
static int sw_end_waiting_threads(void);
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
        const int woken = sw_end_waiting_threads();
        for (int waited = 0; waited < 500 && hen_thread_live(waiting, waiting_args, 16) != 0; waited += 10) {
            svcSleepThread(10000000);
        }
        svcSleepThread(20000000); // (a thread that has just left the list is on its last instructions)
        printf("hen: exit: %d thread(s) were still running; %d condition variable(s) signalled, %d thread(s) left\n",
               running, woken, hen_thread_live(waiting, waiting_args, 16));
    }
    const int stopped = hen_thread_stop_live(300);
    if (stopped != 0) {
        printf("hen: exit: %d thread(s) were still running and have been paused, their stacks unmapped\n", stopped);
    }
    const int count = hen_thread_release_stacks(funcs, 8, &bytes);
    printf("hen: exit: %d thread(s) nobody joined, %zu KiB of stacks released", count, bytes >> 10);
    for (int i = 0; i < count && i < 8; i++) {
        printf("%s+0x%lx", i == 0 ? ": " : ", ", (unsigned long)((uintptr_t)funcs[i] - (uintptr_t)_start));
    }
    printf("\n");
    // Threads still running: they keep running this program's code after it has exited.
    void* (*live[16])(void*);
    void* args[16];
    const int alive = hen_thread_live(live, args, 16);
    const unsigned left = hen_jit_mappings_in_use();
    const unsigned released = hen_jit_release_all();
    printf("hen: exit: %u code mapping(s) left, %u released now; %d thread(s) still running", left, released, alive);
    for (int i = 0; i < alive && i < 16; i++) {
        printf("%s+0x%lx", i == 0 ? ": " : ", ", (unsigned long)((uintptr_t)live[i] - (uintptr_t)_start));
        // (a std::thread's argument is its state object: the vtable names what it runs)
        if (args[i] != NULL && ((uintptr_t)args[i] & 7) == 0) {
            printf(" (arg vtable +0x%lx)", (unsigned long)(*(uintptr_t*)args[i] - (uintptr_t)_start));
        }
    }
    printf("\n");
    fflush(stdout);
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

void userAppInit(void) {
    atexit(release_thread_stacks); // registered first: runs last
    const int fs = hen_fs_posix_sdmc();
    const char* const folder_note = migrate_user_folder();
    const Result romfs = romfsInit();
    const Result sockets = socketInitializeDefault();
#if HEN_CHAIN
    // --director PORT (also --hen-chain PORT), chained test runs: hen-chain, a library
    // outside this repository (HEN_CHAIN_DIR in CMakeLists.txt here). This takes its arguments
    // out of argv, the log host of a chained start among them (the runtime only takes that from
    // the netloader), and leaves the note of this start in /switch/Kaizhar/last-start.txt. The
    // library prints its own lines from a constructor: after this function, before main.
    char chain_note[32];
    snprintf(chain_note, sizeof(chain_note), "sockets 0x%x", sockets);
    const struct hen_chain_options chain = {.dir = "sdmc:/switch/Kaizhar", .note_extra = chain_note};
    hen_chain_init(&chain);
#endif
    if (R_SUCCEEDED(sockets) && __nxlink_host.s_addr != 0) {
        nxlinkStdio();
        setvbuf(stdout, NULL, _IOLBF, 0);
    }
    printf("hen: starting%s%s; romfs 0x%x, sockets 0x%x, sdmc POSIX fixes %s\n",
           __system_argc > 0 ? " " : "", __system_argc > 0 ? __system_argv[0] : "", romfs, sockets,
           fs == 0 ? "on" : "FAILED");
    if (folder_note != NULL) {
        printf("%s\n", folder_note);
    }
}

// --cpu-boost N (experiment): the system's CPU boost mode, the one games ask for while they
// load. Says what the calls returned and what the performance configuration is before and
// after; set back to normal at exit, because the loader's next program runs in this process.
static int s_cpu_boost = 0;
extern int hen_boost_cancel; // hen/jit/hen_jit.c
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

// Called by main before "hen: exiting" (the runtime's exit hook is too late for the log, and
// was not seen to run on this loader).
void sw_cpu_boost_end(void) {
    if (s_cpu_boost != 0) {
        // (hen_boost_cancel, hen/jit/hen_jit.c: the cancel command is what clears the request)
        const Result rc = hen_boost_cancel ? appletCancelCpuBoostMode()
                                           : appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        printf("hen: exit: cpu boost back to normal (%s), rc 0x%x\n",
               hen_boost_cancel ? "cancel" : "set normal", rc);
        s_cpu_boost = 0;
    }
}

// The loading boost's CPU clock (hen/jit/hen_jit.c, docked only) and --clock-info: the clock
// controller service. The CPU rate can be set directly,
// without the system's boost mode, which raises the CPU but lowers the GPU. Running the CPU
// faster with the GPU at full speed is outside what the system's own software does (more heat,
// more power), so: only docked, only up to the rate asked for and never above the range the
// service reports, the starting rate restored at exit, and every step in the log.
extern int hen_n3ds_mode, hen_n3ds_handheld; // hen/jit/hen_jit.c
static ClkrstSession s_clk_cpu, s_clk_gpu;
static bool s_clk_ready;
static u32 s_clk_cpu_start, s_clk_cpu_set;
static bool clock_open(void) {
    if (s_clk_ready) {
        return true;
    }
    // (a line before the first call: a launch on 2026-11-12 stopped without a word about
    // where this would first have run)
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
    // (in handheld only for a New 3DS title in its fast mode: hen_n3ds_handheld, hen_jit.c)
    if (appletGetOperationMode() != AppletOperationMode_Console && !(hen_n3ds_mode && hen_n3ds_handheld == 1)) {
        if (verbose) {
            printf("hen: clock: not docked: the CPU rate is left alone\n");
        }
        return 0;
    }
    u32 now = 0, rates[2] = {0, 0};
    PcvClockRatesListType type = PcvClockRatesListType_Invalid;
    s32 count = 0;
    if (R_FAILED(clkrstGetClockRate(&s_clk_cpu, &now)) ||
        R_FAILED(clkrstGetPossibleClockRates(&s_clk_cpu, rates, 2, &type, &count)) ||
        type != PcvClockRatesListType_Range || count != 2 || hz < rates[0] || hz > rates[1]) {
        printf("hen: clock: %u Hz is not in the range the service offers (%u to %u): left alone\n", hz, rates[0], rates[1]);
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
    return R_SUCCEEDED(rc) && after == hz;
}
int sw_cpu_clock_quiet(unsigned hz) {
    return cpu_clock_set(hz, false);
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
        s_clk_cpu_set = 0;
    }
}

#if HEN_CHAIN
// hen-chain's exit step (--director PORT), once: at the end of main, or from userAppExit when
// the program ended some other way (abort() after a failed start, an uncaught exception), so
// that a failed run still hands over to the next queued one instead of leaving the console in
// hbmenu. A second thread arriving waits for the first to finish asking (and the same thread
// arriving again, should the step itself fail, passes).
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

void userAppExit(void) {
    sw_cpu_boost_end();
    sw_cpu_clock_end();
    if (s_clk_ready) {
        // (every program runs in the loader's one process: sessions left open stay open for
        // the next program, and the service only has so many)
        clkrstCloseSession(&s_clk_cpu);
        clkrstCloseSession(&s_clk_gpu);
        clkrstExit();
        s_clk_ready = false;
    }
    printf("hen: exit: runtime exit reached\n");
    fflush(stdout);
#if HEN_CHAIN
    if (!s_chain_asked && hen_chain_port() > 0) {
        printf("hen: exit: the program ended without its normal exit: asking for the next run now\n");
        fflush(stdout);
    }
    sw_chain_next(); // (does nothing after a normal exit, or in a run without --director)
#endif
    socketExit();
    romfsExit();
}

void* sw_window(void) {
    NWindow* win = nwindowGetDefault();
    nwindowSetDimensions(win, SW_WIDTH, SW_HEIGHT);
    return win;
}

void sw_window_size(unsigned* width, unsigned* height) {
    *width = SW_WIDTH;
    *height = SW_HEIGHT;
}

// A line in the log for each system message (HOME pressed, focus lost, exit asked for): a run
// whose log stops after one of these was closed by the user, and one that stops without was
// a crash. Nothing else tells the two apart here.
static void applet_event(AppletHookType type, void* param) {
    (void)param;
    printf("hen: applet: event %d, focus %d\n", (int)type, (int)appletGetFocusState());
}
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
    // A Joy-Con slid on a surface is the stylus. Its own shoulder buttons are the stylus
    // touching, so they are not also the 3DS's L/R while the pointer is in use.
    // (2026-11-13, after random jumps in the first version. The devkit's helper follows
    // whichever Joy-Con last moved, scrolled or changed a button, and each Joy-Con has a
    // position of its own: a touch of the other one's stick moved the pointer to that one's
    // position. Now one Joy-Con is the stylus until the pointer has been still for 3 s, the
    // position is ours (its movement added up), and samples the sensor marks as not tracking,
    // as when it is lifted, are left out.)
    int pointer_side = 0; // 1 left, 2 right: its motion sensor is not the 3DS's meanwhile
    {
        enum { POINTER_W = 1280, POINTER_H = 960, POINTER_SHOWN = 180 /* updates, 3 s */ };
        static PadPointer pointer;
        static int state; // 0 not tried, 1 working, -1 refused
        static unsigned quiet = POINTER_SHOWN;
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
            if (quiet >= POINTER_SHOWN) {
                has_stylus = false;
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
                    x += dx;
                    y += dy;
                    x = x < 0 ? 0 : x > POINTER_W - 1 ? POINTER_W - 1 : x;
                    y = y < 0 ? 0 : y > POINTER_H - 1 ? POINTER_H - 1 : y;
                    moved = dx || dy;
                    down = states[0].buttons & HidMouseButton_Left;
                }
            }
            if (!present) {
                has_stylus = false;
                quiet = POINTER_SHOWN;
            } else if (moved || down) {
                quiet = 0;
            } else if (quiet < POINTER_SHOWN) {
                quiet++;
            }
            if (has_stylus && quiet < POINTER_SHOWN) {
                pointer_side = s_pointerSide;
                out->pointer = true;
                out->pointer_down = down;
                out->pointer_x = (x + 0.5f) / POINTER_W;
                out->pointer_y = (y + 0.5f) / POINTER_H;
                out->buttons &= pointer_side == 1 ? ~((1u << SW_BTN_L) | (1u << SW_BTN_ZL))
                                                  : ~((1u << SW_BTN_R) | (1u << SW_BTN_ZR));
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
    printf("hen: watchdog: no run-loop pass for 4 s (pause 0x%x, context 0x%x); program base 0x%lx\n",
           rc, rc2, (unsigned long)base);
    if (R_SUCCEEDED(rc2)) {
        printf("hen: watchdog: pc 0x%lx (+0x%lx) lr 0x%lx (+0x%lx) sp 0x%lx\n", (unsigned long)ctx.pc.x,
               (unsigned long)(ctx.pc.x - base), (unsigned long)ctx.lr, (unsigned long)(ctx.lr - base),
               (unsigned long)ctx.sp);
        u64 fp = ctx.fp;
        const u64 low = ctx.sp, high = ctx.sp + 64ull * 1024 * 1024;
        for (int depth = 0; depth < 40 && fp >= low && fp < high && (fp & 7) == 0; depth++) {
            const u64* frame = (const u64*)(uintptr_t)fp;
            printf("hen: watchdog: #%d +0x%lx\n", depth, (unsigned long)(frame[1] - base));
            if (frame[0] <= fp) {
                break;
            }
            fp = frame[0];
        }
    }
    if (R_SUCCEEDED(rc)) {
        svcSetThreadActivity(s_watchThread, ThreadActivity_Runnable);
    }
    fflush(stdout);
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

// ---- Crash report ---------------------------------------------------------------------------
// A fault in the emulator itself (not in the emulated program): where it happened, as offsets
// for addr2line on the .elf, printed to the log before the process ends. libnx calls this on
// its own small stack.

alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    const u64 base = (u64)(uintptr_t)_start;
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


// Every condition-variable "wake one" becomes "wake all" (--no-wake-all turns this off).
// The HEN loader runs one program after another in the same process, and a thread a program
// leaves behind at exit can only be paused (hen_thread.c), not ended: if it was waiting on a
// condition variable, the kernel still has it queued under that variable's address. The next
// program's allocator hands out the same addresses, so a "wake one" for a new variable there
// can go to the paused leftover, and the thread that should have run sleeps for good. Seen
// as the boot hang: the file service's worker never took its first job, and each hung run
// left one more such thread, so every later launch hung too. Waking all is always allowed
// (waits may wake spuriously, every waiter rechecks its condition) and takes the leftovers
// off the kernel's queue; they stay paused.
int hen_cond_wake_all = 1;
int __real___syscall_cond_signal(_COND_T* cond);
int __syscall_cond_broadcast(_COND_T* cond);
int __wrap___syscall_cond_signal(_COND_T* cond) {
    return hen_cond_wake_all ? __syscall_cond_broadcast(cond) : __real___syscall_cond_signal(cond);
}

// Threads left waiting on a condition variable at exit. A thread nobody stops (a worker whose
// owner was never destroyed) would stay queued in the kernel under its variable's address after
// this program is gone, and take the wake-ups of whatever the loader runs next in this process
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
