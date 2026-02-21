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
        DebugSeverity severity;
        DebugMessageType type;
        const char* message;
    };

    using DebugCallback = void(*)(const DebugMessage& message);

    static void DefaultDebugCallback(const DebugMessage& message) {
        const char* severityStr = "";
        switch (message.severity) {
            case DebugSeverity::Info: severityStr = "Info"; break;
            case DebugSeverity::Warning: severityStr = "Warning"; break;
            case DebugSeverity::Error: severityStr = "Error"; break;
        }
        const char* typeStr = "";
        switch (message.type) {
            case DebugMessageType::General: typeStr = "General"; break;
            case DebugMessageType::Validation: typeStr = "Validation"; break;
			case DebugMessageType::Performance: typeStr = "Performance"; break;
                case DebugMessageType::ShaderCompilation: typeStr = "ShaderCompilation"; break;
        }
        std::cerr << "[" << severityStr << "][" << typeStr << "] " << message.message << std::endl;
	}

    struct DeviceDesc {
        const char* applicationName = "RealEngine";

		std::filesystem::path shaderDirectory = "assets/shaders";

        uint32_t maxFramesInFlight = 2;

		bool enableDebug = false;
		bool enableValidationLayers = false;
        DebugCallback debugCallback = DefaultDebugCallback;
    };
}