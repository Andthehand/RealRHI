#pragma once
#include "Window.h"

#include <cstdint>

namespace RealRHI {
	struct SwapchainDesc {
		Window* window;
		// Expose a storage descriptor on each backbuffer for compute writes in General layout.
		// Requires surface storage support; selects a storage-capable UNorm format instead of sRGB.
		bool enableStorage = false;
		// Synchronize presentation to the display refresh using FIFO.
		// When disabled, prefer Immediate, then Mailbox, then FIFO if neither is supported.
		bool enableVSync = true;
	};
}
