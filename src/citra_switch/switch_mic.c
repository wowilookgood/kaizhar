// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Microphone input through libnx's audin (the default device: 48 kHz, s16) for
// audio_core/switch_input.cpp. A thread opens the device when a title starts
// sampling, hands every buffer the system releases to the input, and closes
// the device again when the title stops. Nothing here makes the caller wait:
// starting and stopping only tell the thread what is wanted.
//
// Any call that fails turns the microphone off for the rest of the session
// (the title then gets no samples, as with no microphone), with one line in
// the log of a development build.
//
// The service lookup is the one call that may never return: the homebrew
// environment forwards lookups to the system, which holds a lookup of a name
// nobody registered, and every other program's lookups behind it, until the
// environment restarts. So the lookup is noted in a file before it is made
// and again when it has returned: a start that finds the first note alone
// leaves the microphone off instead of asking again.
#include <switch.h>

#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "switch_platform.h"

#define SW_MIC_BUFFERS 4
#define SW_MIC_BUFFER_MS 20
#define SW_MIC_BUFFER_SIZE 0x8000 // 20 ms of 8 channels at 96 kHz fit
#define SW_MIC_MAX_CHANNELS 8
#define SW_MIC_NOTE "/switch/Kaizhar/mic-service.txt"

#if HEN_DEV
#define MIC_LOG(...) printf("hen: mic: " __VA_ARGS__)
#else
#define MIC_LOG(...) ((void)0)
#endif

typedef void (*sw_mic_deliver)(void* user, const int16_t* frames, size_t count, unsigned channels, unsigned rate);

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER; // what is wanted and the thread's state
static sw_mic_deliver s_deliver;
static void* s_user;
static pthread_t s_thread;
static bool s_thread_live; // created and not joined
static bool s_thread_done; // it has left its loop for good
static atomic_bool s_want;
static atomic_bool s_failed; // a device call failed: off until the program ends
static bool s_left_behind;
static AudioInBuffer s_buffers[SW_MIC_BUFFERS];

// "pending" or "returned", from the note of the last lookup; "" without one.
static void note_read(char* word, size_t size) {
    word[0] = 0;
    FILE* const file = fopen(SW_MIC_NOTE, "r");
    if (file != NULL) {
        if (fgets(word, (int)size, file) != NULL) {
            word[strcspn(word, " \n")] = 0;
        }
        fclose(file);
    }
}

static void note_write(const char* text) {
    mkdir("/switch/Kaizhar", 0777);
    FILE* const file = fopen(SW_MIC_NOTE, "w");
    if (file != NULL) {
        fputs(text, file);
        fclose(file);
    }
}

// audinInitialize: the lookup of audin:u, opening the default device and its buffer event.
static Result mic_open(void) {
    char word[16];
    note_read(word, sizeof(word));
    if (strcmp(word, "pending") == 0) {
        MIC_LOG("an earlier lookup of audin:u never returned: microphone off (delete %s to try again)\n",
                SW_MIC_NOTE);
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    const bool known = strcmp(word, "returned") == 0;
    if (!known) {
        note_write("pending\nThe lookup of the audio input service (audin:u) was started and has not returned.\n"
                   "If this is still here after Kaizhar has closed, the system has no such service:\n"
                   "the microphone stays off while this file exists.\n");
    }
    MIC_LOG("looking up audin:u%s\n",
            known ? "" : " (no line after this one: the lookup is held, and every program's lookups with it)");
    const Result rc = audinInitialize();
    if (!known) {
        char text[64];
        snprintf(text, sizeof(text), "returned\nThe lookup of audin:u returned (opening the device: 0x%x).\n", rc);
        note_write(text);
    }
    return rc;
}

static void buffers_free(void) {
    for (int i = 0; i < SW_MIC_BUFFERS; i++) {
        free(s_buffers[i].buffer);
        s_buffers[i].buffer = NULL;
    }
}

// Queues every buffer for 20 ms of the device's format and starts capturing.
static Result capture_start(unsigned rate, unsigned channels, Result* append_rc) {
    size_t bytes = (size_t)rate * SW_MIC_BUFFER_MS / 1000 * channels * sizeof(int16_t);
    if (bytes > SW_MIC_BUFFER_SIZE) {
        bytes = SW_MIC_BUFFER_SIZE;
    }
    Result rc = 0;
    for (int i = 0; i < SW_MIC_BUFFERS && R_SUCCEEDED(rc); i++) {
        memset(&s_buffers[i], 0, sizeof(s_buffers[i]));
        s_buffers[i].buffer = memalign(0x1000, SW_MIC_BUFFER_SIZE);
        if (s_buffers[i].buffer == NULL) {
            rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
            break;
        }
        memset(s_buffers[i].buffer, 0, SW_MIC_BUFFER_SIZE);
        s_buffers[i].buffer_size = SW_MIC_BUFFER_SIZE;
        s_buffers[i].data_size = bytes;
        rc = audinAppendAudioInBuffer(&s_buffers[i]);
    }
    if (append_rc != NULL) {
        *append_rc = rc;
    }
    return R_SUCCEEDED(rc) ? audinStartAudioIn() : rc;
}

#if HEN_DEV
struct mic_level {
    unsigned buffers, peak;
    unsigned long long frames;
    double squares;
};

static void level_add(struct mic_level* level, const AudioInBuffer* buffer, unsigned channels) {
    const int16_t* const samples = buffer->buffer;
    const size_t count = buffer->data_size / sizeof(int16_t);
    for (size_t i = 0; i < count; i++) {
        const unsigned value = (unsigned)abs(samples[i]);
        if (value > level->peak) {
            level->peak = value;
        }
        level->squares += (double)samples[i] * samples[i];
    }
    level->buffers++;
    level->frames += count / channels;
}

static double level_rms(const struct mic_level* level, unsigned channels) {
    return level->frames == 0 ? 0.0 : sqrt(level->squares / ((double)level->frames * channels));
}
#endif

// One sampling session: the device from open to close. False if a call failed.
static bool capture(void) {
    Result rc = mic_open();
    if (R_FAILED(rc)) {
        MIC_LOG("audin unavailable (audinInitialize 0x%x): no microphone\n", rc);
        return false;
    }
    const unsigned rate = audinGetSampleRate(), channels = audinGetChannelCount();
    bool ok = audinGetPcmFormat() == PcmFormat_Int16 && rate != 0 && channels >= 1 && channels <= SW_MIC_MAX_CHANNELS;
    if (!ok) {
        MIC_LOG("device format not handled (%u Hz, %u channels, format %d): no microphone\n", rate, channels,
                (int)audinGetPcmFormat());
    } else if (R_FAILED(rc = capture_start(rate, channels, NULL))) {
        MIC_LOG("couldn't start capturing (0x%x): no microphone\n", rc);
        ok = false;
    } else {
        MIC_LOG("audin opened: %u Hz, %u channel(s), s16, %d buffers of %d ms\n", rate, channels, SW_MIC_BUFFERS,
                SW_MIC_BUFFER_MS);
    }
#if HEN_DEV
    struct mic_level level = {0};
    unsigned waits = 0;
#endif
    while (ok && atomic_load(&s_want)) {
        AudioInBuffer* released = NULL;
        u32 count = 0;
        // A timeout so that stopping never waits on a stalled device.
        rc = audinWaitCaptureFinish(&released, &count, 50000000ull);
        if (rc == KERNELRESULT(TimedOut)) {
#if HEN_DEV
            if (level.buffers == 0 && ++waits == 40) {
                MIC_LOG("no buffer from the device in 2 s\n");
            }
#endif
            continue;
        }
        while (R_SUCCEEDED(rc) && released != NULL) {
            const size_t frames = released->data_size / (channels * sizeof(int16_t));
            pthread_mutex_lock(&s_lock);
            if (atomic_load(&s_want) && s_deliver != NULL && frames != 0) {
                s_deliver(s_user, released->buffer, frames, channels, rate);
            }
            pthread_mutex_unlock(&s_lock);
#if HEN_DEV
            level_add(&level, released, channels);
            if (level.buffers == 50 || level.buffers % 500 == 0) {
                MIC_LOG("%u buffers, %llu frames from the device, peak %u, rms %.1f (of 32768)\n", level.buffers,
                        level.frames, level.peak, level_rms(&level, channels));
            }
#endif
            rc = audinAppendAudioInBuffer(released);
            released = NULL;
            if (R_SUCCEEDED(rc) && (R_FAILED(audinGetReleasedAudioInBuffer(&released, &count)) || count == 0)) {
                break;
            }
        }
        if (R_FAILED(rc)) {
            MIC_LOG("capturing failed (0x%x): no microphone from here on\n", rc);
            ok = false;
        }
    }
    audinStopAudioIn();
    audinExit();
    buffers_free(); // (after the session has closed: the system reads and writes them until then)
    MIC_LOG("audin closed\n");
    return ok;
}

static void* mic_thread(void* arg) {
    (void)arg;
    for (;;) {
        if (!capture()) {
            atomic_store(&s_failed, true);
        }
        // (a title that started sampling again while the device was closing)
        pthread_mutex_lock(&s_lock);
        const bool again = atomic_load(&s_want) && !atomic_load(&s_failed);
        s_thread_done = !again;
        pthread_mutex_unlock(&s_lock);
        if (!again) {
            return NULL;
        }
    }
}

// Starts sampling: deliver is called from the microphone's thread with every buffer of the
// device (interleaved s16 frames). False if there is no microphone to ask.
bool sw_mic_start(sw_mic_deliver deliver, void* user) {
    pthread_mutex_lock(&s_lock);
    bool ok = !atomic_load(&s_failed);
    if (ok) {
        s_deliver = deliver;
        s_user = user;
        atomic_store(&s_want, true);
        if (s_thread_live && s_thread_done) {
            pthread_join(s_thread, NULL);
            s_thread_live = false;
        }
        if (!s_thread_live) {
            s_thread_done = false;
            s_thread_live = pthread_create(&s_thread, NULL, mic_thread, NULL) == 0;
            if (!s_thread_live) {
                atomic_store(&s_want, false);
                ok = false;
            }
        }
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

// Stops sampling: deliver is not called again once this returns. The thread closes the device
// by itself within a few buffers.
void sw_mic_stop(void) {
    pthread_mutex_lock(&s_lock);
    atomic_store(&s_want, false);
    s_deliver = NULL;
    pthread_mutex_unlock(&s_lock);
}

// At exit: the thread ended and joined. A thread held inside the service (the lookup) can't be
// ended: it is left for the exit code in switch_platform.c, which pauses what still runs.
void sw_mic_shutdown(void) {
    sw_mic_stop();
    if (!s_thread_live || s_left_behind) {
        return;
    }
    bool done = false;
    for (int waited = 0; waited < 1000 && !done; waited += 10) {
        pthread_mutex_lock(&s_lock);
        done = s_thread_done;
        pthread_mutex_unlock(&s_lock);
        if (!done) {
            svcSleepThread(10000000);
        }
    }
    if (done) {
        pthread_join(s_thread, NULL);
        s_thread_live = false;
    } else {
        s_left_behind = true;
        MIC_LOG("the microphone's thread is held in the service: left behind\n");
    }
}

#if HEN_DEV
// --mic-probe: records from the device that is open for ms and prints what arrived.
static void probe_record(const char* name, bool by_event, unsigned rate, unsigned channels, int format, int state,
                         unsigned ms) {
    if (format != PcmFormat_Int16 || rate == 0 || channels < 1 || channels > SW_MIC_MAX_CHANNELS) {
        MIC_LOG("probe: \"%s\": %u Hz, %u channel(s), format %d, state %d: format not handled, not recorded\n", name,
                rate, channels, format, state);
        return;
    }
    Result append_rc = 0, wait_rc = 0;
    const Result start_rc = capture_start(rate, channels, &append_rc);
    struct mic_level level = {0};
    const u64 t0 = armGetSystemTick();
    while (R_SUCCEEDED(start_rc) && armTicksToNs(armGetSystemTick() - t0) < ms * 1000000ull) {
        AudioInBuffer* released = NULL;
        u32 count = 0;
        Result rc = by_event ? audinWaitCaptureFinish(&released, &count, 50000000ull)
                             : audinGetReleasedAudioInBuffer(&released, &count);
        if (R_FAILED(rc) && rc != KERNELRESULT(TimedOut)) {
            wait_rc = rc;
            break;
        }
        if (R_FAILED(rc) || released == NULL) {
            svcSleepThread(by_event ? 0 : 5000000);
            continue;
        }
        level_add(&level, released, channels);
        audinAppendAudioInBuffer(released);
    }
    const Result stop_rc = audinStopAudioIn();
    MIC_LOG("probe: \"%s\": %u Hz, %u channel(s), format %d, state %d; append 0x%x, start 0x%x, %s 0x%x, "
            "stop 0x%x; %u buffers, %llu frames in %u ms, peak %u, rms %.1f (of 32768)\n",
            name, rate, channels, format, state, append_rc, start_rc, by_event ? "wait" : "poll", wait_rc, stop_rc,
            level.buffers, level.frames, ms, level.peak, level_rms(&level, channels));
}

static atomic_bool s_probe_done;

static void* probe_thread(void* arg) {
    (void)arg;
    const Result rc = mic_open();
    MIC_LOG("probe: audinInitialize 0x%x\n", rc);
    if (R_SUCCEEDED(rc)) {
        static char names[4][0x100];
        u32 count = 0;
        const Result list_rc = audinListAudioIns(names[0], 4, &count);
        MIC_LOG("probe: audinListAudioIns 0x%x, %u device(s)\n", list_rc, count);
        // The default device, the way a title's sampling uses it: by the buffer event.
        probe_record("(default)", true, audinGetSampleRate(), audinGetChannelCount(), (int)audinGetPcmFormat(),
                     (int)audinGetDeviceState(), 1000);
        // Then each listed device by name. (libnx keeps one device session and its event belongs
        // to the default device's: these are polled.)
        for (u32 i = 0; R_SUCCEEDED(list_rc) && i < count && i < 4; i++) {
            char opened[0x100] = {0};
            u32 rate = 0, channels = 0;
            PcmFormat format = PcmFormat_Invalid;
            AudioInState state = AudioInState_Stopped;
            names[i][0xff] = 0;
            serviceClose(audinGetServiceSession_AudioIn());
            const Result open_rc =
                audinOpenAudioIn(names[i], opened, 48000, 0x00020000, &rate, &channels, &format, &state);
            opened[0xff] = 0;
            MIC_LOG("probe: device %u \"%s\": audinOpenAudioIn 0x%x (opened \"%s\")\n", i, names[i], open_rc, opened);
            if (R_SUCCEEDED(open_rc)) {
                probe_record(names[i], false, rate, channels, (int)format, (int)state, 500);
            }
        }
        audinExit();
        buffers_free();
    }
    atomic_store(&s_probe_done, true);
    return NULL;
}

void sw_mic_probe(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, probe_thread, NULL) != 0) {
        return;
    }
    for (int waited = 0; waited < 8000 && !atomic_load(&s_probe_done); waited += 50) {
        svcSleepThread(50000000);
    }
    if (atomic_load(&s_probe_done)) {
        pthread_join(thread, NULL);
        MIC_LOG("probe: done\n");
    } else {
        MIC_LOG("probe: no answer in 8 s: a call is held in the service. If the last line is the lookup, audin:u "
                "is not registered, and service lookups stay held until the homebrew environment restarts\n");
    }
}
#endif
