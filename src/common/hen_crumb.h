// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// Breadcrumbs for finding crashes on Switch 2 homebrew, where a crash leaves no report:
// HEN_CRUMB("where") prints its argument at once when the frontend was started with --crumbs
// (hen/vktrace/hen_crumb.c). The last line printed is the last place reached.
// Development builds only (HEN_DEV).
#if defined(__SWITCH__) && HEN_DEV
extern "C" void hen_crumb(const char* where);
#define HEN_CRUMB(where) hen_crumb(where)
#else
#define HEN_CRUMB(where) static_cast<void>(0)
#endif
