#pragma once
#include "Result.h"

#include "VulkanDescriptorHeaps.h"
#include "VulkanDevice.h"

#include <memory>
#include <vector>
#include <mutex>
#include <unordered_set>
#include "Queue.h"

namespace RealRHI {
	class VulkanBuffer;
	class VulkanTextureView;

	class VulkanDescriptorManager {
	public:
		VulkanDescriptorManager(const VulkanDevice* device);
		~VulkanDescriptorManager() = default;

		Result Init();
		void Cleanup();

		void BindDescriptorHeaps(VkCommandBuffer commandBuffer) const;

		uint32_t AllocateBufferDescriptor(const VulkanBuffer* buffer);
		void FreeBufferDescriptor(uint32_t index);

		uint32_t AllocateImageDescriptor(const VulkanTextureView* textureView, VkDescriptorType descriptorType, VkImageLayout descriptorAccessLayout);
		void FreeImageDescriptor(uint32_t index);

		uint32_t AllocateSamplerDescriptor(const VkSamplerCreateInfo& samplerCI);
		void FreeSamplerDescriptor(uint32_t index);

	private:
		const VulkanDevice* m_Device;
        mutable std::mutex m_DescriptorMutex;
		std::unique_ptr<VulkanDescriptorHeaps> m_DescriptorHeaps;

		// Resource Heap
		std::unordered_set<uint32_t> m_AllocatedBufferDescriptors;
		std::unordered_set<uint32_t> m_AllocatedImageDescriptors;
		std::unordered_set<uint32_t> m_AllocatedSamplerDescriptors;
		VkDeviceSize m_BufferDescriptorSize;
		std::vector<uint32_t> m_FreeBufferDescriptors;
		uint32_t m_NextBufferDescriptorIndex;

		VkDeviceSize m_ImageDescriptorSize;
		std::vector<uint32_t> m_FreeImageDescriptors;
		uint32_t m_NextImageDescriptorIndex;
		uint32_t m_ImageDescriptorCapacity = 0;

		// Sampler Heap
		VkDeviceSize m_SamplerDescriptorSize;
		std::vector<uint32_t> m_FreeSamplerDescriptors;
		uint32_t m_NextSamplerDescriptorIndex;
	};
}
