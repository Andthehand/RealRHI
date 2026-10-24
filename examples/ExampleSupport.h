#pragma once
#include "DeviceDesc.h"

#include <atomic>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace RealRHI::Examples {
	inline std::atomic<uint32_t> s_ErrorCount = 0;

	inline void CaptureDebugMessage(const DebugMessage& message) {
		if (message.severity == DebugSeverity::Error) {
			++s_ErrorCount;
		}
		DefaultDebugCallback(message);
	}

	inline bool ParseFrameLimit(int argc, char** argv, uint32_t& frameLimit) {
		frameLimit = 0;
		if (argc == 1) {
			return true;
		}
		if (argc == 3 && std::string_view(argv[1]) == "--frames") {
			const std::string_view value(argv[2]);
			const auto result = std::from_chars(value.data(), value.data() + value.size(), frameLimit);
			if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && frameLimit > 0) {
				return true;
			}
		}
		std::cerr << "Usage: " << argv[0] << " [--frames <positive count>]\n";
		return false;
	}
}
