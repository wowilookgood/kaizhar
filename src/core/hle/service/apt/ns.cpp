// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/core.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/apt/ns.h"
#include "core/loader/loader.h"
#ifdef __SWITCH__
#include <algorithm>
#include <cstdio>
#include <iterator>
#include "common/file_util.h"
#include "common/hen_gap.h"
extern "C" int hen_n3ds_mode_keep; // hen/jit/hen_jit.c: --no-n3ds-mode-keep
#if HEN_DEV
extern "C" unsigned long long hen_jit_now_ns(void);
#endif
#else
static constexpr int hen_n3ds_mode_keep = 0; // (other platforms: as upstream)
#endif

namespace Service::NS {

#if defined(__SWITCH__) && HEN_DEV
// (development builds) Parts of AppLoader_NCCH::Load and LoadExec, added up there
// (core/loader/ncch.cpp): reading the code, patching it, Process::Run, the look-up of an update
// title, making the code set and the process, registering the title's own archive (RomFS, icon,
// logo, banner), the region look-up. For the "hen: launch:" line below.
unsigned long long hen_launch_ns[7];
#define HEN_LAUNCH_NOW() hen_jit_now_ns()
#else
#define HEN_LAUNCH_NOW() 0ull
#endif

std::shared_ptr<Kernel::Process> LaunchTitle(Core::System& system, FS::MediaType media_type,
                                             u64 title_id) {
    std::string path;
    // (development builds: where the time of a launch goes, one line at the end)
    [[maybe_unused]] const unsigned long long hen_t0 = HEN_LAUNCH_NOW();
#if defined(__SWITCH__) && HEN_DEV
    const unsigned long long hen_files0 = hen_gap_total(HEN_GAP_FILE_CALLS);
    unsigned long long hen_parts0[7];
    std::copy(std::begin(hen_launch_ns), std::end(hen_launch_ns), hen_parts0);
    unsigned hen_absent0[2];
    FileUtil::HenAbsentStats(&hen_absent0[0], &hen_absent0[1]);
    unsigned hen_cache0[2];
    unsigned long long hen_cache_bytes;
    Loader::HenLaunchCacheStats(&hen_cache0[0], &hen_cache0[1], &hen_cache_bytes);
#endif
#ifdef __SWITCH__
    // (hen_launch_cache; common/file_util.cpp, hen_absent) For the length of this call, on this
    // thread: an optional file looked for and not found is remembered for the run. Starting a
    // title looks for about forty of them.
    struct HenAbsentScope {
        HenAbsentScope() {
            FileUtil::HenAbsentScope(+1);
        }
        ~HenAbsentScope() {
            FileUtil::HenAbsentScope(-1);
        }
    } hen_absent_scope;
#endif

    if (media_type == FS::MediaType::GameCard) {
        path = system.GetCartridge();
    } else {
        path = AM::GetTitleContentPathForLaunch(media_type, title_id);
    }
    [[maybe_unused]] const unsigned long long hen_t1 = HEN_LAUNCH_NOW();

    auto loader = Loader::GetLoader(path);
    u64 program_id;
    if (loader) {
        // (the loader may use the per-run launch cache: core/loader/ncch.cpp)
        loader->hen_relaunchable = true;
    }

    if (!loader || loader->ReadProgramId(program_id) != Loader::ResultStatus::Success ||
        program_id != title_id) {
        LOG_WARNING(Service_NS, "Could not load title=0x{:016x} media_type={}", title_id,
                    static_cast<int>(media_type));
        return nullptr;
    }

    auto plg_ldr = Service::PLGLDR::GetService(system);
    if (plg_ldr) {
        const auto& plg_context = plg_ldr->GetPluginLoaderContext();
        if (plg_context.is_enabled && plg_context.use_user_load_parameters &&
            plg_context.user_load_parameters.low_title_Id == static_cast<u32>(title_id) &&
            plg_context.user_load_parameters.plugin_memory_strategy ==
                PLGLDR::PLG_LDR::PluginMemoryStrategy::PLG_STRATEGY_MODE3) {
            loader->SetKernelMemoryModeOverride(Kernel::MemoryMode::Dev2);
        }
    }
    [[maybe_unused]] const unsigned long long hen_t2 = HEN_LAUNCH_NOW();

    {
        // This is normally done by PM, but we don't emulate it
        // so we do it here instead.
        auto mem_mode_res = loader->LoadKernelMemoryMode();
        Kernel::MemoryMode mem_mode{};
        auto n3ds_cap_res = loader->LoadNew3dsHwCapabilities();
        Kernel::New3dsHwCapabilities n3ds_hw_cap{};

        if (mem_mode_res.second == Loader::ResultStatus::Success && mem_mode_res.first) {
            mem_mode = mem_mode_res.first.value();
        }
        if (n3ds_cap_res.second == Loader::ResultStatus::Success && n3ds_cap_res.first) {
            n3ds_hw_cap = n3ds_cap_res.first.value();
        }
        // (Switch 2 port, hen_n3ds_mode_keep) The 804 MHz mode is one state for the whole
        // machine, and UpdateCPUAndMemoryState sets it from the header of whatever title is
        // started. For the memory half of the state that function already leaves applets and
        // system modules out ("PM only updates the reported memory for normal applications",
        // kernel.cpp), and APT reads the CPU half as the application's own
        // (AppletManager::GetApplicationRunningMode: New3ds or Old3ds by GetRunning804MHz).
        // The CPU half has no such test upstream: the console's keyboard, started as a title by
        // a New 3DS application, turns the mode off (its header does not ask for it) and
        // nothing turns it on again when the keyboard closes, so the application runs on at
        // 268 MHz and reports itself as running in the old mode. So a title of the applet or
        // system module kind started from here keeps the state the application left. The
        // title the emulator boots (core.cpp, System::Load) is not started from here and sets
        // the state as upstream, whatever its kind.
        constexpr u32 TID_HIGH_APPLET = 0x00040030;
        constexpr u32 TID_HIGH_SYSMODULE = 0x00040130;
        const u32 tid_high = static_cast<u32>(title_id >> 32);
        if (hen_n3ds_mode_keep && (tid_high == TID_HIGH_APPLET || tid_high == TID_HIGH_SYSMODULE)) {
#if defined(__SWITCH__) && HEN_DEV
            if (static_cast<bool>(n3ds_hw_cap.enable_804MHz_cpu) !=
                system.Kernel().GetRunning804MHz()) {
                std::printf("hen: clock: title %016llx is an applet or a system module: the 804 "
                            "MHz mode stays %s, as the application set it\n",
                            static_cast<unsigned long long>(title_id),
                            system.Kernel().GetRunning804MHz() ? "on" : "off");
            }
#endif
            n3ds_hw_cap.enable_804MHz_cpu = system.Kernel().GetRunning804MHz();
        }
        system.Kernel().UpdateCPUAndMemoryState(title_id, mem_mode, n3ds_hw_cap);
    }
    [[maybe_unused]] const unsigned long long hen_t3 = HEN_LAUNCH_NOW();

    std::shared_ptr<Kernel::Process> process;
    Loader::ResultStatus result = loader->Load(process);

    if (result != Loader::ResultStatus::Success) {
        LOG_WARNING(Service_NS, "Error loading .app for title 0x{:016x}", title_id);
        return nullptr;
    }

#if defined(__SWITCH__) && HEN_DEV
    {
        // "the rest of loading": opening the title's container, the look-up of an update
        // title on the SD card, making the code set and the process, its kernel capabilities,
        // registering it with the file services, the region look-up.
        const unsigned long long hen_t4 = hen_jit_now_ns();
        double part[7];
        for (int i = 0; i < 7; i++) {
            part[i] = (hen_launch_ns[i] - hen_parts0[i]) / 1e6;
        }
        const double load = (hen_t4 - hen_t3) / 1e6;
        unsigned absent[2], cache[2];
        unsigned long long cache_bytes;
        FileUtil::HenAbsentStats(&absent[0], &absent[1]);
        Loader::HenLaunchCacheStats(&cache[0], &cache[1], &cache_bytes);
        std::printf("hen: launch: title %016llx in %.1f ms: finding its file %.1f, opening it and "
                    "reading its ID %.1f, header fields and CPU/memory state %.1f, loading %.1f "
                    "(looking for an update title %.1f, reading the code %.1f, patching it "
                    "%.1f, making the code set and the process %.1f, starting the process %.1f, "
                    "its own archive with icon, logo and banner %.1f, the region look-up %.1f, "
                    "the rest of loading %.1f); of all that in file calls %.1f ms (fread, "
                    "fwrite, fflush and fclose on any thread; opening files and folder look-ups "
                    "are not in that figure); code image %s; optional files not found: %u "
                    "answered from memory, %u looked for on the card; the JIT is made after "
                    "this call, when a core first runs the process: see \"hen: launch: JIT "
                    "made\"\n",
                    static_cast<unsigned long long>(title_id), (hen_t4 - hen_t0) / 1e6,
                    (hen_t1 - hen_t0) / 1e6, (hen_t2 - hen_t1) / 1e6, (hen_t3 - hen_t2) / 1e6, load,
                    part[3], part[0], part[1], part[4], part[2], part[5], part[6],
                    load - part[0] - part[1] - part[2] - part[3] - part[4] - part[5] - part[6],
                    (hen_gap_total(HEN_GAP_FILE_CALLS) - hen_files0) / 1e6,
                    cache[0] != hen_cache0[0] ? "from memory" : "read from the card",
                    absent[0] - hen_absent0[0], absent[1] - hen_absent0[1]);
        (void)hen_cache_bytes;
        (void)cache_bytes;
    }
#endif
    return process;
}

void RebootToTitle(Core::System& system, FS::MediaType media_type, u64 title_id,
                   std::optional<Kernel::MemoryMode> mem_mode) {
    std::string new_path;
    if (media_type == FS::MediaType::GameCard) {
        new_path = system.GetCartridge();
    } else {
        new_path = AM::GetTitleContentPath(media_type, title_id);
    }

    if (new_path.empty() || !FileUtil::Exists(new_path)) {
        // TODO: This can happen if the requested title is not installed. Need a way to find
        // non-installed titles in the game list.
        LOG_CRITICAL(Service_APT,
                     "Failed to find title '{}' to jump to, resetting current title instead.",
                     new_path);
        new_path.clear();
    }

    std::optional<u8> mem_mode_u8;
    if (mem_mode) {
        mem_mode_u8 = static_cast<u8>(mem_mode.value());
    }
    system.RequestReset(new_path, mem_mode_u8);
}

} // namespace Service::NS
