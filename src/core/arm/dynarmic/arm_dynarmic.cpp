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
extern "C" void hen_jit_note_run(unsigned long long ns);
#endif

#if HEN_DEV
extern "C" void hen_blackbox_dump(unsigned ms); // hen/jit/hen_jit.c
#endif

namespace Core {

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

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit DynarmicUserCallbacks(ARM_Dynarmic& parent)
        : parent(parent), svc_context(parent.system), memory(parent.memory) {}
    ~DynarmicUserCallbacks() = default;

    std::optional<std::uint32_t> MemoryReadCode(VAddr vaddr) override {
        return memory.Read32OrNullopt(vaddr);
    }

    std::uint8_t MemoryRead8(VAddr vaddr) override {
        hen_slow_read_megabytes[vaddr >> 20]++; // (plain counters: this is every LDREX too)
        hen_slow_read_last[vaddr >> 20] = vaddr;
        hen_mem_calls[0]++;
        return memory.Read8(vaddr);
    }
    std::uint16_t MemoryRead16(VAddr vaddr) override {
        hen_slow_read_megabytes[vaddr >> 20]++; // (plain counters: this is every LDREX too)
        hen_slow_read_last[vaddr >> 20] = vaddr;
        hen_mem_calls[0]++;
        return memory.Read16(vaddr);
    }
    std::uint32_t MemoryRead32(VAddr vaddr) override {
        hen_slow_read_megabytes[vaddr >> 20]++; // (plain counters: this is every LDREX too)
        hen_slow_read_last[vaddr >> 20] = vaddr;
        hen_mem_calls[0]++;
        return memory.Read32(vaddr);
    }
    std::uint64_t MemoryRead64(VAddr vaddr) override {
        hen_slow_read_megabytes[vaddr >> 20]++; // (plain counters: this is every LDREX too)
        hen_slow_read_last[vaddr >> 20] = vaddr;
        hen_mem_calls[0]++;
        return memory.Read64(vaddr);
    }

    void MemoryWrite8(VAddr vaddr, std::uint8_t value) override {
        hen_mem_calls[1]++;
        memory.Write8(vaddr, value);
    }
    void MemoryWrite16(VAddr vaddr, std::uint16_t value) override {
        hen_mem_calls[1]++;
        memory.Write16(vaddr, value);
    }
    void MemoryWrite32(VAddr vaddr, std::uint32_t value) override {
        hen_mem_calls[1]++;
        memory.Write32(vaddr, value);
    }
    void MemoryWrite64(VAddr vaddr, std::uint64_t value) override {
        hen_mem_calls[1]++;
        memory.Write64(vaddr, value);
    }

    bool MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) override {
        const bool stored = memory.WriteExclusive8(vaddr, value, expected);
        hen_exclusive_writes[stored ? 0 : 1]++;
        return stored;
    }
    bool MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) override {
        const bool stored = memory.WriteExclusive16(vaddr, value, expected);
        hen_exclusive_writes[stored ? 0 : 1]++;
        return stored;
    }
    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) override {
        const bool stored = memory.WriteExclusive32(vaddr, value, expected);
        hen_exclusive_writes[stored ? 0 : 1]++;
        return stored;
    }
    bool MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) override {
        const bool stored = memory.WriteExclusive64(vaddr, value, expected);
        hen_exclusive_writes[stored ? 0 : 1]++;
        return stored;
    }

    void InterpreterFallback(VAddr pc, std::size_t num_instructions) override {
        // Should never happen.
        UNREACHABLE_MSG("InterpeterFallback reached with pc = 0x{:08x}, code = 0x{:08x}, num = {}",
                        pc, MemoryReadCode(pc).value(), num_instructions);
    }

    void CallSVC(std::uint32_t swi) override {
        hen_svc_counts[swi & 0xff]++;
        const unsigned long long hen_svc_start = hen_jit_now_ns();
        svc_context.CallSVC(swi);
        hen_svc_ns[swi & 0xff] += hen_jit_now_ns() - hen_svc_start;
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
        hen_code_ticks_total += ticks;
        hen_code_ticks += ticks; // (ticks of executed program code only: system calls charge theirs elsewhere)
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

ARM_Dynarmic::ARM_Dynarmic(Core::System& system_, Memory::MemorySystem& memory_, u32 core_id_,
                           std::shared_ptr<Core::Timing::Timer> timer_,
                           Core::ExclusiveMonitor& exclusive_monitor_)
    : ARM_Interface(core_id_, timer_), system(system_), memory(memory_),
      cb(std::make_unique<DynarmicUserCallbacks>(*this)),
      exclusive_monitor{dynamic_cast<Core::DynarmicExclusiveMonitor&>(exclusive_monitor_)} {
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

ARM_Dynarmic::~ARM_Dynarmic() {
    if (hen_interp) {
        HenInterpForget(hen_interp.get());
    }
    for (const auto& j : jits) {
        HenBlockList::Detach(j.second.get());
    }
}
#else
ARM_Dynarmic::~ARM_Dynarmic() = default;
#endif

MICROPROFILE_DEFINE(ARM_Jit, "ARM JIT", "ARM JIT", MP_RGB(255, 64, 64));

void ARM_Dynarmic::Run() {
    ASSERT(memory.GetCurrentPageTable() == current_page_table);
    MICROPROFILE_SCOPE(ARM_Jit);
    if (break_flag) [[unlikely]] {
        return;
    }

#ifdef __SWITCH__
    const unsigned long long hen_start = hen_jit_now_ns();
    static unsigned long long hen_last_end;
#if HEN_DEV
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
    hen_last_end = hen_jit_now_ns();
    hen_jit_note_run(hen_last_end - hen_start);
#else
    jit->Run();
#endif
}

#ifdef __SWITCH__
extern "C" int hen_read_table; // hen/jit/hen_jit.c; core/memory.h, PageTable::Pointers
extern "C" int hen_tier_force;
extern "C" void hen_jit_note_interpreted(unsigned instrs, unsigned long long ns);
extern "C" int hen_interp_chain; // --no-interp-chain
extern "C" void hen_jit_note_chained(unsigned blocks);


void ARM_Dynarmic::HenClearInterpreter() {
    for (ARMul_State* const state : hen_interp_states) {
        state->instruction_cache.clear();
    }
    trans_cache_buf_top = 0;
}

// One block in the interpreter, and with hen_interp_chain the blocks after it for as long as
// the next one would only come back here again (not compiled yet: Jit::HenPeekDefer, which
// counts and queues the visit as the lookup in Run() would have). Between two blocks of such a
// run the registers stay in the interpreter's state; everything Run()'s loop and the JIT's
// entry look at between two blocks is looked at here: ticks, reschedule, break, a halt or a
// cache invalidation asked for (in HenPeekDefer), a block the interpreter refused.
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
    // (no coarser for events than a time slice is: the tick count ends a run as it ends Run()'s
    // loop; the limit is for whatever else only Run()'s caller looks at)
    constexpr unsigned HEN_CHAIN_MAX = 256;
    unsigned chained = 0;
    for (;;) {
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
        // The first line is Run()'s test after a block; then the lookup Run() would go to.
        if (hen_reschedule || break_flag || timer->GetDowncount() <= 0 ||
            !hen_interp_chain || refused || chained + 1 >= HEN_CHAIN_MAX || !jit->HenPeekDefer()) {
            break;
        }
        chained++;
        // The two status registers as the JIT's state gives them back (it keeps fewer bits),
        // which is what the interpreter was handed for every block before.
        state.Cpsr = jit->Cpsr();
        state.VFP[VFP_FPSCR] = jit->Fpscr();
        start = hen_jit_now_ns(); // (the lookup isn't the interpreter's time)
    }
    jit->Regs() = state.Reg;
    jit->ExtRegs() = state.ExtReg;
    fpexc = state.VFP[VFP_FPEXC];
    cp15_state.cp15_thread_uprw = state.CP15[CP15_THREAD_UPRW];
    if (chained != 0) {
        hen_jit_note_chained(chained);
    }
}
#endif

void ARM_Dynarmic::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }

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

    auto iter = jits.find(current_page_table);
    if (iter != jits.end()) {
        jit = iter->second.get();
        LoadContext(ctx);
        return;
    }

    auto new_jit = MakeJit();
    jit = new_jit.get();
    LoadContext(ctx);
#ifdef __SWITCH__
    // (Named by core and order of creation: the same from one session to the next.)
    HenBlockList::Attach(jit, static_cast<u16>(GetID() * 256 + jits.size()));
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
    config.code_cache_size = 128 * 1024 * 1024;
#endif

    // Multi-process state
    config.processor_id = GetID();
    config.global_monitor = &exclusive_monitor.monitor;

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

} // namespace Core
