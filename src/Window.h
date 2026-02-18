#pragma once
#include <cstdint>

namespace RealRHI {
	class Window {
	public:
		virtual ~Window() = default;

		virtual uint32_t GetWidth() const = 0;
		virtual uint32_t GetHeight() const = 0;
	};
}