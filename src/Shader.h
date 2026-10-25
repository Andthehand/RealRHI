#pragma once
#include "RefCounted.h"
#include "VertexLayout.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RealRHI {
	enum class ShaderStage {
		Vertex,
		Fragment,
		Compute
	};
	enum class ShaderScalarType {
		Float16,
		Float32,
		Float64,
		Int32,
		UInt32
	};

	struct ShaderVertexInput {
		std::string name;
		uint32_t location = 0;
		ShaderScalarType scalarType = ShaderScalarType::Float32;
		uint32_t componentCount = 1;
	};
	struct ShaderEntryPoint {
		std::string name;
		ShaderStage stage = ShaderStage::Vertex;
		std::array<uint32_t, 3> computeGroupSize{};
	};
	// The current Vulkan contract permits one push block rooted at byte zero.
	struct ShaderPushData {
		uint32_t offset = 0;
		uint32_t size = 0;
	};
	struct ShaderReflection {
		std::vector<ShaderEntryPoint> entryPoints;
		std::vector<ShaderVertexInput> vertexInputs;
		VertexLayout vertexLayout;
		ShaderPushData pushData;
	};
	class Shader : public RefCounted {
	public:
		virtual ~Shader() = default;
		virtual const ShaderReflection& GetReflection() const = 0;
	};
}
