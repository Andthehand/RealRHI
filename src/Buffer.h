#pragma once
#include "RefCounted.h"
#include "Result.h"

namespace RealRHI {
	class Buffer : public RefCounted {
	public:
		virtual ~Buffer() = default;

		// Native allocation size in bytes, including alignment.
		virtual uint64_t GetAllocationSize() const = 0;

		virtual uint64_t GetSize() const = 0;
		virtual uint32_t GetDescriptorIndex() const = 0;

		// Mapped-memory operations validate the range and report copy/flush failures.
		virtual Result WriteData(const void* data, uint64_t size, uint64_t offset = 0) = 0;
		virtual Result ReadData(void* data, uint64_t size, uint64_t offset = 0) const = 0;
	};
}
