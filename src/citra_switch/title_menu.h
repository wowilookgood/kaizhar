// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

/// The title picker shown when the app is opened without a program to run: the files in
/// /switch/Kaizhar/roms, 3DS homebrew and the titles installed on the emulated
/// SD card (a .cia in roms is offered for installing), and the choice of screen arrangement.
namespace TitleMenu {

struct Choice {
    std::string name;
    std::string path;
};

/// Shows the picker until a title is chosen (the screen arrangement is then in Settings).
/// Nothing: leave the app.
std::optional<Choice> Run();

/// (development builds, --sequence) The title list picks these by itself, one per visit, instead
/// of waiting for a person: each text is looked for in the entries' paths and names (any case).
/// When the texts are used up the list leaves the app, with the saved arrangement put back.
void SetScript(std::vector<std::string> matches);

/// Puts the saved screen arrangement into Settings (for a title given on the command line).
/// layout: 0..3 from the list in title_menu.cpp, or -1 for the saved one.
void ApplySavedLayout(int layout = -1);

/// What a short click of the left stick does while a title runs (the Settings page's row).
enum class StickAction { NextArrangement, SwapScreens, Mute, Nothing };

/// For the title about to start: hands the Settings page's pointer rows to the platform layer
/// and says what the left stick's click does.
StickAction ApplyInputSettings();

/// While a title runs: the next screen arrangement of the list, saved and put into Settings as
/// X in the title list does (the caller lays the window out again). Returns its name.
const char* NextArrangement();

/// A message with "A: back to the titles", shown until A or B is pressed.
void ShowMessage(const std::string& heading, const std::string& line1, const std::string& line2);

/// A page of text, wrapped, shown until A or B is pressed.
void ShowPage(const std::string& heading, const std::string& text);

/// Shows one line while work runs (the menu's display is taken for it and given back).
void Busy(const std::string& heading, const std::string& line, const std::function<void()>& work);

/// The online system modules are held on by a launch option: the Online play screen says so
/// and leaves its switch alone.
void SetOnlineForced(bool forced);

} // namespace TitleMenu
