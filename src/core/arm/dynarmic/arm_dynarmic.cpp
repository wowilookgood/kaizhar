// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <csignal>
#include <cstring>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/optimization_flags.h>
#include "common/hen_stats.h"
#include "common/assert.h"
#include "common/microprofile.h"
#ifdef __SWITCH__
#include "core/arm/dynarmic/hen_block_list.h"
#endif
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_cp15.h"
#include "core/arm/dynarmic/arm_exclusive_monitor.h"
#include "core/arm/dynarmic/arm_tick_counts.h"
#ifdef __SWITCH__
#include <algorithm>
#include <condition_variable>
#include <iterator>
#include <cstdio>
#include <mutex>
#include <pthread.h>
#include <vector>
#include "core/arm/dyncom/arm_dyncom_interpreter.h"
#include "core/arm/dyncom/arm_dyncom_trans.h"
#include "core/arm/skyeye_common/armstate.h"
#endif
#include "core/arm/exception_handler.h"
#include "core/core.h"
#include "core/core_timing.h"
#ifdef ENABLE_GDBSTUB
#include "core/gdbstub/gdbstub.h"
#endif
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

#ifndef SIGILL
constexpr u32 SIGILL = 4;
#endif

#ifndef SIGTRAP
constexpr u32 SIGTRAP = 5;
#endif

#ifdef __SWITCH__
// hen/jit/hen_jit.c: statistics.
extern "C" unsigned long long hen_jit_now_ns(void);
extern "C" int hen_code_cache_mib; // hen/jit/hen_jit.c: a JIT's code buffer, MiB
extern "C" int hen_buffer_sized;   // --jit-buffer-sized
#endif

#if HEN_DEV
extern "C" void hen_jit_note_run(unsigned long long ns);
// ("hen: emu:": hen/jit/hen_jit.c) 1 system calls, 5 and 6 memory reads and writes that
// left generated code.
extern "C" unsigned long long hen_emu_ns[12], hen_emu_count[12];
// ("hen: emu svc:": hen/jit/hen_jit.c; reported and emptied by kernel/svc.cpp)
extern "C" unsigned long long hen_svc_seen[256], hen_svc_seen_ns[256];
extern "C" void hen_blackbox_dump(unsigned ms); // hen/jit/hen_jit.c
#endif

namespace Core {

// The counters below are for the development builds' reports: no other build has them, and
// the callbacks of generated code count nothing there.
#if HEN_DEV
/// (Switch 2 port) Memory reads that left generated code, by megabyte of address: which
/// region the fast paths don't cover (hen_jit_slow_reads, printed with the statistics).
static std::array<u64, 2> hen_mem_calls{};
extern "C" void hen_gfx_add(int kind, unsigned long long count);
static std::array<u32, 4096> hen_slow_read_megabytes{};
/// Exclusive stores that succeeded and that failed; system calls by number.
static std::array<u32, 2> hen_exclusive_writes{};
static std::array<u32, 256> hen_svc_counts{};
static std::array<u64, 256> hen_svc_ns{};
static u64 hen_code_ticks = 0;
static u64 hen_code_ticks_total = 0; // (never reset: the benchmark marks in svc.cpp read it)
extern "C" unsigned long long hen_jit_code_ticks_total(void) {
    return hen_code_ticks_total;
}
extern "C" unsigned long long hen_jit_code_ticks(void) {
    return std::exchange(hen_code_ticks, 0);
}
/// The system calls that took the most time since the last call, and the time in all of them.
extern "C" unsigned long long hen_jit_svc_times(unsigned* svc, unsigned long long* ns,
                                                unsigned* count, int top) {
    unsigned long long total = 0;
    for (const u64 n : hen_svc_ns) {
        total += n;
    }
    for (int i = 0; i < top; i++) {
        svc[i] = 0;
        ns[i] = 0;
        for (unsigned n = 0; n < 256; n++) {
            if (hen_svc_ns[n] > ns[i]) {
                svc[i] = n;
                ns[i] = hen_svc_ns[n];
            }
        }
        count[i] = hen_svc_counts[svc[i]];
        hen_svc_ns[svc[i]] = 0;
    }
    hen_svc_ns.fill(0);
    return total;
}
extern "C" void hen_jit_guest_stats(unsigned* exclusive, unsigned* svc, unsigned* svc_count, int top) {
    exclusive[0] = hen_exclusive_writes[0];
    exclusive[1] = hen_exclusive_writes[1];
    hen_exclusive_writes.fill(0);
    for (int i = 0; i < top; i++) {
        svc[i] = svc_count[i] = 0;
        for (unsigned n = 0; n < 256; n++) {
            if (hen_svc_counts[n] > svc_count[i]) {
                svc[i] = n;
                svc_count[i] = hen_svc_counts[n];
            }
        }
        hen_svc_counts[svc[i]] = 0;
    }
    hen_svc_counts.fill(0);
}
static std::array<u32, 4096> hen_slow_read_last{};
extern "C" unsigned hen_jit_slow_read_last(unsigned megabyte) {
    return hen_slow_read_last[megabyte & 4095];
}
extern "C" void hen_jit_slow_reads(unsigned* megabyte, unsigned* count, int top) {
    hen_gfx_add(HEN_GFX_MEM_READ_CALL, hen_mem_calls[0]);
    hen_gfx_add(HEN_GFX_MEM_WRITE_CALL, hen_mem_calls[1]);
    hen_mem_calls.fill(0);
    for (int i = 0; i < top; i++) {
        megabyte[i] = count[i] = 0;
        for (unsigned m = 0; m < hen_slow_read_megabytes.size(); m++) {
            if (hen_slow_read_megabytes[m] > count[i]) {
                megabyte[i] = m;
                count[i] = hen_slow_read_megabytes[m];
            }
        }
        hen_slow_read_megabytes[megabyte[i]] = 0;
    }
    hen_slow_read_megabytes.fill(0);
}
static inline void HenNoteRead(VAddr vaddr) {
    hen_slow_read_megabytes[vaddr >> 20]++; // (plain counters: this is every LDREX too)
    hen_slow_read_last[vaddr >> 20] = vaddr;
    hen_mem_calls[0]++;
    hen_emu_count[5]++;
}
static inline void HenNoteWrite() {
    hen_mem_calls[1]++;
    hen_emu_count[6]++;
}
static inline void HenNoteExclusiveWrite(bool stored) {
    hen_exclusive_writes[stored ? 0 : 1]++;
}
#else
static inline void HenNoteRead(VAddr) {}
static inline void HenNoteWrite() {}
static inline void HenNoteExclusiveWrite(bool) {}
#endif

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit DynarmicUserCallbacks(ARM_Dynarmic& parent)
        : parent(parent), svc_context(parent.system), memory(parent.memory) {}
    ~DynarmicUserCallbacks() = default;

    std::optional<std::uint32_t> MemoryReadCode(VAddr vaddr) override {
        return memory.Read32OrNullopt(vaddr);
    }

    std::uint8_t MemoryRead8(VAddr vaddr) override {
        HenNoteRead(vaddr);
        return memory.Read8(vaddr);
    }
    std::uint16_t MemoryRead16(VAddr vaddr) override {
        HenNoteRead(vaddr);
        return memory.Read16(vaddr);
    }
    std::uint32_t MemoryRead32(VAddr vaddr) override {
        HenNoteRead(vaddr);
        return memory.Read32(vaddr);
    }
    std::uint64_t MemoryRead64(VAddr vaddr) override {
        HenNoteRead(vaddr);
        return memory.Read64(vaddr);
    }

    void MemoryWrite8(VAddr vaddr, std::uint8_t value) override {
        HenNoteWrite();
        memory.Write8(vaddr, value);
    }
    void MemoryWrite16(VAddr vaddr, std::uint16_t value) override {
        HenNoteWrite();
        memory.Write16(vaddr, value);
    }
    void MemoryWrite32(VAddr vaddr, std::uint32_t value) override {
        HenNoteWrite();
        memory.Write32(vaddr, value);
    }
    void MemoryWrite64(VAddr vaddr, std::uint64_t value) override {
        HenNoteWrite();
        memory.Write64(vaddr, value);
    }

    bool MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) override {
        const bool stored = memory.WriteExclusive8(vaddr, value, expected);
        HenNoteExclusiveWrite(stored);
        return stored;
    }
    bool MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) override {
        const bool stored = memory.WriteExclusive16(vaddr, value, expected);
        HenNoteExclusiveWrite(stored);
        return stored;
    }
    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) override {
        const bool stored = memory.WriteExclusive32(vaddr, value, expected);
        HenNoteExclusiveWrite(stored);
        return stored;
    }
    bool MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) override {
        const bool stored = memory.WriteExclusive64(vaddr, value, expected);
        HenNoteExclusiveWrite(stored);
        return stored;
    }

    void InterpreterFallback(VAddr pc, std::size_t num_instructions) override {
        // Should never happen.
        UNREACHABLE_MSG("InterpeterFallback reached with pc = 0x{:08x}, code = 0x{:08x}, num = {}",
                        pc, MemoryReadCode(pc).value(), num_instructions);
    }

    void CallSVC(std::uint32_t swi) override {
#if HEN_DEV
        hen_svc_counts[swi & 0xff]++;
        const unsigned long long hen_svc_start = hen_jit_now_ns();
        svc_context.CallSVC(swi);
        const unsigned long long hen_svc_took = hen_jit_now_ns() - hen_svc_start;
        hen_svc_ns[swi & 0xff] += hen_svc_took;
        hen_emu_ns[1] += hen_svc_took;
        hen_emu_count[1]++;
        hen_svc_seen[swi & 0xff]++;
        hen_svc_seen_ns[swi & 0xff] += hen_svc_took;
#else
        svc_context.CallSVC(swi);
#endif
    }

    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::UndefinedInstruction:
        case Dynarmic::A32::Exception::UnpredictableInstruction:
        case Dynarmic::A32::Exception::DecodeError:
        case Dynarmic::A32::Exception::NoExecuteFault:
            break;
        case Dynarmic::A32::Exception::Breakpoint:
#ifdef ENABLE_GDBSTUB
            if (GDBStub::IsConnected()) {
                parent.SetPC(pc);
                parent.ServeBreak(SIGTRAP);
                return;
            }
#endif
            break;
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForInterrupt:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::Yield:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadInstruction:
            return;
        }

        static constexpr auto ExceptionToString = [](Dynarmic::A32::Exception e) -> std::string {
            switch (e) {
            case Dynarmic::A32::Exception::UndefinedInstruction:
                return "UndefinedInstruction";
            case Dynarmic::A32::Exception::UnpredictableInstruction:
                return "UnpredictableInstruction";
            case Dynarmic::A32::Exception::DecodeError:
                return "DecodeError";
            case Dynarmic::A32::Exception::NoExecuteFault:
                return "NoExecuteFault";
            case Dynarmic::A32::Exception::Breakpoint:
                return "Breakpoint";
            default:
                return fmt::format("Unknown({})", e);
            }
        };

        parent.SetPC(pc);
#ifdef ENABLE_GDBSTUB
        if (GDBStub::IsConnected()) {
            parent.ServeBreak(SIGILL);
        } else
#endif
        {
            Core::ExceptionType exc_type;
            switch (exception) {
            case Dynarmic::A32::Exception::UndefinedInstruction:
            case Dynarmic::A32::Exception::UnpredictableInstruction:
            case Dynarmic::A32::Exception::DecodeError:
            case Dynarmic::A32::Exception::NoExecuteFault:
                exc_type = Core::ExceptionType::UndefinedInstruction;
                break;
            case Dynarmic::A32::Exception::Breakpoint:
                exc_type = Core::ExceptionType::Break;
                break;
            default:
                exc_type = Core::ExceptionType::UndefinedInstruction;
                break;
            }
            Core::LogException(parent.system, exc_type);
        }
    }

    void AddTicks(std::uint64_t ticks) override {
#if HEN_DEV
        hen_code_ticks_total += ticks;
        hen_code_ticks += ticks; // (ticks of executed program code only: system calls charge theirs elsewhere)
#endif
        parent.GetTimer().AddTicks(ticks);
    }
    std::uint64_t GetTicksRemaining() override {
        s64 ticks = parent.GetTimer().GetDowncount();
        return static_cast<u64>(ticks <= 0 ? 0 : ticks);
    }
    std::uint64_t GetTicksForCode(bool is_thumb, VAddr, std::uint32_t instruction) override {
        return Core::TicksForInstruction(is_thumb, instruction);
    }

    ARM_Dynarmic& parent;
    Kernel::SVCContext svc_context;
    Memory::MemorySystem& memory;
};

#ifdef __SWITCH__
static void HenCoreAlive(ARM_Dynarmic* core);
#endif

ARM_Dynarmic::ARM_Dynarmic(Core::System& system_, Memory::MemorySystem& memory_, u32 core_id_,
                           std::shared_ptr<Core::Timing::Timer> timer_,
                           Core::ExclusiveMonitor& exclusive_monitor_)
    : ARM_Interface(core_id_, timer_), system(system_), memory(memory_),
      cb(std::make_unique<DynarmicUserCallbacks>(*this)),
      exclusive_monitor{dynamic_cast<Core::DynarmicExclusiveMonitor&>(exclusive_monitor_)} {
#ifdef __SWITCH__
    HenCoreAlive(this);
#endif
    SetPageTable(memory.GetCurrentPageTable());
}

#ifdef __SWITCH__
#if HEN_DEV
extern "C" int hen_jit_trace; // --jit-trace
#else
static constexpr int hen_jit_trace = 0;
#endif
namespace {
// The interpreter's decoded instructions are in one buffer for all cores (trans_cache_buf).
std::vector<ARMul_State*> hen_interp_states;
void HenInterpForget(ARMul_State* state) {
    std::erase(hen_interp_states, state);
}
} // namespace

// A process that has ended leaves its JIT behind upstream: the JITs are kept by page table and
// never dropped while the emulated system runs. Each has its own 128 MiB of code space here,
// which is real memory on this platform, and a console that starts applets over and over (the
// keyboard, each time a text field is touched) runs out of it. So the kernel tells when a
// process's page table goes (core/memory.cpp, UnregisterPageTable), and each core drops its
// JIT for it the next time it changes page tables: then it is outside generated code, and the
// table is not the one it is about to run. An entry stays until every core has seen it.
namespace {
struct EndedTable {
    std::shared_ptr<Memory::PageTable> table;
    u32 seen = 0; ///< a bit per core that has dropped its JIT for it
};
std::vector<EndedTable> hen_ended_tables;
int hen_cores_alive = 0;
unsigned hen_jits_dropped = 0;
} // namespace

// Parked JITs (hen_jit_park; --no-jit-park: none).
//
// An application that opens the console's keyboard starts that title as a new process each
// time and ends it each time, and each new process would have a new JIT: the keyboard's code
// translated again at every open (System Settings: 6 to 11 ms of compiling and 6 to 16
// ms of interpreting in the first gap after each open, more in the one or two after it), a
// new 128 MiB code buffer made, and the old JIT taken down. Instead the JITs of an
// ended process are kept ("parked") with the process's page table, and the next process that
// is the same program at the same addresses gets that page table and so finds the JITs.
//
// What makes handing translated code to another process right:
//  - What the generated code holds of its process (MakeJit below, and dynarmic's
//    A32AddressSpace::EmitPrelude): the addresses of the page table's two pointer arrays, as
//    constants; nothing else. The callbacks, the CP15 state and the exclusive monitor belong
//    to the core, which stays. So the new process is given the SAME PageTable object, emptied
//    (Kernel::Process::HenParkedJits, before anything is mapped), and nothing in the code
//    needs changing. That is also what the compile worker reads the program's code through.
//  - Which process may take it: one whose code set has the same title ID, the same hash of
//    its whole image as the loader made it (CodeSet::hen_image_hash, 64 bits), the same
//    segment addresses and sizes and the same entry point (HenParkKey).
//  - Which blocks it keeps: before a taken-over JIT runs anything (Run, Step), every block is
//    checked against the code now in the new process's memory, by the hash of the guest code
//    it was compiled from (dynarmic, AddressSpace::HenRevalidate; the hash the block list
//    uses). A block compiled from anything else (code the old process changed or loaded
//    later, memory that is not mapped now) is invalidated the way a write to its code
//    invalidates it. So a kept block is the translation of bytes that are there.
//  - What is still assumed: that a program which changes its code afterwards says so (a
//    cache flush, a module load: System::InvalidateCacheRange). The compile worker and the
//    saved block list assume the same for every title: they compile code the program has
//    not reached yet from what is in memory at that moment.
//  - Between the two processes nothing is compiled or read: HenPark stops the JIT's worker
//    from compiling until the check is done.
// Bounds: at most HEN_PARK_MAX_JITS parked JITs in all (each holds its code buffer: 128 MiB
// of address space and two heap blocks of that size, hen/jit/hen_jit.c); a process with more
// JITs than that is not parked, and the oldest parked process makes room (it goes the
// ordinary way: HenPurgeEndedJits, the teardown thread). Whatever is parked when the cores
// are destroyed is destroyed with them, as every other JIT is.
extern "C" int hen_jit_park; // hen/jit/hen_jit.c
namespace {
constexpr std::size_t HEN_PARK_MAX_JITS = 4;
struct HenParkKey {
    u64 program_id;
    u64 image_hash;
    std::array<u32, 7> layout; ///< code, read-only data, data: address and size; entry point
    bool operator==(const HenParkKey&) const = default;
};
struct HenParkedTable {
    std::shared_ptr<Memory::PageTable> table;
    HenParkKey key;
    std::size_t jits;
};
std::vector<ARM_Dynarmic*> hen_cores;
/// The page tables of running processes that may be parked when they end.
std::vector<std::pair<const Memory::PageTable*, HenParkKey>> hen_park_keys;
std::vector<HenParkedTable> hen_parked; ///< oldest first
struct {
    unsigned parked, taken, made_room, too_many;
    unsigned long long kept, dropped, check_ns;
} hen_park_stats;
} // namespace

struct HenParkAccess {
    static bool Has(ARM_Dynarmic* core, const std::shared_ptr<Memory::PageTable>& table) {
        return core->jits.find(table) != core->jits.end();
    }
    static Dynarmic::A32::Jit* Jit(ARM_Dynarmic* core,
                                   const std::shared_ptr<Memory::PageTable>& table) {
        const auto found = core->jits.find(table);
        return found == core->jits.end() ? nullptr : found->second.get();
    }
    static void TakenOver(ARM_Dynarmic* core, Dynarmic::A32::Jit* jit, u64 program_id) {
        std::erase_if(core->hen_taken_over,
                      [jit](const ARM_Dynarmic::HenTakenOver& t) { return t.jit == jit; });
        core->hen_taken_over.push_back({jit, program_id});
    }
};

static void HenCoreAlive(ARM_Dynarmic* core) {
    hen_cores_alive++;
    hen_cores.push_back(core);
}

/// The process of this page table has ended. True: its JITs stay where they are, parked.
static bool HenParkTable(const std::shared_ptr<Memory::PageTable>& table, const HenParkKey& key) {
    std::size_t jits = 0;
    for (ARM_Dynarmic* core : hen_cores) {
        jits += HenParkAccess::Has(core, table) ? 1 : 0;
    }
    if (jits == 0) {
        return false; // (it never ran)
    }
    if (jits > HEN_PARK_MAX_JITS) {
        hen_park_stats.too_many++;
        return false;
    }
    std::size_t held = 0;
    for (const HenParkedTable& parked : hen_parked) {
        held += parked.jits;
    }
    while (!hen_parked.empty() && held + jits > HEN_PARK_MAX_JITS) {
        // The oldest goes the way of every ended process.
        held -= hen_parked.front().jits;
        hen_ended_tables.push_back({std::move(hen_parked.front().table), 0});
        hen_parked.erase(hen_parked.begin());
        hen_park_stats.made_room++;
    }
    for (ARM_Dynarmic* core : hen_cores) {
        if (Dynarmic::A32::Jit* const jit = HenParkAccess::Jit(core, table)) {
            jit->HenPark();
        }
    }
    hen_parked.push_back({table, key, jits});
    hen_park_stats.parked++;
#if HEN_DEV
    std::printf("hen: jit: the %zu JIT(s) of the ended process of title %016llx are kept for its "
                "next start (%zu parked JIT(s) now, at most %zu)\n",
                jits, static_cast<unsigned long long>(key.program_id), held + jits,
                HEN_PARK_MAX_JITS);
#endif
    return true;
}

void HenForgetPageTable(const std::shared_ptr<Memory::PageTable>& table) {
    if (hen_cores_alive > 0 && table) {
        const auto key = std::find_if(hen_park_keys.begin(), hen_park_keys.end(),
                                      [&](const auto& k) { return k.first == table.get(); });
        if (key != hen_park_keys.end()) {
            const HenParkKey park_key = key->second;
            hen_park_keys.erase(key);
            // (not while the title is being shut down: System::Shutdown ends every process
            // before it destroys the cores, and nothing will start again)
            if (hen_jit_park && Core::System::GetInstance().IsPoweredOn() &&
                HenParkTable(table, park_key)) {
                return;
            }
        }
        hen_ended_tables.push_back({table, 0});
    }
}

/// From the kernel, for a process being created from a code set whose image hash is known
/// (a title started while another runs), before anything is mapped: the page table of an
/// ended process that was the same program at the same addresses, if one is parked. The
/// caller makes it the new process's page table and empties it. Each core's JIT for it is
/// checked against the new code before it runs (HenCheckTakenOver).
std::shared_ptr<Memory::PageTable> HenParkTake(u64 program_id, u64 image_hash,
                                               const std::array<u32, 7>& layout) {
    if (!hen_jit_park || hen_cores_alive <= 0 || image_hash == 0) {
        return nullptr;
    }
    const HenParkKey key{program_id, image_hash, layout};
    // (the newest first: the one most likely to have all of the program compiled)
    for (auto parked = hen_parked.rbegin(); parked != hen_parked.rend(); ++parked) {
        if (parked->key == key) {
            std::shared_ptr<Memory::PageTable> table = std::move(parked->table);
            hen_parked.erase(std::next(parked).base());
            for (ARM_Dynarmic* core : hen_cores) {
                if (Dynarmic::A32::Jit* const jit = HenParkAccess::Jit(core, table)) {
                    HenParkAccess::TakenOver(core, jit, program_id);
                }
            }
            hen_park_stats.taken++;
            return table;
        }
    }
    return nullptr;
}

/// From the kernel, once the new process has its page table (a parked one or its own): when
/// this process ends, its JITs may be parked under this description.
void HenParkRegister(const std::shared_ptr<Memory::PageTable>& table, u64 program_id,
                     u64 image_hash, const std::array<u32, 7>& layout) {
    if (!hen_jit_park || hen_cores_alive <= 0 || image_hash == 0 || !table) {
        return;
    }
    std::erase_if(hen_park_keys, [&](const auto& k) { return k.first == table.get(); });
    hen_park_keys.push_back({table.get(), HenParkKey{program_id, image_hash, layout}});
}

void ARM_Dynarmic::HenCheckTakenOver() {
    const auto mine = std::find_if(hen_taken_over.begin(), hen_taken_over.end(),
                                   [this](const HenTakenOver& t) { return t.jit == jit; });
    if (mine == hen_taken_over.end()) {
        return;
    }
    const u64 program_id = mine->program_id;
    hen_taken_over.erase(mine);
    const unsigned long long start = hen_jit_now_ns();
    std::size_t dropped = 0;
    const std::size_t kept = jit->HenRevalidate(&dropped);
    const unsigned long long ns = hen_jit_now_ns() - start;
    hen_park_stats.kept += kept;
    hen_park_stats.dropped += dropped;
    hen_park_stats.check_ns += ns;
#if HEN_DEV
    std::printf("hen: jit: core %u: the new process of title %016llx runs on the JIT of the one "
                "before it: %zu block(s) kept, %zu not (their code is not what it was); checked "
                "in %.1f ms\n",
                static_cast<unsigned>(GetID()), static_cast<unsigned long long>(program_id), kept,
                dropped, ns / 1e6);
#else
    (void)program_id;
#endif
}

// Taking an ended process's JIT down is not cheap: its compile worker is stopped
// and joined, its tables of blocks and links are freed entry by entry, its code is unmapped and
// its two 128 MiB blocks go back to the allocator. Measured in System Settings (each
// close of the keyboard): 51 to 96 ms on the emulation thread, about 8 ms per MiB of code the
// applet had compiled. So the emulation thread only takes the JIT out of the block list and
// out of its core's map (after which nothing on it can reach the JIT), and one thread kept
// for this destroys it:
//  - the JIT's own worker may still be running until the destructor joins it; it reads the
//    program's memory through this core's callbacks, as it did before the process ended, so
//    the queue is emptied and the thread joined before a core goes (~ARM_Dynarmic, which
//    System::Shutdown reaches before the memory is destroyed);
//  - the page table travels with the JIT and is let go after it, in the order the map's
//    erase had;
//  - unmapping takes hen_jit.c's lock, which every publish takes: the two wait for each other;
//  - at most HEN_TEARDOWN_MAX_PENDING JITs wait; past that the emulation thread destroys the
//    JIT itself, so code mappings and 128 MiB blocks cannot pile up behind a thread
//    that gets no core;
//  - the thread is started on first use, and joined when a core is destroyed: none is left at
//    exit.
// hen_jit_teardown_thread 0 (--no-jit-teardown-thread): on the emulation thread.
extern "C" int hen_jit_teardown_thread;          // hen/jit/hen_jit.c
extern "C" void hen_jit_teardown_creating(void); // (the thread that starts it: which core it is on)
extern "C" void hen_jit_teardown_started(void);  // (the new thread: another core, lower priority)
namespace {
constexpr std::size_t HEN_TEARDOWN_MAX_PENDING = 8;
struct EndedJit {
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::shared_ptr<Memory::PageTable> table;
};
struct HenTeardown {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<EndedJit> queue;
    pthread_t thread{};
    bool started = false;
    bool stop = false;
    // (for the line at the end)
    unsigned done = 0, on_emulation_thread = 0;
    unsigned long long ns = 0, longest_ns = 0;
};
HenTeardown hen_teardown;

void* HenTeardownMain(void*) {
    hen_jit_teardown_started();
    std::unique_lock lock{hen_teardown.mutex};
    for (;;) {
        hen_teardown.wake.wait(lock,
                               [] { return hen_teardown.stop || !hen_teardown.queue.empty(); });
        if (hen_teardown.queue.empty()) {
            return nullptr; // (asked to stop, and nothing is waiting)
        }
        EndedJit ended = std::move(hen_teardown.queue.front());
        hen_teardown.queue.erase(hen_teardown.queue.begin());
        lock.unlock();
        const unsigned long long start = hen_jit_now_ns();
        ended.jit.reset();
        ended.table.reset();
        const unsigned long long ns = hen_jit_now_ns() - start;
#if HEN_DEV
        std::printf("hen: jit: the JIT of an ended process was taken down in %.1f ms on the "
                    "teardown thread\n",
                    ns / 1e6);
#endif
        lock.lock();
        hen_teardown.done++;
        hen_teardown.ns += ns;
        hen_teardown.longest_ns = std::max(hen_teardown.longest_ns, ns);
    }
}

/// From the emulation thread: true if the teardown thread now owns the JIT; false if the
/// caller still does (the switch is off, the queue is full or the thread did not start).
bool HenTeardownHandOver(std::unique_ptr<Dynarmic::A32::Jit>& jit,
                         const std::shared_ptr<Memory::PageTable>& table) {
    if (!hen_jit_teardown_thread) {
        return false;
    }
    {
        std::scoped_lock lock{hen_teardown.mutex};
        if (hen_teardown.queue.size() >= HEN_TEARDOWN_MAX_PENDING) {
            hen_teardown.on_emulation_thread++;
            return false;
        }
        if (!hen_teardown.started) {
            // (its own stack size, as the JITs' workers have: the default is small, and a
            // JIT's tables are freed by functions that call themselves)
            hen_jit_teardown_creating();
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
            hen_teardown.stop = false;
            hen_teardown.started =
                pthread_create(&hen_teardown.thread, &attr, HenTeardownMain, nullptr) == 0;
            pthread_attr_destroy(&attr);
            if (!hen_teardown.started) {
                hen_teardown.on_emulation_thread++;
                return false;
            }
        }
        hen_teardown.queue.push_back({std::move(jit), table});
    }
    hen_teardown.wake.notify_one();
    return true;
}

/// Before a core goes: what is waiting is destroyed, and the thread ends and is joined.
void HenTeardownFinish() {
    {
        std::scoped_lock lock{hen_teardown.mutex};
        if (!hen_teardown.started) {
            return;
        }
        hen_teardown.stop = true;
    }
    hen_teardown.wake.notify_all();
    pthread_join(hen_teardown.thread, nullptr);
    std::scoped_lock lock{hen_teardown.mutex};
    hen_teardown.started = false;
    hen_teardown.stop = false;
#if HEN_DEV
    std::printf("hen: jit: teardown thread joined: %u JIT(s) of ended processes taken down on it, "
                "%.1f ms in all, longest %.1f ms; %u on the emulation thread instead (more than "
                "%zu waiting, or no thread)\n",
                hen_teardown.done, hen_teardown.ns / 1e6, hen_teardown.longest_ns / 1e6,
                hen_teardown.on_emulation_thread, HEN_TEARDOWN_MAX_PENDING);
#endif
    hen_teardown.done = hen_teardown.on_emulation_thread = 0;
    hen_teardown.ns = hen_teardown.longest_ns = 0;
}
} // namespace

void ARM_Dynarmic::HenPurgeEndedJits(const Dynarmic::A32::Jit* in_use) {
    if (hen_ended_tables.empty()) {
        return;
    }
    const u32 bit = 1u << GetID();
    const u32 all = (1u << system.GetNumCores()) - 1;
    for (auto it = hen_ended_tables.begin(); it != hen_ended_tables.end();) {
        if ((it->seen & bit) == 0 && it->table != current_page_table) {
            const auto found = jits.find(it->table);
            if (found == jits.end()) {
                it->seen |= bit;
            } else if (found->second.get() != in_use) {
                std::erase_if(hen_taken_over, [&](const HenTakenOver& t) {
                    return t.jit == found->second.get();
                });
                HenBlockList::Detach(found->second.get());
                // (handed over or not, the entry goes: an entry handed over is empty)
                HenTeardownHandOver(found->second, found->first);
                jits.erase(found);
                hen_jits_dropped++;
                it->seen |= bit;
            }
        }
        it = (it->seen & all) == all ? hen_ended_tables.erase(it) : it + 1;
    }
}

ARM_Dynarmic::~ARM_Dynarmic() {
    // (first: a JIT still being taken down may have a worker reading through this core's
    // callbacks, and no thread of ours may outlive the title)
    HenTeardownFinish();
    if (hen_interp) {
        HenInterpForget(hen_interp.get());
    }
    for (const auto& j : jits) {
        HenBlockList::Detach(j.second.get());
    }
    std::erase(hen_cores, this);
    hen_taken_over.clear();
    if (--hen_cores_alive == 0) {
        if (hen_jits_dropped != 0) {
            std::printf("hen: jit: %u JIT(s) of ended processes were dropped while the title ran\n",
                        hen_jits_dropped);
        }
        hen_jits_dropped = 0;
        hen_ended_tables.clear();
        // (parked JITs are in the cores' maps like every other JIT and are destroyed with
        // them, here on this thread; only the list of their page tables is ours)
        if (hen_park_stats.parked != 0 || hen_park_stats.too_many != 0) {
            std::size_t held = 0;
            for (const HenParkedTable& parked : hen_parked) {
                held += parked.jits;
            }
            std::printf("hen: jit: ended processes kept for their next start: %u; taken over by "
                        "a new process: %u (%llu block(s) kept, %llu not, %.1f ms checking); %u "
                        "given up to make room, %u not kept (more than %zu JITs); %zu JIT(s) "
                        "parked at the end, destroyed now with the cores\n",
                        hen_park_stats.parked, hen_park_stats.taken, hen_park_stats.kept,
                        hen_park_stats.dropped, hen_park_stats.check_ns / 1e6,
                        hen_park_stats.made_room, hen_park_stats.too_many, HEN_PARK_MAX_JITS,
                        held);
        }
        hen_park_stats = {};
        hen_parked.clear();
        hen_park_keys.clear();
    }
}
#else
ARM_Dynarmic::~ARM_Dynarmic() = default;
#endif

#if defined(__SWITCH__) && HEN_DEV
extern "C" void hen_core_sample_emu(int at_vblank); // hen/platform/hen_thread.c
#endif
MICROPROFILE_DEFINE(ARM_Jit, "ARM JIT", "ARM JIT", MP_RGB(255, 64, 64));

void ARM_Dynarmic::Run() {
    ASSERT(memory.GetCurrentPageTable() == current_page_table);
    MICROPROFILE_SCOPE(ARM_Jit);
    if (break_flag) [[unlikely]] {
        return;
    }

#ifdef __SWITCH__
    if (!hen_taken_over.empty()) [[unlikely]] {
        HenCheckTakenOver(); // (a parked JIT's blocks, against the new process's code)
    }
#if HEN_DEV
    const unsigned long long hen_start = hen_jit_now_ns();
    static unsigned long long hen_last_end;
    if (hen_last_end != 0 && hen_start - hen_last_end > 100'000'000ull) {
        std::printf("hen: slow: %.0f ms between two runs of the emulated CPU (next pc %08x)\n",
                    (hen_start - hen_last_end) / 1e6, jit->Regs()[15]);
        hen_blackbox_dump(static_cast<unsigned>((hen_start - hen_last_end) / 1'000'000)); // (where the thread was meanwhile)
    }
#endif
    // Interpreter first (dynarmic's AddressSpace::HenGetOrDefer): the generated code comes back
    // with UserDefined2 at a block that isn't compiled yet. The interpreter runs that one block
    // and the generated code goes on from where it ends.
    for (;;) {
        if (hen_jit_trace) {
            std::printf("J  %08x\n", jit->Regs()[15]);
        }
#if HEN_DEV
        const u32 hen_pc_before = jit->Regs()[15];
        const unsigned long long hen_seg = hen_jit_now_ns();
        const Dynarmic::HaltReason reason = jit->Run();
        if (const unsigned long long took = hen_jit_now_ns() - hen_seg; took > 100'000'000ull) {
            // (a time slice is a few milliseconds of emulated time at most)
            std::printf("hen: slow: %.0f ms in one entry of generated code, from %08x to %08x (reason %x)\n",
                        took / 1e6, hen_pc_before, jit->Regs()[15], static_cast<unsigned>(reason));
        }
#else
        const Dynarmic::HaltReason reason = jit->Run();
#endif
        if (hen_jit_trace) {
            std::printf("j  %08x %x\n", jit->Regs()[15], static_cast<unsigned>(reason));
        }
        if (reason != Dynarmic::HaltReason::UserDefined2) {
            break; // (anything else as well: that first, the block is still not compiled next time)
        }
#if HEN_DEV
        const unsigned long long hen_interp_start = hen_jit_now_ns();
        HenInterpretBlock();
        if (const unsigned long long took = hen_jit_now_ns() - hen_interp_start; took > 50'000'000ull) {
            std::printf("hen: slow: %.0f ms interpreting one block, now at %08x\n", took / 1e6, jit->Regs()[15]);
        }
#else
        HenInterpretBlock();
#endif
        if (hen_reschedule || break_flag || timer->GetDowncount() <= 0) {
            break;
        }
    }
    hen_reschedule = false;
#if HEN_DEV
    hen_last_end = hen_jit_now_ns();
    hen_jit_note_run(hen_last_end - hen_start);
    hen_core_sample_emu(0); // (which host core this thread is on; outside the time counted above)
#endif
#else
    jit->Run();
#endif
}

#ifdef __SWITCH__
extern "C" int hen_read_table; // hen/jit/hen_jit.c; core/memory.h, PageTable::Pointers
extern "C" int hen_tier_force;
extern "C" void hen_jit_note_interpreted(unsigned instrs, unsigned long long ns);


void ARM_Dynarmic::HenClearInterpreter() {
    for (ARMul_State* const state : hen_interp_states) {
        state->instruction_cache.clear();
    }
    trans_cache_buf_top = 0;
}

// One block in the interpreter.
void ARM_Dynarmic::HenInterpretBlock() {
    unsigned long long start = hen_jit_now_ns();
    if (!hen_interp) {
        hen_interp = std::make_unique<ARMul_State>(system, memory, USER32MODE);
        hen_interp->hen_tier = true;
        hen_interp_states.push_back(hen_interp.get());
    }
    ARMul_State& state = *hen_interp;
    state.Reg = jit->Regs();
    state.Cpsr = jit->Cpsr();
    state.ExtReg = jit->ExtRegs();
    state.VFP[VFP_FPSCR] = jit->Fpscr();
    state.VFP[VFP_FPEXC] = fpexc;
    state.CP15[CP15_THREAD_UPRW] = cp15_state.cp15_thread_uprw;
    if (trans_cache_buf_top > TRANS_CACHE_SIZE - 8 * 1024 * 1024) {
        HenClearInterpreter();
    }
    state.CP15[CP15_THREAD_URO] = cp15_state.cp15_thread_uro;
    state.hen_bail = false;
    state.NumInstrsToExecute = 1u << 20;
    if (hen_jit_trace) {
        std::printf("I  %08x %c", state.Reg[15], (state.Cpsr & 0x20) ? 'T' : 'A');
        std::fflush(stdout);
    }
    hen_interpreting = true;
    const u32 executed = InterpreterMainLoop(&state);
    hen_interpreting = false;
    if (hen_jit_trace) {
        std::printf(" -> %08x %u%s\n", state.Reg[15], executed, state.hen_bail ? " bail" : "");
        std::fflush(stdout);
    }
    // What makes the next block's location, to the JIT's state after every block.
    jit->Regs()[15] = state.Reg[15];
    jit->SetCpsr(state.Cpsr);
    jit->SetFpscr(state.VFP[VFP_FPSCR]);
    const bool refused = executed == 0 || state.hen_bail;
    if (refused) {
        hen_tier_force = 1; // not for the interpreter: the next lookup compiles
    }
    // (the JIT counts what Core::TicksForInstruction says, which averages about two a instruction)
    timer->AddTicks(static_cast<u64>(executed) * 2);
    hen_jit_note_interpreted(executed, hen_jit_now_ns() - start);
    jit->Regs() = state.Reg;
    jit->ExtRegs() = state.ExtReg;
    fpexc = state.VFP[VFP_FPEXC];
    cp15_state.cp15_thread_uprw = state.CP15[CP15_THREAD_UPRW];
}
#endif

void ARM_Dynarmic::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }
#ifdef __SWITCH__
    if (!hen_taken_over.empty()) [[unlikely]] {
        HenCheckTakenOver();
    }
#endif

    jit->Step();
}

void ARM_Dynarmic::SetPC(u32 pc) {
    jit->Regs()[15] = pc;
}

u32 ARM_Dynarmic::GetPC() const {
    return jit->Regs()[15];
}

u32 ARM_Dynarmic::GetReg(int index) const {
    return jit->Regs()[index];
}

void ARM_Dynarmic::SetReg(int index, u32 value) {
    jit->Regs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPReg(int index) const {
    return jit->ExtRegs()[index];
}

void ARM_Dynarmic::SetVFPReg(int index, u32 value) {
    jit->ExtRegs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return jit->Fpscr();
    case VFP_FPEXC:
        return fpexc;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }

    return UINT_MAX;
}

void ARM_Dynarmic::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        jit->SetFpscr(value);
        return;
    case VFP_FPEXC:
        fpexc = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

u32 ARM_Dynarmic::GetCPSR() const {
    return jit->Cpsr();
}

void ARM_Dynarmic::SetCPSR(u32 cpsr) {
    jit->SetCpsr(cpsr);
}

u32 ARM_Dynarmic::GetCP15Register(CP15Register reg) const {
    switch (reg) {
    case CP15_THREAD_UPRW:
        return cp15_state.cp15_thread_uprw;
    case CP15_THREAD_URO:
        return cp15_state.cp15_thread_uro;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }

    return 0;
}

void ARM_Dynarmic::SetCP15Register(CP15Register reg, u32 value) {
    switch (reg) {
    case CP15_THREAD_UPRW:
        cp15_state.cp15_thread_uprw = value;
        return;
    case CP15_THREAD_URO:
        cp15_state.cp15_thread_uro = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }
}

void ARM_Dynarmic::SaveContext(ThreadContext& ctx) {
    ctx.cpu_registers = jit->Regs();
    ctx.cpsr = jit->Cpsr();
    ctx.fpu_registers = jit->ExtRegs();
    ctx.fpscr = jit->Fpscr();
    ctx.fpexc = fpexc;
}

void ARM_Dynarmic::LoadContext(const ThreadContext& ctx) {
    jit->Regs() = ctx.cpu_registers;
    jit->SetCpsr(ctx.cpsr);
    jit->ExtRegs() = ctx.fpu_registers;
    jit->SetFpscr(ctx.fpscr);
    fpexc = ctx.fpexc;
}

void ARM_Dynarmic::PrepareReschedule() {
#ifdef __SWITCH__
    if (hen_interpreting) {
        hen_reschedule = true;
    }
#endif
    if (jit->IsExecuting()) {
        jit->HaltExecution();
    }
}

void ARM_Dynarmic::ClearInstructionCache() {
#ifdef __SWITCH__
    HenClearInterpreter();
#endif
    for (const auto& j : jits) {
        j.second->ClearCache();
    }
}

void ARM_Dynarmic::InvalidateCacheRange(u32 start_address, std::size_t length) {
#ifdef __SWITCH__
    if (hen_interp) {
        HenClearInterpreter();
    }
    // (Kaizhar) The range is the running process's. This core may be set to another
    // process just now (a system module), and its JIT for the running one would keep the old
    // blocks: that one is invalidated too. (The JIT in use is still done as upstream: at worst
    // blocks of another process at the same addresses are compiled again.)
    if (const auto process = system.Kernel().GetCurrentProcess()) {
        const auto found = jits.find(process->vm_manager.page_table);
        if (found != jits.end() && found->second.get() != jit) {
            found->second->InvalidateCacheRange(start_address, length);
        }
    }
#endif
    jit->InvalidateCacheRange(start_address, length);
}

void ARM_Dynarmic::ClearExclusiveState() {
    jit->ClearExclusiveState();
}

std::shared_ptr<Memory::PageTable> ARM_Dynarmic::GetPageTable() const {
    return current_page_table;
}

void ARM_Dynarmic::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
#ifdef __SWITCH__
    if (hen_interp && page_table != current_page_table) {
        HenClearInterpreter(); // (its decoded instructions are by address only)
    }
#endif
    current_page_table = page_table;
    ThreadContext ctx{};
    if (jit) {
        SaveContext(ctx);
    }
#ifdef __SWITCH__
    HenPurgeEndedJits(jit);
#endif

    auto iter = jits.find(current_page_table);
    if (iter != jits.end()) {
        jit = iter->second.get();
        LoadContext(ctx);
        return;
    }

#if defined(__SWITCH__) && HEN_DEV
    const unsigned long long hen_make_start = hen_jit_now_ns();
#endif
    auto new_jit = MakeJit();
#if defined(__SWITCH__) && HEN_DEV
    // (the "JIT creation" part of a title launch: it is not inside the launching service call
    // but here, when a core first runs the new process; see "hen: launch:" in apt/ns.cpp)
    std::printf("hen: launch: JIT made for a process new to core %u in %.1f ms (number %u on "
                "this core)\n",
                static_cast<unsigned>(GetID()), (hen_jit_now_ns() - hen_make_start) / 1e6,
                static_cast<unsigned>(hen_jits_made));
#endif
    jit = new_jit.get();
    LoadContext(ctx);
#ifdef __SWITCH__
    // (Named by core and order of creation: the same from one session to the next.)
    // (a count, not the number of JITs alive: those of ended processes are dropped)
    HenBlockList::Attach(jit, static_cast<u16>(GetID() * 256 + hen_jits_made++));
#endif
    jits.emplace(current_page_table, std::move(new_jit));
}

void ARM_Dynarmic::ServeBreak([[maybe_unused]] int signal) {
#ifdef ENABLE_GDBSTUB
    GDBStub::Break(signal);
#endif
}

std::unique_ptr<Dynarmic::A32::Jit> ARM_Dynarmic::MakeJit() {
    Dynarmic::A32::UserConfig config;
    config.callbacks = cb.get();
    if (current_page_table) {
        config.page_table = &current_page_table->GetPointerArray();
#ifdef __SWITCH__
        if (hen_read_table) {
            config.read_page_table = &current_page_table->GetReadPointerArray();
        }
#endif
    }
    config.coprocessors[15] = std::make_shared<DynarmicCP15>(cp15_state);
    config.define_unpredictable_behaviour = true;
#ifdef __SWITCH__
    // The system maps at most 16 pieces of code at a time, shared by all code caches;
    // hen/jit/hen_jit.c cuts a cache into few enough pieces for any size. 96 MiB: Super Smash
    // Bros. filled 44 MiB within a minute and had to start over (twice the work).
#ifdef __SWITCH__
    // (Kaizhar) 124 MiB, not 128: the SDK's allocator gives a block over
    // 32 MiB whole 32 MiB units, and a block has a 64 KiB header. 128 MiB and its header took
    // five units, 160 MiB, for the write area, and five more for the block the code is mapped
    // from (128 + 2 MiB for the 2 MiB boundary): 320 MiB a JIT. 124 and 126 MiB take four each.
    config.code_cache_size = hen_code_cache_mib * 1024ull * 1024ull;
    {
        // (Kaizhar) The process this JIT is for and the size of its code: host code comes
        // to 7 to 11 times the whole segment in six games. With
        // --jit-buffer-sized the buffer is 12 times the segments + 8 MiB, in steps that fill
        // whole 32 MiB units of the allocator with their header and the 2 MiB for the boundary
        // (28, 60, 92, 124 MiB); a buffer that proves too small grows (--jit-buffer-grow).
        // Without the flag only the line is printed, so that one session shows what every
        // process would get.
        u64 segments = 0;
        u32 process_id = 0;
        for (const auto& process : system.Kernel().GetProcessList()) {
            if (process && process->vm_manager.page_table == current_page_table && process->codeset) {
                segments = static_cast<u64>(process->codeset->CodeSegment().size) +
                           process->codeset->RODataSegment().size + process->codeset->DataSegment().size;
                process_id = process->process_id;
                break;
            }
        }
        const u64 want_mib = segments * 12 / (1024 * 1024) + 8;
        // (no process: the JIT every core is given when it is made, before any program is
        // loaded. It has never compiled a block; the smallest size, and it grows like any other.)
        const int sized_mib = segments == 0 || want_mib <= 28 ? 28 : want_mib <= 60 ? 60 : want_mib <= 92 ? 92 : 124;
        if (hen_buffer_sized && sized_mib < hen_code_cache_mib) {
            config.code_cache_size = sized_mib * 1024ull * 1024ull;
        }
#if HEN_DEV
        std::printf("hen: jit size: core %u, process %u: code, read-only data and data %llu KiB; by size it would get a "
                    "%d MiB code buffer; it gets %llu MiB%s\n",
                    static_cast<unsigned>(GetID()), process_id, static_cast<unsigned long long>(segments >> 10), sized_mib,
                    static_cast<unsigned long long>(config.code_cache_size >> 20),
                    segments == 0 ? " (made before any process: no code to size by)" : "");
#endif
    }
#else
    config.code_cache_size = 128 * 1024 * 1024;
#endif
#endif

    // Multi-process state
    config.processor_id = GetID();
    config.global_monitor = &exclusive_monitor.monitor;

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

} // namespace Core
