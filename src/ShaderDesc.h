#pragma once
#include "Shader.h"

#include <optional>

namespace RealRHI {
	struct ShaderPreprocessorDefinition {
		std::string name;
		std::string value = "1";
	};
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
		// Object-like macros scoped to this shader, including its imports and includes.
		// Names must be unique identifiers. An omitted value expands to 1; "" is an empty replacement.
		std::vector<ShaderPreprocessorDefinition> preprocessorDefinitions;
	};
}
