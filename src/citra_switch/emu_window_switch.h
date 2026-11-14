// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "core/frontend/emu_window.h"
#include "citra_switch/switch_platform.h"

/// The console's one display (the default NWindow, 1920x1080, docked and handheld) as the
/// emulator's window. The Vulkan renderer creates its surface on it and presents by itself.
class EmuWindow_Switch final : public Frontend::EmuWindow {
public:
    EmuWindow_Switch();
    ~EmuWindow_Switch() override;

    /// Reads the controller and touch screen; called once per frame by the renderer.
    void PollEvents() override;

    /// The input state PollEvents last read (for the input devices in input_switch.cpp).
    static sw_input Input();

    /// The Joy-Con mouse's crosshair on the bottom screen.
    CursorInfo GetCursorInfo() const override;

private:
    bool was_touching = false;
};
