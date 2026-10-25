#pragma once
#include "Shader.h"
#include "TextureFormat.h"
#include "VertexLayout.h"

#include <array>
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

	enum class PrimitiveTopology {
		PointList,
		LineList,
		LineStrip,
		TriangleList,
		TriangleStrip,
		TriangleFan,
	};

	struct DepthBiasState {
		bool enable = false;
		float constantFactor = 0.f;
		float clamp = 0.f;
		float slopeFactor = 0.f;
	};

	struct RasterState {
		CullMode cullMode = CullMode::Back;
		FillMode fillMode = FillMode::Solid;
		bool frontCounterClockwise = false;
		DepthBiasState depthBias;
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

	enum class BlendFactor {
		Zero,
		One,
		SourceColor,
		OneMinusSourceColor,
		DestinationColor,
		OneMinusDestinationColor,
		SourceAlpha,
		OneMinusSourceAlpha,
		DestinationAlpha,
		OneMinusDestinationAlpha,
		ConstantColor,
		OneMinusConstantColor,
		ConstantAlpha,
		OneMinusConstantAlpha,
		SourceAlphaSaturate,
	};

	enum class BlendOperation {
		Add,
		Subtract,
		ReverseSubtract,
		Min,
		Max,
	};

	enum class ColorWriteMask : uint32_t {
		None = 0,
		Red = 1 << 0,
		Green = 1 << 1,
		Blue = 1 << 2,
		Alpha = 1 << 3,
		All = Red | Green | Blue | Alpha,
	};

	constexpr ColorWriteMask operator|(ColorWriteMask left, ColorWriteMask right) {
		return static_cast<ColorWriteMask>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
	}

	struct BlendState {
		bool enable = false;
		BlendFactor sourceColorFactor = BlendFactor::SourceAlpha;
		BlendFactor destinationColorFactor = BlendFactor::OneMinusSourceAlpha;
		BlendOperation colorOperation = BlendOperation::Add;
		BlendFactor sourceAlphaFactor = BlendFactor::One;
		BlendFactor destinationAlphaFactor = BlendFactor::Zero;
		BlendOperation alphaOperation = BlendOperation::Add;
		ColorWriteMask writeMask = ColorWriteMask::All;

		bool operator==(const BlendState&) const = default;
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
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		// Restart uses the index type's maximum value and is supported for strips and fans.
		bool primitiveRestartEnable = false;

		BlendState blendState;
		// Empty repeats blendState for every color target. Otherwise provide one state per target.
		std::vector<BlendState> colorBlendStates;
		std::array<float, 4> blendConstants{};

		RenderTargetFormats renderTargetFormats;
		const char* debugName = nullptr;
	};
}
