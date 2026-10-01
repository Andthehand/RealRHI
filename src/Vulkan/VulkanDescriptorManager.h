#pragma once
#include "Result.h"

#include "VulkanDescriptorHeaps.h"
#include "VulkanDevice.h"
#include "VulkanTexture.h"

#include <memory>
#include <vector>

namespace RealRHI {
	class VulkanDescriptorManager {
	public:
		VulkanDescriptorManager(const VulkanDevice* device);
		~VulkanDescriptorManager() = default;

		Result Init();
		void Cleanup();

		void BindDescriptorHeaps(VkCommandBuffer commandBuffer) const;

		uint32_t AllocateBufferDescriptor(const VulkanBuffer* buffer);
		void FreeBufferDescriptor(uint32_t index);

		uint32_t AllocateImageDescriptor(const VulkanTexture* texture);
		void FreeImageDescriptor(uint32_t index);

		uint32_t AllocateSamplerDescriptor(const VkSamplerCreateInfo& samplerCI);
		void FreeSamplerDescriptor(uint32_t index);

	private:
		const VulkanDevice* m_Device;
		std::unique_ptr<VulkanDescriptorHeaps> m_DescriptorHeaps;

		// Resource Heap
		VkDeviceSize m_BufferDescriptorSize;
		std::vector<uint32_t> m_FreeBufferDescriptors;
		uint32_t m_NextBufferDescriptorIndex;

		VkDeviceSize m_ImageDescriptorSize;
		std::vector<uint32_t> m_FreeImageDescriptors;
		uint32_t m_NextImageDescriptorIndex;

		// Sampler Heap
		VkDeviceSize m_SamplerDescriptorSize;
		std::vector<uint32_t> m_FreeSamplerDescriptors;
		uint32_t m_NextSamplerDescriptorIndex;
	};
}
