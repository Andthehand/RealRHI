#pragma once
#include "PipelineDesc.h"

#include <limits>

namespace RealRHI {
	enum class Filter {
		Nearest,
		Linear,
	};

	enum class SamplerAddressMode {
		Repeat,
		MirroredRepeat,
		ClampToEdge,
		ClampToBorder,
	};

	enum class SamplerBorderColor {
		TransparentBlack,
		OpaqueBlack,
		OpaqueWhite,
	};

	struct SamplerDesc {
		Filter minFilter = Filter::Linear;
		Filter magFilter = Filter::Linear;
		Filter mipFilter = Filter::Linear;

		SamplerAddressMode addressU = SamplerAddressMode::Repeat;
		SamplerAddressMode addressV = SamplerAddressMode::Repeat;
		SamplerAddressMode addressW = SamplerAddressMode::Repeat;

		float mipLodBias = 0.0f;
		float minLod = 0.0f;
		float maxLod = std::numeric_limits<float>::max();

		bool compareEnable = false;
		CompareOp compareOp = CompareOp::Always;
		SamplerBorderColor borderColor = SamplerBorderColor::OpaqueBlack;
	};
}
