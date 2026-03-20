#pragma once
#include "RefCounted.h"

#include "Result.h"

namespace RealRHI {
	class Buffer : public RefCounted {
	public:
		virtual ~Buffer() = default;

		virtual Result WriteData(const void* data, uint64_t size, uint64_t offset = 0) = 0;
	};
}