// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <memory>
#include <string>
#include <tuple>
#include <fmt/format.h>

#include "common/param_package.h"
#include "common/settings.h"
#include "common/vector_math.h"
#include "core/frontend/input.h"
#include "citra_switch/emu_window_switch.h"
#include "citra_switch/input_switch.h"

namespace SwitchInput {
namespace {

class Button final : public Input::ButtonDevice {
public:
    explicit Button(int bit_) : bit(bit_) {}
    bool GetStatus() const override {
        return (EmuWindow_Switch::Input().buttons >> bit) & 1;
    }

private:
    int bit;
};

class ButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    /// "button": a SW_BTN_* index
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<Button>(params.Get("button", 0));
    }
};

class Stick final : public Input::AnalogDevice {
public:
    explicit Stick(int index_) : index(index_ == 1 ? 1 : 0) {}
    std::tuple<float, float> GetStatus() const override {
        const auto input = EmuWindow_Switch::Input();
        return {input.sticks[index][0], input.sticks[index][1]};
    }

private:
    int index;
};

class StickFactory final : public Input::Factory<Input::AnalogDevice> {
public:
    /// "stick": 0 left, 1 right
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<Stick>(params.Get("stick", 0));
    }
};

class Motion final : public Input::MotionDevice {
public:
    std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() const override {
        const auto input = EmuWindow_Switch::Input();
        if (!input.motion) {
            return {Common::Vec3<float>{0.0f, -1.0f, 0.0f}, Common::Vec3<float>{}}; // lying flat
        }
        // Switch (right, away, out of the screen) to 3DS (left, out of the screen, away);
        // turns per second to degrees per second.
        return {Common::Vec3<float>{-input.accel[0], input.accel[2], input.accel[1]},
                Common::Vec3<float>{-input.gyro[0], input.gyro[2], input.gyro[1]} * 360.0f};
    }
};

class MotionFactory final : public Input::Factory<Input::MotionDevice> {
public:
    std::unique_ptr<Input::MotionDevice> Create(const Common::ParamPackage&) override {
        return std::make_unique<Motion>();
    }
};

std::string ButtonParam(int bit) {
    return fmt::format("engine:switch,button:{}", bit);
}

} // namespace

void Init() {
    Input::RegisterFactory<Input::ButtonDevice>("switch", std::make_shared<ButtonFactory>());
    Input::RegisterFactory<Input::AnalogDevice>("switch", std::make_shared<StickFactory>());
    Input::RegisterFactory<Input::MotionDevice>("switch", std::make_shared<MotionFactory>());

    // The 3DS's buttons on the same-labelled Switch buttons (A is A, on the right).
    using namespace Settings::NativeButton;
    auto& profile = Settings::values.current_input_profile;
    profile.buttons[A] = ButtonParam(SW_BTN_A);
    profile.buttons[B] = ButtonParam(SW_BTN_B);
    profile.buttons[X] = ButtonParam(SW_BTN_X);
    profile.buttons[Y] = ButtonParam(SW_BTN_Y);
    profile.buttons[Up] = ButtonParam(SW_BTN_UP);
    profile.buttons[Down] = ButtonParam(SW_BTN_DOWN);
    profile.buttons[Left] = ButtonParam(SW_BTN_LEFT);
    profile.buttons[Right] = ButtonParam(SW_BTN_RIGHT);
    profile.buttons[L] = ButtonParam(SW_BTN_L);
    profile.buttons[R] = ButtonParam(SW_BTN_R);
    profile.buttons[Start] = ButtonParam(SW_BTN_START);
    profile.buttons[Select] = ButtonParam(SW_BTN_SELECT);
    profile.buttons[ZL] = ButtonParam(SW_BTN_ZL);
    profile.buttons[ZR] = ButtonParam(SW_BTN_ZR);
    profile.analogs[Settings::NativeAnalog::CirclePad] = "engine:switch,stick:0";
    profile.analogs[Settings::NativeAnalog::CStick] = "engine:switch,stick:1";
    profile.touch_device = "engine:emu_window";
    profile.motion_device = "engine:switch";
}

void Shutdown() {
    Input::UnregisterFactory<Input::ButtonDevice>("switch");
    Input::UnregisterFactory<Input::AnalogDevice>("switch");
    Input::UnregisterFactory<Input::MotionDevice>("switch");
}

} // namespace SwitchInput
