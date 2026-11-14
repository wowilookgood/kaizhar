// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

namespace SwitchInput {

/// Registers the "switch" input engine (buttons and sticks of the console's controller) and
/// points the current input profile at it.
void Init();
void Shutdown();

} // namespace SwitchInput
