// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Audio output through libnx's audout (48 kHz, stereo, s16) for
// audio_core/switch_sink.cpp: a thread keeps a few short buffers queued and
// asks the sink to fill each one as the system releases it.
#include <switch.h>

#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define SW_AUDIO_RATE 48000
#define SW_AUDIO_BUFFERS 5
#define SW_AUDIO_FRAMES 768  // 16 ms each: ~80 ms queued

static void (*s_fill)(void* user, int16_t* frames, size_t count);
static void* s_user;
static pthread_t s_thread;
static atomic_bool s_run;
static bool s_open;
static AudioOutBuffer s_buffers[SW_AUDIO_BUFFERS];
// Statistics since the last sw_audio_stats call.
static _Atomic unsigned s_wakes, s_starved, s_maxGapUs, s_maxFillUs;

// wakes, times the device had played everything queued (an audible gap), the longest time
// between two wakes and the longest fill, in microseconds.
void sw_audio_stats(unsigned* out) {
    out[0] = atomic_exchange(&s_wakes, 0);
    out[1] = atomic_exchange(&s_starved, 0);
    out[2] = atomic_exchange(&s_maxGapUs, 0);
    out[3] = atomic_exchange(&s_maxFillUs, 0);
}

unsigned sw_audio_sample_rate(void) {
    return SW_AUDIO_RATE;
}

static void fill_and_queue(AudioOutBuffer* buffer) {
    const u64 t0 = armGetSystemTick();
    s_fill(s_user, buffer->buffer, SW_AUDIO_FRAMES);
    const unsigned us = (unsigned)(armTicksToNs(armGetSystemTick() - t0) / 1000);
    if (us > atomic_load(&s_maxFillUs)) {
        atomic_store(&s_maxFillUs, us);
    }
    buffer->data_size = SW_AUDIO_FRAMES * 2 * sizeof(int16_t);
    buffer->data_offset = 0;
    audoutAppendAudioOutBuffer(buffer);
}

static void* audio_thread(void* arg) {
    (void)arg;
    // Ahead of the emulation threads: a late refill is an audible gap.
    s32 priority = 0;
    if (R_SUCCEEDED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)) && priority > 1) {
        const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, priority - 1);
        printf("hen: audio: thread priority %d -> %d (rc 0x%x)\n", priority, priority - 1, rc);
    }
    for (int i = 0; i < SW_AUDIO_BUFFERS; i++) {
        fill_and_queue(&s_buffers[i]);
    }
    u64 lastWake = armGetSystemTick();
    while (atomic_load(&s_run)) {
        AudioOutBuffer* released = NULL;
        u32 count = 0;
        // A timeout so that stopping never waits on a stalled device.
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ull))) {
            continue;
        }
        const u64 now = armGetSystemTick();
        const unsigned gapUs = (unsigned)(armTicksToNs(now - lastWake) / 1000);
        lastWake = now;
        if (gapUs > atomic_load(&s_maxGapUs)) {
            atomic_store(&s_maxGapUs, gapUs);
        }
        atomic_fetch_add(&s_wakes, 1);
        int refilled = 0;
        while (released != NULL) {
            fill_and_queue(released);
            refilled++;
            released = NULL;
            if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)) || count == 0) {
                break;
            }
        }
        if (refilled >= SW_AUDIO_BUFFERS) {
            atomic_fetch_add(&s_starved, 1);
        }
    }
    return NULL;
}

bool sw_audio_start(void (*fill)(void* user, int16_t* frames, size_t count), void* user) {
    if (s_open) {
        return false;
    }
    Result rc = audoutInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = audoutStartAudioOut();
        if (R_FAILED(rc)) {
            audoutExit();
        }
    }
    if (R_FAILED(rc)) {
        printf("hen: audio: audout unavailable (0x%x)\n", rc);
        return false;
    }
    const size_t size = (SW_AUDIO_FRAMES * 2 * sizeof(int16_t) + 0xfff) & ~(size_t)0xfff;
    for (int i = 0; i < SW_AUDIO_BUFFERS; i++) {
        memset(&s_buffers[i], 0, sizeof(s_buffers[i]));
        s_buffers[i].buffer = memalign(0x1000, size);
        s_buffers[i].buffer_size = size;
        if (s_buffers[i].buffer == NULL) {
            audoutStopAudioOut();
            audoutExit();
            return false;
        }
        memset(s_buffers[i].buffer, 0, size);
    }
    s_fill = fill;
    s_user = user;
    atomic_store(&s_run, true);
    if (pthread_create(&s_thread, NULL, audio_thread, NULL) != 0) {
        atomic_store(&s_run, false);
        audoutStopAudioOut();
        audoutExit();
        return false;
    }
    s_open = true;
    printf("hen: audio: audout started, %d Hz, %d buffers of %d frames\n", SW_AUDIO_RATE, SW_AUDIO_BUFFERS,
           SW_AUDIO_FRAMES);
    return true;
}

void sw_audio_stop(void) {
    if (!s_open) {
        return;
    }
    atomic_store(&s_run, false);
    pthread_join(s_thread, NULL);
    audoutStopAudioOut();
    audoutExit();
    for (int i = 0; i < SW_AUDIO_BUFFERS; i++) {
        free(s_buffers[i].buffer);
        s_buffers[i].buffer = NULL;
    }
    s_open = false;
}
