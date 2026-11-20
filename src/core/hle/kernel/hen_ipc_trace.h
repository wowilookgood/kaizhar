// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// (Kaizhar, development builds: HEN_DEV) What each emulated thread last asked a service, and
// how often each request was made lately. A title that sits and shows nothing while it keeps
// running is asking some service the same thing over and over: the report of the emulated
// threads at a title's end (citra_switch/main.cpp) prints both. Only the thread that runs the
// emulated console uses this. Other builds: nothing is kept, the calls are empty.

#include <string>
#include <vector>
#include "common/common_types.h"

namespace Kernel::HenIpc {

/// The last request of one thread.
struct Last {
    std::string session; ///< the session's name (the service's, with "_Server" added)
    u32 header = 0;      ///< the request's first word: command and sizes
    u64 sent_us = 0;     ///< emulated time
    bool answered = false;
};

/// One kind of request and how many of it were made.
struct Count {
    u32 process_id = 0;
    std::string session;
    u32 header = 0;
    u32 count = 0;
};

/// The last time an event, mutex or semaphore was signalled or released.
struct Signal {
    u32 thread_id = 0; ///< 0: by the emulator's side (a service, a timer), not by an emulated thread
    u64 at_us = 0;
    u32 count = 0;
};

#if HEN_DEV
/// A request as it is sent. words: its first five (the header and four more); app: the asking
/// process is not a system module; lle: the session is served by emulated code (a system module
/// running as the console's own code). Lines in the log, each one line:
///   "hen: lle: ..." every request of an application to such a session, and its answer;
///   "hen: req: ..." an application's requests of any kind from its first one: the first
///   REQ_PER_KIND of each kind (session and header), REQ_LINES lines at most.
void Request(u32 thread_id, u32 process_id, bool app, bool lle, const std::string& session,
             const u32* words, u64 now_us);
/// The request of this thread is not answered yet (an emulated server, or a service that put
/// the thread to sleep): its "hen: req:" line is printed now, the answer gets a line of its own.
void Pending(u32 thread_id);
/// The answer to the thread's last request. words: its first five (header, result, three more).
void Answer(u32 thread_id, const u32* words, u64 now_us);
/// "hen: req:" lines printed and requests of applications left out.
void RequestLineStats(u64& printed, u64& left_out);
/// An event was signalled, a mutex or a semaphore released. object_id: Object::GetObjectId().
void Signalled(u32 object_id, u32 thread_id, u64 now_us);
const Signal* FindSignal(u32 object_id);
/// The emulated thread whose system call is signalling an event right now; 0: none (the
/// emulator's side is). Set around the call by the system call, read by Event::Signal.
extern u32 signalling_thread;
/// Null: the thread has made no request.
const Last* Find(u32 thread_id);
inline constexpr u32 REQ_PER_KIND = 6;
inline constexpr u32 REQ_LINES = 1500;
/// The requests made at since_us or later, most frequent first. total: how many those were;
/// kept_from_us: the oldest request still remembered if that is later than since_us (the
/// counts then cover less time than was asked for), otherwise since_us.
std::vector<Count> Recent(u64 since_us, u64& total, u64& kept_from_us);
/// A new emulated console: nothing of the last one's threads applies.
void Reset();
#else
inline const Last* Find(u32) {
    return nullptr;
}
inline const Signal* FindSignal(u32) {
    return nullptr;
}
inline void RequestLineStats(u64& printed, u64& left_out) {
    printed = left_out = 0;
}
inline std::vector<Count> Recent(u64 since_us, u64& total, u64& kept_from_us) {
    total = 0;
    kept_from_us = since_us;
    return {};
}
inline void Reset() {}
#endif

} // namespace Kernel::HenIpc
