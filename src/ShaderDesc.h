#pragma once
#include <vector>

namespace RealRHI {
	enum class ShaderStage {
		Vertex,
		Fragment,
		Compute,
	};

	struct EntryPoint {
		const char* entryPoint;
		ShaderStage stage;
	};

	struct ShaderDesc {
		const char* moduleName;
		std::vector<EntryPoint> entryPoints;
	};
}
