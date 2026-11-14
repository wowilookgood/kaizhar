// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <utility>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/hle/applets/erreula.h"
#include "core/hle/service/apt/apt.h"

namespace HLE::Applets {

std::string ErrEula::last_message;

std::string ErrEula::TakeLastMessage() {
    return std::exchange(last_message, {});
}

Result ErrEula::ReceiveParameterImpl(const Service::APT::MessageParameter& parameter) {
    if (parameter.signal != Service::APT::SignalType::Request) {
        LOG_ERROR(Service_APT, "unsupported signal {}", parameter.signal);
        UNIMPLEMENTED();
        // TODO(Subv): Find the right error code
        return ResultUnknown;
    }

    // The LibAppJustStarted message contains a buffer with the size of the framebuffer shared
    // memory.
    // Create the SharedMemory that will hold the framebuffer data
    Service::APT::CaptureBufferInfo capture_info;
    ASSERT(sizeof(capture_info) == parameter.buffer.size());

    std::memcpy(&capture_info, parameter.buffer.data(), sizeof(capture_info));

    // TODO: allocated memory never released
    using Kernel::MemoryPermission;
    // Create a SharedMemory that directly points to this heap block.
    framebuffer_memory = system.Kernel().CreateSharedMemoryForApplet(
        0, capture_info.size, MemoryPermission::ReadWrite, MemoryPermission::ReadWrite,
        "ErrEula Memory");

    // Send the response message with the newly created SharedMemory
    SendParameter({
        .sender_id = id,
        .destination_id = parent,
        .signal = Service::APT::SignalType::Response,
        .object = framebuffer_memory,
    });

    return ResultSuccess;
}

Result ErrEula::Start(const Service::APT::MessageParameter& parameter) {
    memcpy(&param, parameter.buffer.data(), std::min(parameter.buffer.size(), sizeof(param)));

    // Do something here, like showing error codes, or prompting for EULA agreement.
    // (There is nowhere to show the message, and programs often end after it: at least keep
    // it for the log and for the frontend, which can show it once the program has ended.)
    if (param.type != DisplayType::Agree) {
        std::u16string text(param.message,
                            std::find(param.message, param.message + ErrEulaParam::MESSAGE_SIZE,
                                      u'\0'));
        last_message = Common::UTF16ToUTF8(text);
        if (last_message.empty()) {
            last_message = fmt::format("Error code {:#010X}", static_cast<u32>(param.error_code));
        }
        LOG_ERROR(Service_APT, "The program shows an error (type {}, code {:#010X}): {}",
                  static_cast<u32>(param.type), static_cast<u32>(param.error_code), last_message);
    }
    if (param.type == DisplayType::Agree) {
        param.result = 1;
    }

    // Let the application know that we're closing.
    Finalize();
    return ResultSuccess;
}

Result ErrEula::Finalize() {
    std::vector<u8> buffer(sizeof(param));
    memcpy(buffer.data(), &param, buffer.size());
    CloseApplet(nullptr, buffer);
    return ResultSuccess;
}

void ErrEula::Update() {}

} // namespace HLE::Applets
