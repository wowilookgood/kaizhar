// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The libnx side of the Switch 2 frontend, behind a C interface: libnx's
// headers define names that clash with the core's (Result, for one), so no
// C++ file of the frontend includes <switch.h>.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Button bits of sw_input_update(), in the order the 3DS has them.
enum {
    SW_BTN_A,
    SW_BTN_B,
    SW_BTN_X,
    SW_BTN_Y,
    SW_BTN_UP,
    SW_BTN_DOWN,
    SW_BTN_LEFT,
    SW_BTN_RIGHT,
    SW_BTN_L,
    SW_BTN_R,
    SW_BTN_START,
    SW_BTN_SELECT,
    SW_BTN_ZL,
    SW_BTN_ZR,
    SW_BTN_COUNT,
};

struct sw_input {
    uint32_t buttons;      ///< 1 << SW_BTN_*
    float sticks[2][2];    ///< left and right stick, x and y in [-1, 1], up positive
    bool menu_combo;       ///< both stick buttons held: back to the title picker
    bool touching;
    float touch_x, touch_y; ///< in window pixels (sw_window_size), of the picture before
                            ///< sw_flipped() turns it
    /// A Joy-Con used as a mouse (slid on a surface), or the right stick while the title reads
    /// no C-Stick: the stylus of the bottom screen.
    /// pointer: it moved or clicked in the last few seconds; x and y in [0, 1] of the bottom
    /// screen; pointer_down: the Joy-Con's ZL or ZR, or the stick's click, is held (the stylus
    /// touches). While the stick is the pointer, sticks[1] is zero.
    bool pointer, pointer_down;
    float pointer_x, pointer_y;
    /// The motion sensor of the controller in use (the console's own in handheld mode).
    /// Switch axes: x to the right, y away from the player, z out of the screen;
    /// accel in g (flat on a table: 0, 0, -1), gyro in turns per second. In a portrait layout
    /// the console's own sensor is turned with the picture (sw_set_orientation).
    bool motion;
    float accel[3], gyro[3];
};

/// The display window (an NWindow*), set to 1920x1080.
#if HEN_DEV
void sw_cpu_boost(int mode);
#endif
void sw_cpu_boost_end(void);
void* sw_window(void);
void sw_window_size(unsigned* width, unsigned* height);

/// False once the system asks the app to close.
/// "Kaizhar 2126.2-kaizhar3", with " dev" after it in a development build (KAIZHAR_VERSION:
/// CMakeLists.txt).
#if HEN_DEV
#define SW_VERSION_TEXT "Kaizhar " KAIZHAR_VERSION " dev"
#else
#define SW_VERSION_TEXT "Kaizhar " KAIZHAR_VERSION
#endif

bool sw_main_loop(void);
bool sw_docked(void); ///< in the dock (asks the system)
/// The window's frame rates for a 120 Hz and a 60 Hz display
/// and whether variable refresh is allowed (nwindowSetFrameRates). Applied by sw_window.
/// Asked for by default: 120 at a 120 Hz display, 60 at a 60 Hz one, variable refresh
/// declined. sw_display_request_none (--no-display-120): the
/// call is not made: the system's default (a 1/60 s interval at every refresh rate).
#if HEN_DEV
void sw_display_request_none(void);
/// (--power-log [SECONDS], development builds) A "hen: power:" line per period: the battery's
/// raw charge, charger, temperatures, clocks; totals (percent per hour) at the end. request:
/// the emulator thread, at start (seconds 0: 30). poll: the first thread's 100 ms loop, where
/// the services are opened, read and closed. finish: the emulator thread, before "hen: exiting".
void sw_power_log_request(unsigned seconds);
void sw_power_log_poll(void);
void sw_power_log_finish(void);
/// (development builds) The interval the window's images are presented with, read back now; a
/// line in the log the first time and whenever it differs from the last one said. when: for the line.
void sw_display_poll(const char* when);
#endif
/// The clock controller's state and the last calls to it, for the session report
/// (switch_platform.c says what each of the nine values is).
void sw_clock_record(unsigned* out);
#if HEN_DEV
int sw_focus_state(void); ///< the system's AppletFocusState, as a number for the log
#endif

/// How the console is held, from the screen layout. upright: a portrait layout, the picture's
/// top at the display's left edge. flipped: that picture turned half a turn, its top at the
/// right edge: the renderer turns what it draws, sw_input_update the touch panel's points.
void sw_set_orientation(bool upright, bool flipped);
bool sw_flipped(void);

/// Reads the controller and touch screen.
void sw_input_update(struct sw_input* out);

/// How many short clicks of the left stick alone there have been (counted by sw_input_update: a
/// click counts when the stick comes up within 0.4 s, the right stick's button not having been
/// down meanwhile or just before). The emulation thread acts on a change (main.cpp).
unsigned sw_stick_clicks(void);

/// The pointer rows of the Settings page, for the title about to start: how long a pointer
/// stays after its last use, the Joy-Con mouse's speed factor, the seconds the stick pointer
/// takes for the bottom screen's width, and the drawn pointer's size factor and style
/// (renderer_vulkan.cpp, DrawCursor: 0 is the cross that inverts the picture under it).
void sw_pointer_settings(unsigned shown_ms, float mouse_speed, float stick_travel_s, float size, int style);
float sw_pointer_size(void);
int sw_pointer_style(void);

/// What the right stick is: the 3DS's C-Stick (true), or the pointer of the bottom screen with
/// the stick's click as the touch (false). Said by the emulation thread as the title runs
/// (main.cpp, RightStickRole). sw_stick_pointer_reset: a new title (the C-Stick until the first
/// answer; the pointer back at the middle of the bottom screen).
void sw_right_stick_role(bool c_stick);
void sw_stick_pointer_reset(void);

/// Milliseconds since start-up.
uint64_t sw_time_ms(void);

/// The crash record on the card, every build: /switch/Kaizhar/log/crash.txt (the one
/// before it kept as crash.old.txt), written by the fault handler itself with the file
/// service's own calls: nothing allocated, no stdio. prepare: once at start (finds the card's
/// file system). title: a title starts (its ID, for the record and its "seconds since") or has
/// ended (0). hen_crash_flush: the handler sets 1 and waits up to a second for the session
/// report's thread to put the logs' tails on the card and set 2 (session_report.cpp).
void sw_crash_prepare(void);
/// Adds text to the run's /switch/Kaizhar/log/hang.txt (the first call of a run keeps the
/// old file as hang.old.txt): what the hang watchdog and the "hen: hang:" lines print.
void sw_hang_record(const char* text);
/// (development builds) Two "hen: heap:" lines: what is allocated by size of block against
/// the heap's size and the allocator's free count, and who holds the blocks of 1 MiB and more
/// (+offsets in the program).
#if HEN_DEV
void sw_heap_report(void);
#endif
/// The program is about to end itself for the reason given (out of memory for a code
/// buffer): the reason goes to crash.txt as a fault would, and the session table gets its
/// "ended in a crash" line. From an ordinary thread, before abort().
void sw_fatal_record(const char* text);
void sw_crash_title(unsigned long long id);
extern volatile int hen_crash_flush;
/// Seconds since the title started, when the fault handler ran (session_report.cpp's line).
extern volatile unsigned hen_crash_seconds;

/// Hang watchdog: the emulator thread registers itself and beats once per run-loop pass while
/// enabled; the first thread polls and prints the emulator thread's call stack if beats stop.
void sw_watch_register(void);
void sw_watch_enable(bool enabled);
void sw_watch_beat(void);
void sw_watch_poll(void); ///< sleeps 100 ms, then checks
/// Both stick buttons held right now, read from the system's pad state itself: from any thread,
/// and whether or not the emulated console shows frames (sw_input_update is only called when
/// one is shown). The way out of a title must not depend on the title.
bool sw_menu_combo_now(void);

/// Shows the system's keyboard and waits. @returns 1 with the text in out, 0 if it closed
/// without text (cancelled, or it couldn't be shown), -1 if it couldn't be created.
int sw_keyboard(const char* hint, const char* initial, unsigned max_length, char* out, size_t out_size);

/// No screen dimming or automatic sleep while a title runs.
void sw_keep_awake(bool on);

/// The clock controller: a line with the CPU and GPU rates, what the service offers, and the
/// temperature (--clock-info, development builds); setting the CPU rate for the loading boost
/// (docked, within the offered range; 1 if it took); and putting the starting rate back.
#if HEN_DEV
void sw_clock_report(const char* when);
#endif
int sw_cpu_clock_quiet(unsigned hz);
int sw_cpu_clock_step(unsigned hz, unsigned* out); // (--n3ds-clock-high: switch_platform.c)
void sw_cpu_clock_end(void);

/// The microphone (switch_mic.c, for audio_core/switch_input.cpp). At exit: its thread ended
/// and the device closed. --mic-probe (development builds): lists the input devices, records
/// a second from the default one and half a second from each listed one, and prints what the
/// service answered and the levels.
void sw_mic_shutdown(void);
#if HEN_DEV
void sw_mic_probe(void);
#endif

/// The datagrams of a multiplayer room (switch_room_io.c, for room_link.cpp).
/// out: sent, bytes sent, received, bytes received, sends failed, receives failed.
void sw_room_io_stats(unsigned long long out[6]);
void sw_room_io_reset(void);
/// This console's address in the network it is connected to ("192.168.1.30"). False without one.
bool sw_own_address(char* out, size_t out_size);
/// The network's mask as four bytes in the address's order (255.255.255.0: ff ff ff 00). False
/// if the system doesn't say.
bool sw_own_netmask(unsigned char out[4]);

#ifdef __cplusplus
}
#endif
