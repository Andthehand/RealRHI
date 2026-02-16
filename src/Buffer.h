#pragma once

namespace RealRHI {
	class Buffer {
	public:
		virtual ~Buffer() = default;

		virtual void* Map() = 0;
		virtual void Unmap() = 0;
	};
}