// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The title picker's screen and input (switch_menu.c), behind a C interface as
// switch_platform.h is. Only while no title runs: the display then belongs to
// the emulator's renderer.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SW_MENU_NONE,
    SW_MENU_SELECT,  ///< A
    SW_MENU_OPTION,  ///< X
    SW_MENU_REFRESH, ///< Y
    SW_MENU_EXIT,    ///< +, or the system asked the app to close
};

struct sw_menu_item {
    const char* title;
    const char* detail; ///< shown smaller at the right; may be null
};

/// Takes the display for the menu. False if it couldn't.
bool sw_menu_open(void);
/// Gives the display back (before a title starts).
void sw_menu_close(void);

/// Portrait: the menu is drawn for a console held with its left side up (flipped: its right
/// side), and left and right move the selection.
void sw_menu_set_portrait(bool portrait, bool flipped);

/// Draws the list and reads the controller, once per display frame. Up and down move *selected.
/// @returns SW_MENU_*
int sw_menu_frame(const char* title, const struct sw_menu_item* items, int count, int* selected,
                  const char* option_line, const char* hint_line); ///< hint_line may hold several lines

/// Draws a message, with a progress bar if progress is 0..1 (none if negative).
/// @returns false once the system asks the app to close
bool sw_menu_message(const char* title, const char* line1, const char* line2, float progress);

/// Waits for A or B. False once the system asks the app to close.
bool sw_menu_wait_button(void);

#ifdef __cplusplus
}
#endif
