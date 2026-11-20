// What dynarmic's Switch paths have come to need beyond shim.cpp.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
struct hen_jit;
extern "C" {
uint32_t* hen_jit_wptr(hen_jit* j);
uint32_t* hen_jit_xptr(hen_jit* j);
unsigned long long hen_jit_now_ns(void);
int hen_buffer_grow = 0, hen_jit_clear_at_ms = -1;
int hen_publish_batch = 32, hen_resort_at_ms = -1, hen_resort_chunk = 256 << 10, hen_resort_poison = 1;
int hen_resort_release = 1, hen_resort = 0, hen_emu_stats_on = 0;
unsigned long long hen_dispatch_count[5], hen_emu_ns[12], hen_emu_count[12];
std::atomic<unsigned long long> shim_resort_notes[8];
void hen_jit_note_batch(unsigned, int) {}
void hen_jit_note_block_place(unsigned long long, unsigned long long, unsigned long long) {}
void hen_jit_note_link_skip(int) {}
size_t hen_jit_write_first_size(void) { return 64 << 10; }
int hen_jit_write_full(hen_jit*) { return 0; }
void hen_layout_touch(const void*) {}
static std::atomic<int> claimed{0};
int hen_resort_claim(void) { int no = 0; return claimed.compare_exchange_strong(no, 1) ? 1 : 0; }
void hen_resort_unclaim(void) { claimed.store(0); }
unsigned hen_resort_number(void) { static std::atomic<unsigned> n{0}; return ++n; }
unsigned long long hen_resort_origin(void) { static const unsigned long long origin = hen_jit_now_ns(); return origin; }
unsigned long long hen_resort_busy_last(void) { return 0; }
void hen_resort_note_pause(unsigned long long) {}
void hen_resort_note(int kind, long long delta) { if (kind >= 0 && kind < 8) shim_resort_notes[kind] += static_cast<unsigned long long>(delta); }
int hen_jit_resort_prepare(hen_jit*, unsigned long long* map_ns) { *map_ns = 0; return 1; }
void hen_jit_resort_publish(hen_jit* j, size_t begin, size_t size, unsigned long long* ns, unsigned* requests, unsigned long long* longest) {
    uint8_t* const w = reinterpret_cast<uint8_t*>(hen_jit_wptr(j));
    uint8_t* const x = reinterpret_cast<uint8_t*>(hen_jit_xptr(j));
    pthread_jit_write_protect_np(0);
    std::memcpy(x + begin, w + begin, size);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(x + begin, size);
    *ns += 1; *requests += 1; if (*longest < 1) *longest = 1;
}
}
