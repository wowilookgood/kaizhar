// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>

#include "citra_switch/session_report.h"
#ifdef __SWITCH__
#include "citra_switch/cache_clear.h"
#endif
#include "citra_switch/switch_platform.h"
#ifdef __SWITCH__
#include "common/logging/backend.h"
#endif

extern "C" void hen_boost_calls(unsigned* out); // hen/jit/hen_jit.c

namespace SessionReport {

namespace {

// (a test program on a computer builds this file too: its files go to the folder it runs in)
#ifdef __SWITCH__
#define REPORT_FOLDER "/switch/Kaizhar/log/"
#else
#define REPORT_FOLDER "./"
constexpr const char* DriverCacheDir = "./driver-cache/";
#endif
constexpr const char* File = REPORT_FOLDER "last-session.txt";
constexpr const char* OldFile = REPORT_FOLDER "last-session.old.txt";
constexpr int WriteEverySeconds = 30;
/// Azahar's log file is flushed this often by the writer thread (it is otherwise
/// flushed only by an Error line: a program that died lost up to a buffer of its last lines).
constexpr int FlushLogEverySeconds = 5;

std::mutex text_mutex; ///< text, written
std::string text;      ///< the whole report so far
std::size_t written;   ///< how much of it is in the file
std::mutex file_mutex; ///< one writer at a time
std::thread writer;
std::condition_variable wake;
bool stopping;
bool started;
bool write_now; ///< (under text_mutex) Begin wants the header on the card soon

// The title that runs (the emulation thread's only).
struct {
    bool running = false;
    std::chrono::steady_clock::time_point start;
    int docked = -1;
    unsigned long long blocks_last = 0;
    unsigned boost_seen[10]{};
    // totals
    unsigned intervals = 0, slow_intervals = 0;
    double speed_sum = 0, speed_min = 0;
    unsigned long long shown = 0, gaps40 = 0, gaps70 = 0, gaps120 = 0, pipelines = 0, blocks = 0, boost_ms = 0;
    double longest_gap_ms = 0;
} now;

void Put(const std::string& line) {
    std::lock_guard lock(text_mutex);
    text += line;
    text += '\n';
}

/// What is not in the file yet goes there. From the writer thread, and from the emulation
/// thread only between titles.
void Write() {
    std::lock_guard file_lock(file_mutex);
    std::string tail;
    {
        std::lock_guard lock(text_mutex);
        if (written >= text.size()) {
            return;
        }
        tail = text.substr(written);
        written = text.size();
    }
    const int fd = open(File, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) {
        return;
    }
    (void)!write(fd, tail.data(), tail.size());
    close(fd);
}

/// Wakes four times a second to see whether the fault handler asks for the logs' tails
/// (switch_platform.c: it must not touch this file's text or stdio itself); the table goes to
/// the card every thirty seconds, Azahar's log file is flushed every five.
void WriterLoop() {
    using Clock = std::chrono::steady_clock;
    auto last_write = Clock::now(), last_flush = last_write;
    std::unique_lock lock(text_mutex);
    while (!stopping) {
        wake.wait_for(lock, std::chrono::milliseconds(250));
        const bool asked = write_now;
        write_now = false;
        lock.unlock();
        const auto time = Clock::now();
#ifdef __SWITCH__
        if (hen_crash_flush == 1) {
            // (the emulation thread's `now` is read without a lock: that thread has died or
            // will not get further than the handler's return)
            Put(fmt::format("ended in a crash at {} s: see crash.txt in this folder", hen_crash_seconds));
            Write();
            Common::Log::HenFlushFile();
            hen_crash_flush = 2;
            last_write = last_flush = time;
        }
        if (time - last_flush >= std::chrono::seconds(FlushLogEverySeconds)) {
            last_flush = time;
            Common::Log::HenFlushFile();
        }
#endif
        if (asked || stopping || time - last_write >= std::chrono::seconds(WriteEverySeconds)) {
            last_write = time;
            Write();
        }
        lock.lock();
    }
}

/// Whether the session before this one ended in a crash: its table's last line says so.
bool PreviousCrashed() {
    const int fd = open(File, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char tail[400];
    const off_t size = lseek(fd, 0, SEEK_END);
    const off_t from = size > static_cast<off_t>(sizeof(tail) - 1) ? size - (sizeof(tail) - 1) : 0;
    lseek(fd, from, SEEK_SET);
    const ssize_t got = read(fd, tail, sizeof(tail) - 1);
    close(fd);
    if (got <= 0) {
        return false;
    }
    tail[got] = 0;
    return std::string(tail).find("ended in a crash at") != std::string::npos;
}

/// Files in a folder whose names start with prefix: how many, and their size together.
void Measure(const std::string& folder, const std::string& prefix, unsigned& count, unsigned long long& bytes,
             int depth = 0) {
    DIR* dir = opendir(folder.c_str());
    if (dir == nullptr) {
        return;
    }
    while (const dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string path = folder + (folder.back() == '/' ? "" : "/") + name;
        struct stat info {};
        if (stat(path.c_str(), &info) != 0) {
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            if (depth < 2) {
                Measure(path, prefix, count, bytes, depth + 1);
            }
        } else if (name.compare(0, prefix.size(), prefix) == 0) {
            count++;
            bytes += static_cast<unsigned long long>(info.st_size);
        }
    }
    closedir(dir);
}

std::string BoostLine(const unsigned* b) {
    unsigned c[9];
    sw_clock_record(c);
    std::string line = fmt::format(
        "boost: {}; CPU clock raised {} times, refused {}; system boost mode on {} times (last result 0x{:x}), "
        "ended {} times (last result 0x{:x})",
        b[8] == 0 ? (b[6] != 0 ? "SWITCHED OFF after a failed call" : "off") : "on",
        b[0], b[1], b[2], b[4], b[3], b[5]);
    if (b[6] != 0) {
        line += fmt::format("; the call that failed: {} the system's mode, result 0x{:x}",
                            b[6] == 1 ? "turning on" : "ending", b[7]);
    }
    if (c[0] != 0) {
        line += fmt::format("; clock controller: CPU {:.1f} MHz, GPU {:.1f} MHz now; last asked {:.1f} MHz "
                            "(result 0x{:x}, read back {:.1f}); back to {:.1f} MHz (result 0x{:x}); out of range {} times",
                            c[1] / 1e6, c[2] / 1e6, c[3] / 1e6, c[4], c[5] / 1e6, c[7] / 1e6, c[6], c[8]);
    } else {
        line += "; clock rates not read (the clock controller is opened by the first raise only)";
    }
    return line;
}

} // namespace

void Begin(const Title& title) {
    if (!started) {
        started = true;
        // (the folder is the emulator's log folder: there since the first start)
        const bool previous_crashed = PreviousCrashed();
        std::remove(OldFile);
        std::rename(File, OldFile);
        const std::time_t time = std::time(nullptr);
        char date[40] = "?";
        if (const std::tm* utc = std::gmtime(&time)) {
            std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", utc);
        }
        Put(fmt::format("{}, started {} UTC by the console's clock", SW_VERSION_TEXT, date));
        Put("(what ran and how it went, for a problem report; nothing here names the user or the network)");
        if (previous_crashed) {
            Put("the previous session ended in a crash: see crash.txt and last-session.old.txt in this folder");
            std::printf("hen: crash: the previous session ended in a crash: crash.txt in the log folder\n");
        }
        writer = std::thread(WriterLoop);
    }
    now = {};
    now.running = true;
    now.start = std::chrono::steady_clock::now();
    now.docked = sw_docked() ? 1 : 0;
    now.blocks_last = title.blocks_now;
    Put("");
    Put(fmt::format("title {:016X} \"{}\"", title.id, title.name));
    Put(fmt::format("mode: {}; internal resolution: {}; screens: {}", now.docked ? "docked" : "handheld",
                    title.resolution == 0 ? std::string("as shown") : fmt::format("{}x", title.resolution),
                    title.layout));
    unsigned shader_files = 0, driver_files = 0;
    unsigned long long shader_bytes = 0, driver_bytes = 0;
    if (!title.shader_dir.empty()) {
        Measure(title.shader_dir, fmt::format("{:016X}_", title.id), shader_files, shader_bytes);
    }
#ifdef __SWITCH__
    // (Kaizhar's own folder: cache_clear.h)
    Measure(Caches::DriverDir(), "", driver_files, driver_bytes);
#else
    Measure(DriverCacheDir, "", driver_files, driver_bytes);
#endif
    Put(fmt::format("caches at the start: shaders of this title {}; the driver's {}; compiled-block list {}",
                    shader_files == 0 ? std::string("none (cold)")
                                      : fmt::format("{} file(s), {} KiB", shader_files, shader_bytes / 1024),
                    driver_files == 0 ? std::string("none (cold)")
                                      : fmt::format("{} file(s), {} KiB", driver_files, driver_bytes / 1024),
                    !title.jit_list ? std::string("not in use")
                    : title.jit_listed == 0
                        ? std::string("none (cold)")
                        : fmt::format("{} blocks, {} compiled before the start", title.jit_listed,
                                      title.jit_compiled)));
    hen_boost_calls(now.boost_seen);
    Put("  time  mode  speed  game fps  shown  gaps >40/>70/>120 ms  longest  pipelines  new blocks  boost on");
    {
        std::lock_guard lock(text_mutex);
        write_now = true;
    }
    wake.notify_all();
}

void Add(const Interval& i) {
    if (!now.running) {
        return;
    }
    const int docked = sw_docked() ? 1 : 0;
    if (docked != now.docked) {
        Put(fmt::format("mode: {} -> {}", now.docked ? "docked" : "handheld", docked ? "docked" : "handheld"));
        now.docked = docked;
    }
    // The boost's calls, whenever a route is used for the first time or a call fails (and at the
    // title's end): a run closed from outside has it then too.
    unsigned b[10];
    hen_boost_calls(b);
    const bool news = (b[0] != 0) != (now.boost_seen[0] != 0) || (b[1] != 0) != (now.boost_seen[1] != 0) ||
                      (b[2] != 0) != (now.boost_seen[2] != 0) || b[6] != now.boost_seen[6] ||
                      b[8] != now.boost_seen[8];
    if (news) {
        Put(BoostLine(b));
    }
    std::copy(std::begin(b), std::end(b), std::begin(now.boost_seen));

    const unsigned long long blocks = i.blocks_total >= now.blocks_last ? i.blocks_total - now.blocks_last : 0;
    now.blocks_last = i.blocks_total;
    const long long at =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - now.start).count();
    Put(fmt::format("{:5} s  {}  {:5.0f}%  {:8.1f}  {:5}  {:>7}  {:7.0f} ms  {:9}  {:10}  {:5} ms", at,
                    docked ? "dock" : "hand", i.speed * 100.0, i.game_fps, i.shown,
                    fmt::format("{}/{}/{}", i.gaps40, i.gaps70, i.gaps120), i.longest_gap_ms, i.pipelines, blocks,
                    i.boost_on_ms));
    now.intervals++;
    now.slow_intervals += i.speed < 0.97;
    now.speed_sum += i.speed;
    now.speed_min = now.intervals == 1 ? i.speed : std::min(now.speed_min, i.speed);
    now.shown += i.shown;
    now.gaps40 += i.gaps40;
    now.gaps70 += i.gaps70;
    now.gaps120 += i.gaps120;
    now.longest_gap_ms = std::max(now.longest_gap_ms, i.longest_gap_ms);
    now.pipelines += i.pipelines;
    now.blocks += blocks;
    now.boost_ms += i.boost_on_ms;
}

void Note(const std::string& line) {
    if (now.running) {
        Put(line);
    }
}

void End(const char* how) {
    if (!now.running) {
        return;
    }
    now.running = false;
    const long long seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - now.start).count();
    unsigned b[10];
    hen_boost_calls(b);
    Put(BoostLine(b));
    Put(fmt::format("totals: {} s, ended: {}; {} intervals, speed {:.0f}% on average, {:.0f}% at the lowest, {} "
                    "under 97%; {} frames shown, gaps >40/>70/>120 ms: {}/{}/{}, longest {:.0f} ms; {} pipelines "
                    "compiled, {} new blocks; boost on {:.1f} s",
                    seconds, how, now.intervals, now.intervals ? now.speed_sum / now.intervals * 100.0 : 0.0,
                    now.speed_min * 100.0, now.slow_intervals, now.shown, now.gaps40, now.gaps70, now.gaps120,
                    now.longest_gap_ms, now.pipelines, now.blocks, now.boost_ms / 1000.0));
    Write(); // (between titles: no frame waits for this)
}

void Close() {
    if (!started) {
        return;
    }
    {
        std::lock_guard lock(text_mutex);
        stopping = true;
    }
    wake.notify_all();
    if (writer.joinable()) {
        writer.join();
    }
    Write();
}

} // namespace SessionReport
