#pragma once
#include "RefCounted.h"

namespace RealRHI {
	class Pipeline : public RefCounted {
	public:
		virtual ~Pipeline() = default;
	};
}