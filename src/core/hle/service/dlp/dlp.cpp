// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/core.h"
#include "core/hle/service/dlp/dlp.h"
#include "core/hle/service/dlp/dlp_clnt.h"
#include "core/hle/service/dlp/dlp_fkcl.h"
#include "core/hle/service/dlp/dlp_srvr.h"

namespace Service::DLP {

void InstallInterfaces(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    std::make_shared<DLP_CLNT>()->InstallAsService(service_manager);
    std::make_shared<DLP_FKCL>()->InstallAsService(service_manager);
    std::make_shared<DLP_SRVR>()->InstallAsService(service_manager);
}

void StopWorkers(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    if (auto server = service_manager.GetService<DLP_SRVR>("dlp:SRVR")) {
        server->StopWorker();
    }
    if (auto client = service_manager.GetService<DLP_CLNT>("dlp:CLNT")) {
        client->StopWorker();
    }
    if (auto client = service_manager.GetService<DLP_FKCL>("dlp:FKCL")) {
        client->StopWorker();
    }
}

} // namespace Service::DLP
