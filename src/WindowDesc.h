#pragma once
#include <cstdint>

namespace RealRHI {
	struct WindowDesc {
		const char* Title = "RealRHI Window";

		uint32_t Width = 800;
		uint32_t Height = 600;
	};
}
