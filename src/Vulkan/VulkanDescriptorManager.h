#pragma once
#include "Queue.h"
#include "Result.h"
#include "VulkanDescriptorHeaps.h"
#include "VulkanDevice.h"

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RealRHI {
	class VulkanBuffer;

	class VulkanDescriptorManager {
	public:
		VulkanDescriptorManager(const VulkanDevice* device);
		~VulkanDescriptorManager() = default;

		Result Init();
		void Cleanup();

		void BindDescriptorHeaps(VkCommandBuffer commandBuffer) const;

		uint32_t AllocateBufferDescriptor(const VulkanBuffer* buffer, const char* debugName = nullptr);
		void FreeBufferDescriptor(uint32_t index);

		uint32_t AllocateImageDescriptor(const VkImageViewCreateInfo& viewInfo,
			VkDescriptorType descriptorType,
			VkImageLayout descriptorAccessLayout,
			const char* debugName = nullptr);
		void FreeImageDescriptor(uint32_t index);

		uint32_t AllocateSamplerDescriptor(const VkSamplerCreateInfo& samplerCI, const char* debugName = nullptr);
		void FreeSamplerDescriptor(uint32_t index);
		DescriptorStatistics GetStatistics() const;

	private:
		using SamplerKey = std::array<uint32_t, 16>;
		struct SamplerKeyHash {
			size_t operator()(const SamplerKey& key) const noexcept;
		};
		struct SamplerDescriptor {
			std::optional<SamplerKey> key;
			uint64_t references = 1;
		};

	private:
		static SamplerKey MakeSamplerKey(const VkSamplerCreateInfo& info);
		void FreeDescriptor(uint32_t index,
			std::unordered_set<uint32_t>& allocatedDescriptors,
			std::vector<uint32_t>& freeDescriptors);
		const char* RetainDebugName(const char* name);
		void UpdateHighWaterMarks();

	private:
		const VulkanDevice* m_Device;
		mutable std::mutex m_DescriptorMutex;
		std::unique_ptr<VulkanDescriptorHeaps> m_DescriptorHeaps;
		// Identical descriptor bits may share a name association. Intern labels for the device
		// lifetime so freeing one slot cannot invalidate a label still used by an alias.
		std::unordered_set<std::string> m_DescriptorDebugNames;

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
		// Descriptor references belong to sampler wrappers, including texture defaults and retained recordings.
		std::unordered_map<uint32_t, SamplerDescriptor> m_AllocatedSamplerDescriptors;
		std::unordered_map<SamplerKey, uint32_t, SamplerKeyHash> m_SharedSamplers;
		uint32_t m_NextSamplerDescriptorIndex;
		uint64_t m_PeakResourceBytes = 0;
		uint64_t m_PeakSamplerCount = 0;
	};
}
