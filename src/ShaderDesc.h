#pragma once
#include "Shader.h"

#include <optional>

namespace RealRHI {
	struct ShaderEntryPointDesc {
		std::string name;
		// Optional assertion; the stage is always obtained from reflection.
		std::optional<ShaderStage> stage;
	};
	struct ShaderDesc {
		const char* moduleName = nullptr;
		// Empty discovers all entry points. Select names only when the module contains multiple programs.
		std::vector<ShaderEntryPointDesc> entryPoints;
		const char* debugName = nullptr;
	};
}
