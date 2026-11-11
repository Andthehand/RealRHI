#pragma once
#include "TextureFormat.h"
#include "VertexLayout.h"

#include <array>
#include <cstdint>
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

		bool operator==(const DepthBiasState&) const = default;
	};

	struct RasterState {
		CullMode cullMode = CullMode::Back;
		FillMode fillMode = FillMode::Solid;
		bool frontCounterClockwise = false;
		DepthBiasState depthBias;

		bool operator==(const RasterState&) const = default;
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

		bool operator==(const DepthState&) const = default;
	};

	enum class StencilOp {
		Keep,
		Zero,
		Replace,
		IncrementClamp,
		DecrementClamp,
		Invert,
		IncrementWrap,
		DecrementWrap,
	};

	struct StencilFaceState {
		StencilOp failOp = StencilOp::Keep;
		StencilOp passOp = StencilOp::Keep;
		StencilOp depthFailOp = StencilOp::Keep;
		CompareOp compareOp = CompareOp::Always;
		uint32_t compareMask = UINT32_MAX;
		uint32_t writeMask = UINT32_MAX;
		uint32_t reference = 0;

		bool operator==(const StencilFaceState&) const = default;
	};

	struct StencilState {
		bool enable = false;
		StencilFaceState front;
		StencilFaceState back;

		bool operator==(const StencilState&) const = default;
	};

	struct MultisampleState {
		uint32_t sampleCount = 1;
		uint64_t sampleMask = UINT64_MAX;
		bool alphaToCoverageEnable = false;

		bool operator==(const MultisampleState&) const = default;
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

	// Fixed-function graphics state, independent of shaders, vertex layout, and attachment formats.
	struct GraphicsState {
		RasterState raster;
		DepthState depth;
		StencilState stencil;
		MultisampleState multisample;
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		bool primitiveRestartEnable = false;
		BlendState blend;
		// Empty repeats blend for every active color attachment.
		std::vector<BlendState> colorBlends;
		std::array<float, 4> blendConstants{};

		bool operator==(const GraphicsState&) const = default;
	};
}
