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
        void RetireBufferDescriptor(uint32_t index, std::vector<SubmissionToken> dependencies);

		uint32_t AllocateImageDescriptor(const VulkanTextureView* textureView, VkDescriptorType descriptorType, VkImageLayout descriptorAccessLayout);
		void FreeImageDescriptor(uint32_t index);
        void RetireImageDescriptor(uint32_t index, std::vector<SubmissionToken> dependencies);

		uint32_t AllocateSamplerDescriptor(const VkSamplerCreateInfo& samplerCI);
		void FreeSamplerDescriptor(uint32_t index);
        void RetireSamplerDescriptor(uint32_t index, std::vector<SubmissionToken> dependencies);

	private:
		const VulkanDevice* m_Device;
        mutable std::mutex m_DescriptorMutex;
        std::unordered_set<uint32_t> m_RetiredBuffers;
        std::unordered_set<uint32_t> m_RetiredImages;
        std::unordered_set<uint32_t> m_RetiredSamplers;
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
