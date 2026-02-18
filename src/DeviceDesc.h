#pragma once
#include <cstdint>
#include <filesystem>
#include <iostream>

namespace RealRHI {
    enum class DebugSeverity {
        Info,
        Warning,
        Error
    };

    enum class DebugMessageType {
        General,
        Validation,
        Performance,
		ShaderCompilation
    };

    struct DebugMessage {
        DebugSeverity Severity;
        DebugMessageType Type;
        const char* Message;
    };

    using DebugCallback = void(*)(const DebugMessage& message);

    static void DefaultDebugCallback(const DebugMessage& message) {
        const char* severityStr = "";
        switch (message.Severity) {
            case DebugSeverity::Info: severityStr = "Info"; break;
            case DebugSeverity::Warning: severityStr = "Warning"; break;
            case DebugSeverity::Error: severityStr = "Error"; break;
        }
        const char* typeStr = "";
        switch (message.Type) {
            case DebugMessageType::General: typeStr = "General"; break;
            case DebugMessageType::Validation: typeStr = "Validation"; break;
			case DebugMessageType::Performance: typeStr = "Performance"; break;
                case DebugMessageType::ShaderCompilation: typeStr = "ShaderCompilation"; break;
        }
        std::cerr << "[" << severityStr << "][" << typeStr << "] " << message.Message << std::endl;
	}

    struct DeviceDesc {
        const char* ApplicationName = "RealEngine";

		std::filesystem::path ShaderDirectory = "assets/shaders";

        uint32_t MaxFramesInFlight = 2;

		bool EnableDebug = false;
		bool EnableValidationLayers = false;
        DebugCallback DebugCallback = DefaultDebugCallback;
    };
}