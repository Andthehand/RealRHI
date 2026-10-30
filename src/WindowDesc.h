#pragma once
#include <cstdint>

namespace RealRHI {
	struct WindowDesc {
		const char* title = "RealRHI Window";

		uint32_t width = 800;
		uint32_t height = 600;
	};
}
