#pragma once
#include "RefCounted.h"

#include <cstdint>

namespace RealRHI {
	class Sampler : public RefCounted {
	public:
		virtual ~Sampler() = default;

		virtual uint32_t GetDescriptorIndex() const = 0;
	};
}
