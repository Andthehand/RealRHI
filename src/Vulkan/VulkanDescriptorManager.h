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

		// VkDeviceSize is used for sizes and offsets
		// m_Free* is used for free descriptor indices to reuse them
		// m_Allocated* is used for allocated descriptor indices to track them and prevent double frees

		// Resource Heap
		VkDeviceSize m_BufferDescriptorSize;
		std::vector<uint32_t> m_FreeBufferDescriptors;
		std::unordered_set<uint32_t> m_AllocatedBufferDescriptors;
		uint32_t m_NextBufferDescriptorIndex;

		VkDeviceSize m_ImageDescriptorSize;
		std::vector<uint32_t> m_FreeImageDescriptors;
		std::unordered_set<uint32_t> m_AllocatedImageDescriptors;
		uint32_t m_NextImageDescriptorIndex;
		uint32_t m_ImageDescriptorCapacity = 0;

		// Sampler Heap
		VkDeviceSize m_SamplerDescriptorSize;
		std::vector<uint32_t> m_FreeSamplerDescriptors;
		std::unordered_set<uint32_t> m_AllocatedSamplerDescriptors;
		uint32_t m_NextSamplerDescriptorIndex;
	};
}
