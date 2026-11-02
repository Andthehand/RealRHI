#pragma once
#include "Window.h"

#include <cstdint>

namespace RealRHI {
	struct SwapchainDesc {
		Window* window;
		// Expose a storage descriptor on each backbuffer for compute writes in General layout.
		// Requires surface storage support; selects a storage-capable UNorm format instead of sRGB.
		bool enableStorage = false;
	};
}
