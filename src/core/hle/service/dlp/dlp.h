// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::DLP {

/// Initializes the DLP services.
void InstallInterfaces(Core::System& system);

/// Ends the services' threads (a hosting or a download in progress) and waits for them. They
/// look other services up while they run, so this comes first when the system shuts down.
void StopWorkers(Core::System& system);

} // namespace Service::DLP
