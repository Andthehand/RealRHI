#pragma once
#include "RefCounted.h"

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

#include <string>

namespace RealRHI {
	struct EntryPoint {
		std::string entryPointName;
		SlangStage stage;
	};

	class Shader : public RefCounted {
	public:
		virtual ~Shader() = default;
	};
}