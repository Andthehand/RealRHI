#pragma once
#include <vector>

namespace RealRHI {
	enum class DescriptorType {
		UniformBuffer, // constantBuffer
		StorageBuffer, // RWStructuredBuffer
		SampledImage, // Sampler2D
	};

	struct DescriptorBinding {
		DescriptorType type;
		uint32_t count;
	};

	struct DescriptorSetLayoutBinding {
		std::vector<DescriptorBinding> bindings;
	};

	struct DescriptorsDesc {
		std::vector<DescriptorSetLayoutBinding> sets;
	};
}
