// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <random>
#include <tuple>
#include "common/hen_stats.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core_timing.h"

// (Switch 2 port, --n3ds-clock; task "804 MHz") The New 3DS's fast clock. A title asks for it
// in its header (KernelSystem::SetRunning804MHz) and upstream only records that: every
// instruction is charged the emulated time it takes at 268 MHz, so a title written for the
// fast clock does not get its frame's work done in a frame (Xenoblade Chronicles 3D: 16 to
// 21 of its 30 frames a second at full emulation speed). With the switch on and the mode
// asked for, CPU work costs a third of the time (804 / 268 = 3), through the path the
// cpu_clock_percentage setting already has: Timer::AddTicks, which is what generated code,
// the interpreter tier and the per-system-call charges all go through. Not scaled, because
// they are times and not CPU work: scheduled events (vertical blank, audio, timers, sleeps),
// idle skipping, and the system tick the program reads. The mode is one state for the whole
// system on the console too (all cores, whoever runs), set while the title that asked runs.
// The price is host CPU: up to three times the instructions per emulated second.
//
// Automatic, the default: 3 is the ceiling and 1 (upstream's charge) the floor,
// and HenClockControl below moves the factor between them by what the host has to spare, so
// that a scene the host cannot pay for at 804 MHz costs the game frames instead of emulation
// speed and sound. Xenoblade's recorded route, 5 s intervals: fixed 1: 16 to 30 fps (median
// 23) at 91 to 101% speed; fixed 3: 26 to 30 fps, speed down to 85%, 3397 ms of lag given up.
// hen_n3ds_clock: 0 fixed 1 (--n3ds-clock-off), 1 fixed 3 (--n3ds-clock), 2 automatic.
#ifdef __SWITCH__
extern "C" int hen_n3ds_clock;                    // hen/jit/hen_jit.c
extern "C" unsigned long long hen_n3ds_clock_ticks; // work charged at the fast rate (report)
extern "C" int hen_n3ds_mode;
extern "C" unsigned long long hen_jit_interpreted_ns(void);
#else
static int hen_n3ds_clock = 0;
static unsigned long long hen_n3ds_clock_ticks = 0;
static int hen_n3ds_mode = 0;
static unsigned long long hen_jit_interpreted_ns() {
    return 0;
}
#endif

// ("hen: emu:") The scheduled events run on the emulation thread between two slices of the
// emulated CPU: how many of each kind and the time in them (the vertical blank's includes the
// frame limiter's sleep and the wait for the GPU thread; the sound hardware's is the whole of
// making a frame of sound). The clock is read only with hen_emu_stats_on. Development builds.
// Emulation thread only.
#if defined(__SWITCH__) && HEN_DEV
extern "C" int hen_emu_stats_on;
extern "C" unsigned long long hen_jit_now_ns(void);
namespace {
struct HenEventKind {
    const Core::TimingEventType* type;
    unsigned long long count, ns;
};
constexpr int HEN_EVENT_KINDS = 24;
HenEventKind hen_event_kinds[HEN_EVENT_KINDS];
unsigned long long hen_event_other[2]; // (count, ns: kinds past the table's size)
void HenNoteEvent(const Core::TimingEventType* type, unsigned long long ns) {
    for (HenEventKind& kind : hen_event_kinds) {
        if (kind.type == type || kind.type == nullptr) {
            kind.type = type;
            kind.count++;
            kind.ns += ns;
            return;
        }
    }
    hen_event_other[0]++;
    hen_event_other[1] += ns;
}
} // namespace
namespace Core {
// Since the last call: every event run and the time in all of them into total[2]; the kinds
// with the most time, most first, into names/count/ns (up to max). Returns how many.
int HenEventStats(const char** names, unsigned long long* count, unsigned long long* ns, int max,
                  unsigned long long* total) {
    total[0] = hen_event_other[0];
    total[1] = hen_event_other[1];
    hen_event_other[0] = hen_event_other[1] = 0;
    for (const HenEventKind& kind : hen_event_kinds) {
        total[0] += kind.count;
        total[1] += kind.ns;
    }
    int found = 0;
    for (; found < max; found++) {
        HenEventKind* best = nullptr;
        for (HenEventKind& kind : hen_event_kinds) {
            if (kind.count != 0 && (best == nullptr || kind.ns > best->ns ||
                                    (kind.ns == best->ns && kind.count > best->count))) {
                best = &kind;
            }
        }
        if (best == nullptr) {
            break;
        }
        names[found] = best->type->name->c_str();
        count[found] = best->count;
        ns[found] = best->ns;
        best->count = best->ns = 0;
    }
    // (the types stay: an event type lives as long as the emulated system; Timing's
    // constructor empties the table)
    for (HenEventKind& kind : hen_event_kinds) {
        kind.count = kind.ns = 0;
    }
    return found;
}
} // namespace Core
#endif

// Wake-ups queued from other threads and taken over by the emulation thread (hang report).
#include <atomic>
std::atomic<unsigned> hen_ts_pushed{0}, hen_ts_moved{0};

namespace Core {

static bool hen_fast_clock = false;
static u32 hen_clock_percentage = 100;

// What CPU work is divided by while the mode is on, and the controller's state.
// The floor is the old machine's rate. 0.75 gives nothing (Xenoblade's heavy stretch):
// the host's work stays the same (the title does the same work a frame; 3.3 s of 5 on the
// core either way) and speed and frames are inside the run-to-run spread, so the simpler
// floor stays.
constexpr double HEN_CLOCK_FLOOR = 1.0, HEN_CLOCK_STORED = 1.0, HEN_CLOCK_CEILING = 3.0;
static double hen_clock_factor = HEN_CLOCK_CEILING;
static s64 hen_clock_work_us, hen_clock_budget_us; // this window: host time used, emulated time
static unsigned long long hen_clock_interpreted;   // hen_jit_interpreted_ns at the last window
static int hen_clock_vblanks;                      // in this window
static int hen_clock_hold, hen_clock_backoff;      // windows without a step up; the next hold
// (for the report) factor: lowest, sum, highest, samples; steps down, up; ticks charged
static double hen_clock_seen[4] = {HEN_CLOCK_CEILING, 0, 0, 0};
static unsigned hen_clock_steps[2];
static unsigned long long hen_clock_charged;

/// The kernel's 804 MHz state changed (kernel.cpp, UpdateCPUAndMemoryState).
void HenSetFastClock(Timing& timing, bool running_804mhz) {
    hen_n3ds_mode = running_804mhz; // (hen_jit.c: the loading boost holds the CPU clock, docked)
    const bool fast = running_804mhz && hen_n3ds_clock != 0;
    if (fast != hen_fast_clock) {
        std::printf("hen: clock: 804 MHz mode %s\n", fast ? "on" : "off");
    }
    hen_fast_clock = fast;
    hen_clock_factor = HEN_CLOCK_CEILING; // (automatic: from the real machine's rate down)
    hen_clock_work_us = hen_clock_budget_us = hen_clock_vblanks = 0;
    hen_clock_hold = 0;
    hen_clock_backoff = 5;
    timing.UpdateClockSpeed(hen_clock_percentage);
}

/// The automatic clock's controller; once a vertical blank from the frame limiter, with the
/// host time the blank's work took and the time it had (the emulated time at the speed limit).
///
/// It decides once a window of 12 blanks (200 ms: whole game frames at 30, 20, 15 and 10 a
/// second, so a title's uneven blanks do not show), on load = work / budget:
///  - over 0.95: factor *= 0.90 / load, by 15% a step at most. Measured (Xenoblade): the
///    host's work is far from proportional to the factor (13.3 ms a blank at 1x, 15.5 at 3x:
///    a title that makes its frame idles the rest), so the proportional step fell to the floor
///    in two windows; the limit makes it several. Decided early once the window's whole
///    budget is spent already. A blank counts for two at most: a stall (a compile, a file) is
///    not work the clock can take back.
///  - under 0.90: factor *= 1 + 1/16. (0.85 left the factor at the floor for good: there the
///    same title's load is 0.80 to 0.85 at 22 frames a second.) A step up can cross 0.95;
///    the hold below then spaces the tries out.
///  - a step down holds steps up for 1 s, then 2, then 4 while it keeps happening (a title
///    locked to the blank loses a third of its load when it misses a frame, and would be
///    probed into the faster rate and out again several times a second); reaching the ceiling
///    resets that.
/// The 5% under the budget is what repays the lag of the window that overran: the worst
/// measured (19.3 ms a blank at factor 3 against 16.7) is 31 ms a window, inside the limiter's
/// 60 ms allowance, repaid in about 0.6 s at 0.90.
void HenClockControl(Timing& timing, s64 work_us, s64 budget_us) {
    if (!hen_fast_clock) {
        return;
    }
    hen_clock_seen[0] = std::min(hen_clock_seen[0], hen_clock_factor);
    hen_clock_seen[1] += hen_clock_factor;
    hen_clock_seen[2] = std::max(hen_clock_seen[2], hen_clock_factor);
    hen_clock_seen[3]++;
    // (over 100 ms is not a blank: the first call, a pause, the title list)
    if (hen_n3ds_clock < 2 || budget_us <= 0 || budget_us > 100'000) {
        return;
    }
    constexpr int window = 12;
    hen_clock_work_us += std::min(work_us, 2 * budget_us);
    hen_clock_budget_us += budget_us;
    if (++hen_clock_vblanks < window && hen_clock_work_us <= window * budget_us) {
        return;
    }
    const double load = static_cast<double>(hen_clock_work_us) / hen_clock_budget_us;
    hen_clock_work_us = hen_clock_budget_us = hen_clock_vblanks = 0;
    // New code (over 1 ms of the window in the interpreter tier, the loading boost's line): the
    // host is paying for first visits, and at 3x the title reaches three times as many of them
    // a blank (Smash cold: 224 ms of lag against 0 to 11 at 1x, all of it in the
    // intervals with new code). The stored clock until the code is compiled.
    const unsigned long long interpreted = hen_jit_interpreted_ns();
    const bool new_code = interpreted - hen_clock_interpreted > 1'000'000;
    hen_clock_interpreted = interpreted;
    double factor = hen_clock_factor;
    if (new_code) {
        factor = std::min(factor, HEN_CLOCK_STORED);
        hen_clock_hold = 1;
    } else if (load > 0.95) {
        factor = std::max(factor * std::max(0.90 / load, 0.85), HEN_CLOCK_FLOOR);
        hen_clock_hold = hen_clock_backoff;
        hen_clock_backoff = std::min(hen_clock_backoff * 2, 20);
    } else if (hen_clock_hold > 0) {
        hen_clock_hold--;
    } else if (load < 0.90) {
        factor = std::min(factor * (1 + 1.0 / 16), HEN_CLOCK_CEILING);
        if (factor == HEN_CLOCK_CEILING) {
            hen_clock_backoff = 5;
        }
    }
    if (factor != hen_clock_factor) {
        hen_clock_steps[factor > hen_clock_factor]++;
        hen_clock_factor = factor;
        timing.UpdateClockSpeed(hen_clock_percentage);
    }
}

/// For the report, since the last call: the factor's lowest, mean and highest, steps down and
/// up, ticks of CPU work (268 MHz) and what they were charged as. False: the mode was not on.
bool HenClockStats(double* out) {
    const bool any = hen_clock_seen[3] != 0;
    out[0] = hen_clock_seen[0];
    out[1] = any ? hen_clock_seen[1] / hen_clock_seen[3] : 0;
    out[2] = hen_clock_seen[2];
    out[3] = hen_clock_steps[0];
    out[4] = hen_clock_steps[1];
    out[5] = static_cast<double>(hen_n3ds_clock_ticks);
    out[6] = static_cast<double>(hen_clock_charged);
    hen_clock_seen[0] = HEN_CLOCK_CEILING;
    hen_clock_seen[1] = hen_clock_seen[2] = hen_clock_seen[3] = 0;
    hen_clock_steps[0] = hen_clock_steps[1] = 0;
    hen_n3ds_clock_ticks = hen_clock_charged = 0;
    return any;
}

// Sort by time, unless the times are the same, in which case sort by the order added to the queue
bool Timing::Event::operator>(const Timing::Event& right) const {
    return std::tie(time, fifo_order) > std::tie(right.time, right.fifo_order);
}

bool Timing::Event::operator<(const Timing::Event& right) const {
    return std::tie(time, fifo_order) < std::tie(right.time, right.fifo_order);
}

Timing::Timing(std::size_t num_cores, u32 cpu_clock_percentage, s64 override_base_ticks) {
    // Generate non-zero base tick count to simulate time the system ran before launching the game.
    // This accounts for games that rely on the system tick to seed randomness.
    const auto base_ticks = override_base_ticks >= 0 ? override_base_ticks : GenerateBaseTicks();
#if defined(__SWITCH__) && HEN_DEV
    // (the event types of the system before this one are gone)
    for (HenEventKind& kind : hen_event_kinds) {
        kind = {};
    }
    hen_event_other[0] = hen_event_other[1] = 0;
#endif

    timers.resize(num_cores);
    for (std::size_t i = 0; i < num_cores; ++i) {
        timers[i] = std::make_shared<Timer>(base_ticks);
    }
    hen_fast_clock = false; // (a new title: until its kernel says otherwise)
    UpdateClockSpeed(cpu_clock_percentage);
    current_timer = timers[0].get();
}

s64 Timing::GenerateBaseTicks() {
    if (Settings::values.init_ticks_type.GetValue() == Settings::InitTicks::Fixed) {
        return Settings::values.init_ticks_override.GetValue();
    }
    // Bounded to 32 bits to make sure we don't generate too high of a counter and risk overflowing.
    std::mt19937 random_gen(std::random_device{}());
    return random_gen();
}

void Timing::UpdateClockSpeed(u32 cpu_clock_percentage) {
    hen_clock_percentage = cpu_clock_percentage;
    for (auto& timer : timers) {
        timer->cpu_clock_scale = 100.0 / cpu_clock_percentage;
        if (hen_fast_clock) {
            timer->cpu_clock_scale /= hen_n3ds_clock == 1 ? HEN_CLOCK_CEILING : hen_clock_factor;
        }
    }
}

TimingEventType* Timing::RegisterEvent(const std::string& name, TimedCallback callback) {
    // check for existing type with same name.
    // we want event type names to remain unique so that we can use them for serialization.
    auto info = event_types.emplace(name, TimingEventType{});
    TimingEventType* event_type = &info.first->second;
    event_type->name = &info.first->first;
    if (callback != nullptr) {
        event_type->callback = callback;
    }
    return event_type;
}

void Timing::ScheduleEvent(s64 cycles_into_future, const TimingEventType* event_type,
                           std::uintptr_t user_data, std::size_t core_id, bool thread_safe_mode) {
    if (event_queue_locked) {
        return;
    }

    ASSERT(event_type != nullptr);
    Timing::Timer* timer = nullptr;
    if (core_id == std::numeric_limits<std::size_t>::max()) {
        timer = current_timer;
    } else {
        ASSERT(core_id < timers.size());
        timer = timers.at(core_id).get();
    }

    if (thread_safe_mode) {
        // Events scheduled in thread safe mode come after blocking operations with
        // unpredictable timings in the host machine, so there is no need to be cycle accurate.
        // To prevent the event from scheduling before the next advance(), we set a minimum time
        // of MAX_SLICE_LENGTH * 2 cycles into the future.
        cycles_into_future = std::max(static_cast<s64>(MAX_SLICE_LENGTH * 2), cycles_into_future);
        ++hen_ts_pushed;

        timer->ts_queue.Push(Event{static_cast<s64>(timer->GetTicks() + cycles_into_future), 0,
                                   user_data, event_type});
    } else {
        s64 timeout = timer->GetTicks() + cycles_into_future;
        if (current_timer == timer) {
            // If this event needs to be scheduled before the next advance(), force one early
            if (!timer->is_timer_sane)
                timer->ForceExceptionCheck(cycles_into_future);

            timer->event_queue.emplace_back(
                Event{timeout, timer->event_fifo_id++, user_data, event_type});
            std::push_heap(timer->event_queue.begin(), timer->event_queue.end(), std::greater<>());
        } else {
            timer->ts_queue.Push(Event{static_cast<s64>(timer->GetTicks() + cycles_into_future), 0,
                                       user_data, event_type});
        }
    }
}

void Timing::UnscheduleEvent(const TimingEventType* event_type, std::uintptr_t user_data) {
    if (event_queue_locked) {
        return;
    }
    for (auto timer : timers) {
        auto itr = std::remove_if(
            timer->event_queue.begin(), timer->event_queue.end(),
            [&](const Event& e) { return e.type == event_type && e.user_data == user_data; });

        // Removing random items breaks the invariant so we have to re-establish it.
        if (itr != timer->event_queue.end()) {
            timer->event_queue.erase(itr, timer->event_queue.end());
            std::make_heap(timer->event_queue.begin(), timer->event_queue.end(), std::greater<>());
        }
    }
    // TODO:remove events from ts_queue
}

void Timing::RemoveEvent(const TimingEventType* event_type) {
    if (event_queue_locked) {
        return;
    }
    for (auto timer : timers) {
        auto itr = std::remove_if(timer->event_queue.begin(), timer->event_queue.end(),
                                  [&](const Event& e) { return e.type == event_type; });

        // Removing random items breaks the invariant so we have to re-establish it.
        if (itr != timer->event_queue.end()) {
            timer->event_queue.erase(itr, timer->event_queue.end());
            std::make_heap(timer->event_queue.begin(), timer->event_queue.end(), std::greater<>());
        }
    }
    // TODO:remove events from ts_queue
}

void Timing::SetCurrentTimer(std::size_t core_id) {
    current_timer = timers[core_id].get();
}

s64 Timing::GetTicks() const {
    return current_timer->GetTicks();
}

s64 Timing::GetGlobalTicks() const {
    const auto& timer =
        std::max_element(timers.cbegin(), timers.cend(), [](const auto& a, const auto& b) {
            return a->GetTicks() < b->GetTicks();
        });
    return (*timer)->GetTicks();
}

std::chrono::microseconds Timing::GetGlobalTimeUs() const {
    return std::chrono::microseconds{GetGlobalTicks() * 1000000 / BASE_CLOCK_RATE_ARM11};
}

std::shared_ptr<Timing::Timer> Timing::GetTimer(std::size_t cpu_id) {
    return timers[cpu_id];
}

Timing::Timer::Timer(s64 base_ticks) : executed_ticks(base_ticks) {}

Timing::Timer::~Timer() {
    MoveEvents();
}

u64 Timing::Timer::GetTicks() const {
    u64 ticks = static_cast<u64>(executed_ticks);
    if (!is_timer_sane) {
        ticks += slice_length - downcount;
    }
    return ticks;
}

void Timing::Timer::AddTicks(u64 ticks) {
    if (hen_fast_clock) [[unlikely]] {
        // (never nothing for something: a loop of one-tick charges must still pass time)
        const u64 charged = std::max<u64>(static_cast<u64>(ticks * cpu_clock_scale), ticks != 0);
        hen_n3ds_clock_ticks += ticks;
        hen_clock_charged += charged;
        downcount -= charged;
        return;
    }
    downcount -= static_cast<u64>(ticks * cpu_clock_scale);
}

// A system call's charge (svc.cpp, address_arbiter.cpp). In the fast mode the kernel runs at the
// fast clock too, and its charges cost the host nothing, so with the automatic clock they are
// always a third, whatever the factor for code is. (Xenoblade: one core is 90% lock
// calls' charges, 1070 M of 1167 M ticks per 5 s, and that core sets the frame rate: with the
// factor at the floor the title made 21 to 25 frames a second with the host at 90%.)
void Timing::Timer::AddKernelTicks(u64 ticks) {
    if (hen_fast_clock && hen_n3ds_clock == 2) [[unlikely]] {
        const u64 charged = std::max<u64>(
            static_cast<u64>(ticks * cpu_clock_scale * hen_clock_factor / HEN_CLOCK_CEILING),
            ticks != 0);
        hen_n3ds_clock_ticks += ticks;
        hen_clock_charged += charged;
        downcount -= charged;
        return;
    }
    AddTicks(ticks);
}

u64 Timing::Timer::GetIdleTicks() const {
    return static_cast<u64>(idled_cycles);
}

void Timing::Timer::ForceExceptionCheck(s64 cycles) {
    cycles = std::max<s64>(0, cycles);
    if (downcount > cycles) {
        slice_length -= downcount - cycles;
        downcount = cycles;
    }
}

void Timing::Timer::MoveEvents() {
    for (Event ev; ts_queue.Pop(ev);) {
        ++hen_ts_moved;
        ev.fifo_order = event_fifo_id++;
        event_queue.emplace_back(std::move(ev));
        std::push_heap(event_queue.begin(), event_queue.end(), std::greater<>());
    }
}

s64 Timing::Timer::GetMaxSliceLength() const {
    const auto& next_event = event_queue.begin();
    if (next_event != event_queue.end()) {
        ASSERT(next_event->time - executed_ticks > 0);
        return next_event->time - executed_ticks;
    }
    return MAX_SLICE_LENGTH;
}

void Timing::Timer::Advance() {
    MoveEvents();

    s64 cycles_executed = slice_length - downcount;
    idled_cycles = 0;
    executed_ticks += cycles_executed;
    slice_length = 0;
    downcount = 0;

    is_timer_sane = true;

    while (!event_queue.empty() && event_queue.front().time <= executed_ticks) {
        Event evt = std::move(event_queue.front());
        std::pop_heap(event_queue.begin(), event_queue.end(), std::greater<>());
        event_queue.pop_back();
        if (evt.type->callback != nullptr) {
            const HenSlow hen_slow{evt.type->name->c_str()};
#if defined(__SWITCH__) && HEN_DEV
            const unsigned long long hen_event_start = hen_emu_stats_on ? hen_jit_now_ns() : 0;
            evt.type->callback(evt.user_data, static_cast<int>(executed_ticks - evt.time));
            HenNoteEvent(evt.type, hen_event_start != 0 ? hen_jit_now_ns() - hen_event_start : 0);
#else
            evt.type->callback(evt.user_data, static_cast<int>(executed_ticks - evt.time));
#endif
        } else {
            LOG_ERROR(Core, "Event '{}' has no callback", *evt.type->name);
        }
    }

    is_timer_sane = false;
}

void Timing::Timer::SetNextSlice(s64 max_slice_length) {
    slice_length = max_slice_length;

    // Still events left (scheduled in the future)
    if (!event_queue.empty()) {
        slice_length = static_cast<int>(
            std::min<s64>(event_queue.front().time - executed_ticks, max_slice_length));
    }

    downcount = slice_length;
}

void Timing::Timer::Idle() {
    idled_cycles += downcount;
    downcount = 0;
}

s64 Timing::Timer::GetDowncount() const {
    return downcount;
}

} // namespace Core
