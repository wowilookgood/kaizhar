// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <mutex>
#include <utility>

#include "citra_switch/emu_window_switch.h"

namespace {
std::mutex input_mutex;
sw_input input_state{};

/// Where a point of the 3DS's bottom screen (x and y in [0, 1], as the player sees it) is in the
/// layout's bottom screen, in pixels from that rectangle's corner. In an upright layout the
/// picture's top is at the display's left: the inverse of what EmuWindow::TouchPressed does.
/// (A flipped layout is the same here: sw_flipped() turns the finished picture.)
std::pair<float, float> InBottomScreen(const Layout::FramebufferLayout& layout, float x, float y) {
    const float width = static_cast<float>(layout.bottom_screen.GetWidth());
    const float height = static_cast<float>(layout.bottom_screen.GetHeight());
    return layout.is_rotated ? std::pair{x * width, y * height}
                             : std::pair{y * width, (1.0f - x) * height};
}
} // namespace

EmuWindow_Switch::EmuWindow_Switch() {
    {
        // (Nothing held over from the title before: its last state had the menu buttons down.)
        std::scoped_lock lock{input_mutex};
        input_state = {};
    }
    sw_stick_pointer_reset();
    window_info.type = Frontend::WindowSystemType::Switch;
    window_info.render_surface = sw_window();
    unsigned width, height;
    sw_window_size(&width, &height);
    UpdateCurrentFramebufferLayout(width, height);
}

EmuWindow_Switch::~EmuWindow_Switch() = default;

void EmuWindow_Switch::Relayout() {
    unsigned width, height;
    sw_window_size(&width, &height);
    UpdateCurrentFramebufferLayout(width, height);
}

sw_input EmuWindow_Switch::Input() {
    // Read by the emulation thread while PollEvents writes.
    std::scoped_lock lock{input_mutex};
    return input_state;
}

void EmuWindow_Switch::PollEvents() {
    sw_input now;
    sw_input_update(&now);
    {
        std::scoped_lock lock{input_mutex};
        input_state = now;
    }
    // (The Joy-Con mouse, or the right stick as a pointer, is the stylus when the finger is not
    // on the panel.)
    if (!now.touching && now.pointer && now.pointer_down) {
        const auto& layout = GetFramebufferLayout();
        const auto [x, y] = InBottomScreen(layout, now.pointer_x, now.pointer_y);
        now.touching = true;
        now.touch_x = layout.bottom_screen.left + x;
        now.touch_y = layout.bottom_screen.top + y;
    }
    if (now.touching) {
        const auto x = static_cast<unsigned>(now.touch_x);
        const auto y = static_cast<unsigned>(now.touch_y);
        if (was_touching) {
            TouchMoved(x, y);
        } else {
            was_touching = TouchPressed(x, y);
        }
    } else if (was_touching) {
        TouchReleased();
        was_touching = false;
    }
}

Frontend::EmuWindow::CursorInfo EmuWindow_Switch::GetCursorInfo() const {
    const sw_input now = Input();
    if (!now.pointer) {
        return {};
    }
    const auto [x, y] = InBottomScreen(GetFramebufferLayout(), now.pointer_x, now.pointer_y);
    return {true, x, y};
}
