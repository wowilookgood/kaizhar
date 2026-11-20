// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/service/hid/hid_test_input.h"

#ifdef __SWITCH__

namespace Service::HID::TestInput {
void Apply(Core::System&, u32&) {}
void TraceDiscard(Core::System&) {}
} // namespace Service::HID::TestInput

#else

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/memory.h"
#include "network/network.h"
#include "network/room.h"
#include "network/room_member.h"
#include "network/verify_user.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace Service::HID::TestInput {

namespace {

struct State {
    bool looked = false;
    std::string dir;
    unsigned calls = 0;
    u32 held = 0;      ///< the buttons held now
    u64 held_until = 0; ///< emulated ms
    struct Pending {
        u32 buttons;
        u64 ms;
    };
    std::vector<Pending> queue; ///< presses still to come, in order, with a pause between
    u64 next_at = 0;
    unsigned done = 0; ///< commands carried out
    std::shared_ptr<std::vector<u8>> shot;
};
State state;

u32 ButtonBits(const std::string& names) {
    static const struct {
        const char* name;
        u32 bit;
    } table[] = {{"A", 1u << 0},     {"B", 1u << 1},     {"SELECT", 1u << 2}, {"START", 1u << 3},
                 {"RIGHT", 1u << 4}, {"LEFT", 1u << 5},  {"UP", 1u << 6},     {"DOWN", 1u << 7},
                 {"R", 1u << 8},     {"L", 1u << 9},     {"X", 1u << 10},     {"Y", 1u << 11}};
    u32 bits = 0;
    std::stringstream parts(names);
    std::string part;
    while (std::getline(parts, part, '+')) {
        for (const auto& entry : table) {
            if (part == entry.name) {
                bits |= entry.bit;
            }
        }
    }
    return bits;
}

void Command(Core::System& system, const std::string& line, u64 now_ms) {
    std::stringstream words(line);
    std::string what;
    words >> what;
    if (what == "press") {
        std::string names;
        u64 ms = 100;
        words >> names >> ms;
        state.queue.push_back({ButtonBits(names), ms});
    } else if (what == "shot") {
        std::string name;
        words >> name;
        constexpr u32 Width = 400, Height = 480;
        state.shot = std::make_shared<std::vector<u8>>(Width * Height * 4);
        const std::string path = state.dir + "/" + name + ".raw";
        auto buffer = state.shot;
        system.GPU().Renderer().RequestScreenshot(
            buffer->data(),
            [buffer, path](bool) {
                if (FILE* file = std::fopen((path + ".part").c_str(), "wb")) {
                    std::fprintf(file, "%u %u\n", Width, Height);
                    std::fwrite(buffer->data(), 1, buffer->size(), file);
                    std::fclose(file);
                    std::rename((path + ".part").c_str(), path.c_str());
                }
            },
            Layout::DefaultFrameLayout(Width, Height, false, false));
    } else if (what == "room") {
        std::string nick, address;
        u16 port = Network::DefaultRoomPort;
        words >> nick >> address >> port;
        if (const auto member = Network::GetRoomMember().lock()) {
            member->Join(nick, Service::CFG::GetConsoleIdHash(system), address.c_str(), port, 0,
                         Service::CFG::GetConsoleMacAddress(system), "");
        }
    } else if (what == "hostroom") {
        // (a room of this instance's own, for the two test instances to meet in)
        u16 port = Network::DefaultRoomPort;
        words >> port;
        if (const auto room = Network::GetRoom().lock()) {
            room->Create("test room", "", "", port, "", 8, "", "", 0,
                         std::make_unique<Network::VerifyUser::NullBackend>());
        }
    } else if (what == "leave") {
        if (const auto member = Network::GetRoomMember().lock(); member && member->IsConnected()) {
            member->Leave();
        }
    }
    state.done++;
    (void)now_ms;
}

} // namespace

void Apply(Core::System& system, u32& buttons) {
    if (!state.looked) {
        state.looked = true;
        if (const char* dir = std::getenv("KAIZHAR_TEST_DIR")) {
            state.dir = dir;
            LOG_INFO(Service_HID, "scripted test input from {}", state.dir);
        }
    }
    if (state.dir.empty()) {
        return;
    }
    const u64 now_ms = system.CoreTiming().GetTicks() / (BASE_CLOCK_RATE_ARM11 / 1000);
    // (the file is looked for at every sixteenth sample: about fifteen times a second)
    if (state.calls++ % 16 == 0) {
        const std::string path = state.dir + "/cmd";
        std::ifstream file(path);
        if (file) {
            std::vector<std::string> lines;
            for (std::string line; std::getline(file, line);) {
                if (!line.empty()) {
                    lines.push_back(line);
                }
            }
            file.close();
            std::remove(path.c_str());
            for (const std::string& line : lines) {
                Command(system, line, now_ms);
            }
        }
        if (state.calls % 256 == 1) {
            if (FILE* status = std::fopen((state.dir + "/status.part").c_str(), "w")) {
                const auto member = Network::GetRoomMember().lock();
                std::fprintf(status, "ms %llu\ndone %u\nqueued %zu\nroom %d\n",
                             static_cast<unsigned long long>(now_ms), state.done, state.queue.size(),
                             member ? static_cast<int>(member->GetState()) : -1);
                std::fclose(status);
                std::rename((state.dir + "/status.part").c_str(), (state.dir + "/status").c_str());
            }
        }
    }
    if (state.held != 0 && now_ms >= state.held_until) {
        state.held = 0;
        state.next_at = now_ms + 150; // (released for a moment before the next press)
    }
    if (state.held == 0 && !state.queue.empty() && now_ms >= state.next_at) {
        state.held = state.queue.front().buttons;
        state.held_until = now_ms + state.queue.front().ms;
        state.queue.erase(state.queue.begin());
    }
    buttons |= state.held;
}

void TraceDiscard(Core::System& system) {
    const char* dir = std::getenv("KAIZHAR_TEST_TRACE_CODE");
    if (dir == nullptr) {
        return;
    }
    // Tests on a computer: where the program is when it discards a host (registers, the
    // stack's top), and once, its code and data for reading that place.
    auto& core = system.GetRunningCore();
    std::string regs;
    for (int i = 0; i < 16; i++) {
        regs += fmt::format(" r{}={:08x}", i, core.GetReg(i));
    }
    const u32 sp = core.GetReg(13);
    std::string stack;
    for (u32 i = 0; i < 160; i++) {
        stack += fmt::format(" {:08x}", system.Memory().Read32(sp + 4 * i));
    }
    LOG_INFO(Service_DLP, "TRACE discard at:{}; stack:{}", regs, stack);
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        for (const auto& [base, size, name] : {std::tuple<u32, u32, const char*>{0x00100000, 0x00C00000, "code"}}) {
            std::vector<u8> bytes(size);
            system.Memory().ReadBlock(base, bytes.data(), size);
            if (FILE* file = std::fopen(fmt::format("{}/{}.bin", dir, name).c_str(), "wb")) {
                std::fwrite(bytes.data(), 1, bytes.size(), file);
                std::fclose(file);
            }
        }
    }
}

} // namespace Service::HID::TestInput

#endif
