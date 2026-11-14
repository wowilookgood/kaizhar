// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <optional>
#include <string>

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

/// Puts the saved screen arrangement into Settings (for a title given on the command line).
/// layout: 0..3 from the list in title_menu.cpp, or -1 for the saved one.
void ApplySavedLayout(int layout = -1);

/// A message with "A: back to the titles", shown until A or B is pressed.
void ShowMessage(const std::string& heading, const std::string& line1, const std::string& line2);

} // namespace TitleMenu
