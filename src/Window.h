#pragma once
#include "RefCounted.h"

#include <cstdint>

namespace RealRHI {
	class Window : public RefCounted {
	public:
		virtual ~Window() = default;

		virtual uint32_t GetWidth() const = 0;
		virtual uint32_t GetHeight() const = 0;
	};
}
