#pragma once
#include "Pipeline.h"
#include "PipelineDesc.h"

#include "VulkanDevice.h"

#include <Vulkan/vulkan.h>

namespace RealRHI {
	class VulkanPipeline : public Pipeline {
	public:
		VulkanPipeline(const VulkanDevice* device, const PipelineDesc& desc);
		~VulkanPipeline();

		//TODO: Remove
		VkPipeline GetPipeline() const { return m_Pipeline; }
	private:
		const VulkanDevice* m_Device;
		VkPipelineLayout m_PipelineLayout;
		VkPipeline m_Pipeline;
	};
}
