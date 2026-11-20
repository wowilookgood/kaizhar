// Host stand-in for Azahar's hen/jit/hen_jit.c (macOS on arm64), for running dynarmic's Switch
// code paths natively: a write area and an execute area per buffer, with changes copied from
// the first to the second when the JIT reports them (as RO's writable view does on the console).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>

extern "C" {
int hen_link_table = 1, hen_direct_links = 1, hen_compact_links = 1;
int hen_translation_worker = 0, hen_worker_depth_limit = 4, hen_emu_ahead = 4;
int hen_tier = 0, hen_tier_force = 0, hen_bump = 16, hen_publish_early = 1;
int hen_fast_dispatch = 1, hen_fold_cond_branch = 1, hen_inline_exclusive = 0, hen_live_fpsr = 1;
int hen_jit_trace = 0, hen_exit_counts = 0;
unsigned long long hen_exit_count[8];
int shim_alias_active = 1;
std::atomic<unsigned long long> shim_compact[6], shim_links[3], shim_publishes, shim_ranges, shim_clears;

int hen_jit_alias_active(void) { return shim_alias_active; }
unsigned long long hen_jit_now_ns(void) {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void hen_jit_note_block_ticks(unsigned, int) {}
void hen_jit_note_cache_clear(void) { shim_clears++; }
void hen_jit_note_compact(int kind, unsigned long long count) { shim_compact[kind] += count; }
void hen_jit_note_compile(unsigned long long, unsigned long long, unsigned) {}
void hen_jit_note_early_publish(void) {}
void hen_jit_note_emit_parts(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long) {}
void hen_jit_note_link(int kind) { shim_links[kind]++; }
void hen_jit_note_link_upgrades(unsigned long long count) { shim_links[2] += count; }
void hen_jit_note_lock(unsigned long long) {}
void hen_jit_note_lookup(void) {}
void hen_jit_note_miss(int, unsigned long long, unsigned long long) {}
void hen_jit_note_split(unsigned long long, unsigned long long) {}
void hen_jit_note_tier(int) {}
std::atomic<unsigned long long> shim_worker_blocks;
void hen_jit_note_translation(unsigned made, unsigned, unsigned) { shim_worker_blocks += made; }
void hen_jit_note_wake_skipped(void) {}
void hen_jit_record_block(unsigned, unsigned long long, unsigned, unsigned) {}
void hen_jit_worker_creating(void) {}
void hen_jit_worker_started(void) {}

struct hen_jit {
    size_t size;
    uint8_t* write;
    uint8_t* exec;
    bool writing = false;
    std::vector<std::pair<size_t, size_t>> dirty;
    std::mutex lock;
};

static void sync(hen_jit* j) {
    if (j->dirty.empty()) {
        return;
    }
    pthread_jit_write_protect_np(0);
    for (const auto& [begin, end] : j->dirty) {
        std::memcpy(j->exec + begin, j->write + begin, end - begin);
    }
    pthread_jit_write_protect_np(1);
    for (const auto& [begin, end] : j->dirty) {
        sys_icache_invalidate(j->exec + begin, end - begin);
    }
    shim_publishes++;
    shim_ranges += j->dirty.size();
    j->dirty.clear();
}

hen_jit* hen_jit_create(size_t size) {
    auto* j = new hen_jit;
    j->size = size;
    j->write = static_cast<uint8_t*>(std::calloc(1, size));
    j->exec = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0));
    if (j->exec == MAP_FAILED || j->write == nullptr) {
        std::perror("hen_jit_create");
        std::abort();
    }
    pthread_jit_write_protect_np(1);
    return j;
}
void hen_jit_destroy(hen_jit* j) {
    munmap(j->exec, j->size);
    std::free(j->write);
    delete j;
}
uint32_t* hen_jit_wptr(hen_jit* j) { return reinterpret_cast<uint32_t*>(j->write); }
uint32_t* hen_jit_xptr(hen_jit* j) { return reinterpret_cast<uint32_t*>(j->exec); }
size_t hen_jit_usable_size(hen_jit* j) { return j->size; }
void hen_jit_rewind(hen_jit*, size_t) {}
void hen_jit_writable(hen_jit* j) {
    std::scoped_lock lock{j->lock};
    j->writing = true;
}
void hen_jit_executable(hen_jit* j) {
    std::scoped_lock lock{j->lock};
    j->writing = false;
    sync(j);
}
void hen_jit_invalidate(hen_jit* j, uint32_t* xmem, size_t size) {
    const size_t begin = static_cast<size_t>(reinterpret_cast<uint8_t*>(xmem) - j->exec);
    if (size == 0 || begin >= j->size) {
        return;
    }
    const size_t end = begin + size < j->size ? begin + size : j->size;
    std::scoped_lock lock{j->lock};
    // (as hen_jit.c: only the last range is extended)
    if (!j->dirty.empty() && begin >= j->dirty.back().first && begin <= j->dirty.back().second) {
        if (end > j->dirty.back().second) {
            j->dirty.back().second = end;
        }
    } else {
        j->dirty.emplace_back(begin, end);
    }
    if (!j->writing) {
        sync(j);
    }
}
}
