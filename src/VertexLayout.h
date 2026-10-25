#pragma once
#include <cstdint>
#include <vector>

namespace RealRHI {
	enum class VertexFormat {
		Float,
		Float2,
		Float3,
		Float4,
		Int,
		Int2,
		Int3,
		Int4,
		UInt,
		UInt2,
		UInt3,
		UInt4,
		UNorm8x4,
		SNorm8x4,
	};
	enum class VertexInputRate {
		Vertex,
		Instance
	};
	struct VertexAttributeDesc {
		uint32_t location = 0;
		uint32_t binding = 0;
		VertexFormat format = VertexFormat::Float;
		uint32_t offset = 0;
	};
	struct VertexBindingDesc {
		uint32_t binding = 0;
		uint32_t stride = 0;
		VertexInputRate inputRate = VertexInputRate::Vertex;
	};
	// An optional override for the tightly packed layout inferred from shader inputs.
	struct VertexLayout {
		std::vector<VertexBindingDesc> bindings;
		std::vector<VertexAttributeDesc> attributes;
	};
}
