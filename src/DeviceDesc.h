#pragma once
#include <cstdint>

namespace RealRHI {
    enum class DebugSeverity {
        Info,
        Warning,
        Error
    };

    enum class DebugMessageType {
        General,
        Validation,
        Performance
    };

    struct DebugMessage {
        DebugSeverity Severity;
        DebugMessageType Type;
        const char* Message;
    };

    using DebugCallback = void(*)(const DebugMessage& message);

    struct DeviceDesc {
        const char* ApplicationName = "RealEngine";

        uint32_t MaxFramesInFlight = 2;

		bool EnableDebug = false;
        DebugCallback DebugCallback = nullptr;
    };
}