#pragma once
#include "Queue.h"
#include "Result.h"
#include "VulkanDescriptorHeaps.h"
#include "VulkanDevice.h"

#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

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

		uint32_t AllocateImageDescriptor(const VulkanTextureView* textureView,
			VkDescriptorType descriptorType,
			VkImageLayout descriptorAccessLayout);
		void FreeImageDescriptor(uint32_t index);

		uint32_t AllocateSamplerDescriptor(const VkSamplerCreateInfo& samplerCI);
		void FreeSamplerDescriptor(uint32_t index);

	private:
		const VulkanDevice* m_Device;
		mutable std::mutex m_DescriptorMutex;
		std::unique_ptr<VulkanDescriptorHeaps> m_DescriptorHeaps;

		// Reuse descriptor slots while tracking live indices to reject duplicate frees.
		VkDeviceSize m_BufferDescriptorSize;
		std::vector<uint32_t> m_FreeBufferDescriptors;
		std::unordered_set<uint32_t> m_AllocatedBufferDescriptors;
		uint32_t m_NextBufferDescriptorIndex;

		VkDeviceSize m_ImageDescriptorSize;
		std::vector<uint32_t> m_FreeImageDescriptors;
		std::unordered_set<uint32_t> m_AllocatedImageDescriptors;
		uint32_t m_NextImageDescriptorIndex;
		uint32_t m_ImageDescriptorCapacity = 0;

		VkDeviceSize m_SamplerDescriptorSize;
		std::vector<uint32_t> m_FreeSamplerDescriptors;
		std::unordered_set<uint32_t> m_AllocatedSamplerDescriptors;
		uint32_t m_NextSamplerDescriptorIndex;
	};
}
