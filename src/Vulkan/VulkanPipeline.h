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

		static Result Create(const VulkanDevice* device, const PipelineDesc& desc, Ref<VulkanPipeline>& outPipeline);
		Result Init(const PipelineDesc& desc);

		bool HasDescriptors() const { return m_DescriptorSetLayout != VK_NULL_HANDLE; }

	protected:
		friend class VulkanCommandList;
		VkPipeline GetPipeline() const { return m_Pipeline; }
		VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }
		VkDescriptorSetLayout GetDescriptorSetLayout() const { return m_DescriptorSetLayout; }
	private:
		const VulkanDevice* m_Device = nullptr;

		VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
		VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
		VkPipeline m_Pipeline = VK_NULL_HANDLE;
	};
}
