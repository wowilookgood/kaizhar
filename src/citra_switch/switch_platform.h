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
    /// A Joy-Con used as a mouse (slid on a surface): the stylus of the bottom screen.
    /// pointer: it moved or clicked in the last few seconds; x and y in [0, 1] of the bottom
    /// screen; pointer_down: its L/ZL or R/ZR is held (the stylus touches).
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
void sw_cpu_boost(int mode);
void sw_cpu_boost_end(void);
void* sw_window(void);
void sw_window_size(unsigned* width, unsigned* height);

/// False once the system asks the app to close.
bool sw_main_loop(void);

/// How the console is held, from the screen layout. upright: a portrait layout, the picture's
/// top at the display's left edge. flipped: that picture turned half a turn, its top at the
/// right edge: the renderer turns what it draws, sw_input_update the touch panel's points.
void sw_set_orientation(bool upright, bool flipped);
bool sw_flipped(void);

/// Reads the controller and touch screen.
void sw_input_update(struct sw_input* out);

/// Milliseconds since start-up.
uint64_t sw_time_ms(void);

/// Hang watchdog: the emulator thread registers itself and beats once per run-loop pass while
/// enabled; the first thread polls and prints the emulator thread's call stack if beats stop.
void sw_watch_register(void);
void sw_watch_enable(bool enabled);
void sw_watch_beat(void);
void sw_watch_poll(void); ///< sleeps 100 ms, then checks

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
void sw_cpu_clock_end(void);

/// The microphone (switch_mic.c, for audio_core/switch_input.cpp). At exit: its thread ended
/// and the device closed. --mic-probe (development builds): lists the input devices, records
/// a second from the default one and half a second from each listed one, and prints what the
/// service answered and the levels.
void sw_mic_shutdown(void);
#if HEN_DEV
void sw_mic_probe(void);
#endif

#ifdef __cplusplus
}
#endif
