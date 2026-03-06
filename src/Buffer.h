#pragma once
#include "RefCounted.h"

namespace RealRHI {
	class Buffer : public RefCounted {
	public:
		virtual ~Buffer() = default;
	};
}