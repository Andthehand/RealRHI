#pragma once
#include "Pipeline.h"
#include "PipelineDesc.h"

#include "VulkanDevice.h"
#include "Result.h"

#include <Vulkan/vulkan.h>

namespace RealRHI {
	class VulkanPipeline : public Pipeline {
	public:
		VulkanPipeline(const VulkanDevice* device);
		~VulkanPipeline();

		static Result Create(const VulkanDevice* device, const PipelineDesc& desc, Ref<Pipeline>& outPipeline);
		Result Init(const PipelineDesc& desc);

	protected:
		friend class VulkanCommandList;
		VkPipeline GetPipeline() const { return m_Pipeline; }
	private:
		const VulkanDevice* m_Device = nullptr;

		VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
		VkPipeline m_Pipeline = VK_NULL_HANDLE;
	};
}
