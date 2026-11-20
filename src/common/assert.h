// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdlib>
#include "common/common_funcs.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"

// (Kaizhar) On the Switch 2 a program that dies where it stands can keep the next one from
// starting, so the port's frontend gets a say first: on the thread that runs the emulated
// console it leaves the run loop and ends the title in order (citra_switch/main.cpp) and does
// not come back here; on any other thread, or with no frontend listening, it returns and the
// assertion ends the program as everywhere else.
#ifdef __SWITCH__
namespace Common {
inline void (*hen_assert_hook)() = nullptr;
}
#define HEN_ASSERT_HOOK()                                                                          \
    do {                                                                                           \
        if (Common::hen_assert_hook != nullptr) {                                                  \
            Common::hen_assert_hook();                                                             \
        }                                                                                          \
    } while (0)
#else
#define HEN_ASSERT_HOOK()
#endif

// For asserts we'd like to keep all the junk executed when an assert happens away from the
// important code in the function. One way of doing this is to put all the relevant code inside a
// lambda and force the compiler to not inline it.

#define ASSERT(_a_)                                                                                \
    do                                                                                             \
        if (!(_a_)) [[unlikely]] {                                                                 \
            []() CITRA_NO_INLINE CITRA_NO_RETURN {                                                 \
                LOG_CRITICAL(Debug, "Assertion Failed!");                                          \
                HEN_ASSERT_HOOK();                                                                 \
                Common::Log::Stop();                                                               \
                Crash();                                                                           \
                exit(1);                                                                           \
            }();                                                                                   \
        }                                                                                          \
    while (0)

#define ASSERT_MSG(_a_, ...)                                                                       \
    do                                                                                             \
        if (!(_a_)) [[unlikely]] {                                                                 \
            [&]() CITRA_NO_INLINE CITRA_NO_RETURN {                                                \
                LOG_CRITICAL(Debug, "Assertion Failed!\n" __VA_ARGS__);                            \
                HEN_ASSERT_HOOK();                                                                 \
                Common::Log::Stop();                                                               \
                Crash();                                                                           \
                exit(1);                                                                           \
            }();                                                                                   \
        }                                                                                          \
    while (0)

#define UNREACHABLE()                                                                              \
    ([]() CITRA_NO_INLINE CITRA_NO_RETURN {                                                        \
        LOG_CRITICAL(Debug, "Unreachable code!");                                                  \
        HEN_ASSERT_HOOK();                                                                         \
        Common::Log::Stop();                                                                       \
        Crash();                                                                                   \
        exit(1);                                                                                   \
    }())

#define UNREACHABLE_MSG(...)                                                                       \
    ([&]() CITRA_NO_INLINE CITRA_NO_RETURN {                                                       \
        LOG_CRITICAL(Debug, "Unreachable code!\n" __VA_ARGS__);                                    \
        HEN_ASSERT_HOOK();                                                                         \
        Common::Log::Stop();                                                                       \
        Crash();                                                                                   \
        exit(1);                                                                                   \
    }())

#ifdef _DEBUG
#define DEBUG_ASSERT(_a_) ASSERT(_a_)
#define DEBUG_ASSERT_MSG(_a_, ...) ASSERT_MSG(_a_, __VA_ARGS__)
#else // not debug
#define DEBUG_ASSERT(_a_)
#define DEBUG_ASSERT_MSG(_a_, _desc_, ...)
#endif

#define UNIMPLEMENTED() LOG_CRITICAL(Debug, "Unimplemented code!")
#define UNIMPLEMENTED_MSG(_a_, ...) LOG_CRITICAL(Debug, _a_, __VA_ARGS__)
