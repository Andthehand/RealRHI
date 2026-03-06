#pragma once
#include "RefCounted.h"

namespace RealRHI {
	class Shader : public RefCounted {
	public:
		virtual ~Shader() = default;
	};
}