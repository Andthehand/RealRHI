#pragma once
#include "RefCounted.h"

#include "Result.h"

namespace RealRHI {
	class Buffer : public RefCounted {
	public:
		virtual ~Buffer() = default;

		virtual uint64_t GetSize() const = 0;
		virtual uint32_t GetDescriptorIndex() const = 0;

		virtual void WriteData(const void* data, uint64_t size, uint64_t offset = 0) = 0;
		virtual Result ReadData(void* data, uint64_t size, uint64_t offset = 0) const = 0;
	};
}