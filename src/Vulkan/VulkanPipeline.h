#pragma once
#include "DescriptorsDesc.h"
#include "Pipeline.h"
#include "PipelineDesc.h"
#include "Result.h"
#include "VulkanDevice.h"

#include <volk.h>

#include <unordered_map>

namespace RealRHI {
	class VulkanPipeline : public Pipeline {
	public:
		struct ReflectedBindingInfo {
			uint32_t setIndex = 0;
			uint32_t binding = 0;
			VkDescriptorType vkType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
			uint32_t descriptorCount = 1;
		};

	public:
		VulkanPipeline(const VulkanDevice* device);
		~VulkanPipeline();

		static Result CreateGraphics(const VulkanDevice* device,
			const PipelineDesc& desc,
			Ref<VulkanPipeline>& outPipeline);
		Result InitGraphics(const PipelineDesc& desc);

		static Result CreateCompute(const VulkanDevice* device,
			const ComputePipelineDesc& desc,
			Ref<VulkanPipeline>& outPipeline);
		Result InitCompute(const ComputePipelineDesc& desc);

	protected:
		friend class VulkanCommandList;
		VkPipeline GetPipeline() const { return m_Pipeline; }
		VkPipelineBindPoint GetBindPoint() const { return m_BindPoint; }

	private:
		const VulkanDevice* m_Device = nullptr;

		VkPipeline m_Pipeline = VK_NULL_HANDLE;
		VkPipelineBindPoint m_BindPoint = VK_PIPELINE_BIND_POINT_MAX_ENUM;
	};
}
