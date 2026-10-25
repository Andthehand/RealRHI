#pragma once
#include "Shader.h"
#include "TextureFormat.h"
#include "VertexLayout.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace RealRHI {
	enum class CullMode {
		None,
		Front,
		Back,
	};

	enum class FillMode {
		Solid,
		Wireframe,
	};

	struct RasterState {
		CullMode cullMode = CullMode::Back;
		FillMode fillMode = FillMode::Solid;
		bool frontCounterClockwise = false;
	};

	enum class CompareOp {
		Never,
		Less,
		Equal,
		LessEqual,
		Greater,
		NotEqual,
		GreaterEqual,
		Always,
	};

	struct DepthState {
		bool depthTestEnable = true;
		bool depthWriteEnable = true;
		CompareOp compareOp = CompareOp::Less;
	};

	struct BlendState {
		bool enable = false;
	};

	struct RenderTargetFormats {
		std::vector<TextureFormat> colorFormats;
		TextureFormat depthFormat = TextureFormat::Unknown;
	};

	struct ComputePipelineDesc {
		Shader* shader = nullptr;
		const char* debugName = nullptr;
	};

	struct PipelineDesc {
		Shader* shader = nullptr;
		// Empty uses the shader-reflected, tightly packed layout on binding zero.
		VertexLayout vertexLayout;

		RasterState rasterState;
		DepthState depthState;
		BlendState blendState;

		RenderTargetFormats renderTargetFormats;
		const char* debugName = nullptr;
	};
}
