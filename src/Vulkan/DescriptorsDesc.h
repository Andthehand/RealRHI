#pragma once
#include <string>
#include <vector>
#include <Vulkan/vulkan.h>

namespace RealRHI {
	// One reflected binding entry within a Vulkan descriptor set.
	struct DescriptorBinding {
		std::string name;
		uint32_t binding = 0;
		VkDescriptorType vkType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
		uint32_t descriptorCount = 1;
	};

	struct DescriptorSetLayoutDesc {
		uint32_t setIndex = 0;
		std::vector<DescriptorBinding> bindings;
	};

	struct DescriptorsDesc {
		std::vector<DescriptorSetLayoutDesc> sets;
	};
}
