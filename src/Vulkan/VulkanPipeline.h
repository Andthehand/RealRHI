#pragma once
#include "Pipeline.h"
#include "PipelineDesc.h"

#include "DescriptorsDesc.h"

#include "VulkanDevice.h"
#include "Result.h"

#include <unordered_map>
#include <Vulkan/vulkan.h>

namespace RealRHI {
	class VulkanPipeline : public Pipeline {
	public:
		struct ReflectedBindingInfo {
			uint32_t setIndex = 0;
			uint32_t binding = 0;
			VkDescriptorType vkType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
			uint32_t descriptorCount = 1;
		};

		VulkanPipeline(const VulkanDevice* device);
		~VulkanPipeline();

		static Result Create(const VulkanDevice* device, const PipelineDesc& desc, Ref<VulkanPipeline>& outPipeline);
		Result Init(const PipelineDesc& desc);
	protected:
		friend class VulkanCommandList;
		VkPipeline GetPipeline() const { return m_Pipeline; }
		VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }
		const std::vector<VkDescriptorSet>& GetDescriptorSets() const { return m_DescriptorSets; }
		const ReflectedBindingInfo* FindBinding(const char* name) const;
	private:
		Result CreateDescriptorSetLayout(const DescriptorsDesc& desc);
		void BuildBindingLookup(const DescriptorsDesc& desc);
	private:
		const VulkanDevice* m_Device = nullptr;

		VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
		VkPipeline m_Pipeline = VK_NULL_HANDLE;

		std::vector<VkDescriptorSetLayout> m_DescriptorSetLayouts;
		std::vector<VkDescriptorSet> m_DescriptorSets;
		std::unordered_map<std::string, ReflectedBindingInfo> m_BindingLookup;
	};
}
