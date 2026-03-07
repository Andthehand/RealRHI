#pragma once
#include "Pipeline.h"
#include "PipelineDesc.h"

#include "VulkanDevice.h"
#include "Result.h"

#include <Vulkan/vulkan.h>

namespace RealRHI {
	class VulkanPipeline : public Pipeline {
	public:
		static Result Create(const VulkanDevice* device, const PipelineDesc& desc, Ref<Pipeline>& outPipeline);
		~VulkanPipeline();

		//TODO: Remove
		VkPipeline GetPipeline() const { return m_Pipeline; }
	private:
		VulkanPipeline(const VulkanDevice* device, VkPipeline pipeline, VkPipelineLayout layout);
		
		const VulkanDevice* m_Device;
		VkPipelineLayout m_PipelineLayout;
		VkPipeline m_Pipeline;
	};
}
