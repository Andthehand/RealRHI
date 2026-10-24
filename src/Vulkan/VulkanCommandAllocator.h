#pragma once
#include "CommandAllocator.h"

#include <volk.h>

#include <cstdint>
#include <mutex>

namespace RealRHI {
	class VulkanDevice;
	class VulkanCommandAllocator final : public CommandAllocator {
	public:
		explicit VulkanCommandAllocator(const VulkanDevice* device, uint32_t family);
		~VulkanCommandAllocator() override;

		static Result Create(const VulkanDevice* device, uint32_t family, Ref<VulkanCommandAllocator>& out);

		Result Reset() override;
		VkResult Allocate(VkCommandBuffer& out);
		void Free(VkCommandBuffer buffer);
		VkResult ResetBuffer(VkCommandBuffer buffer);

		VkCommandPool GetPool() const { return m_Pool; }
		uint32_t GetQueueFamilyIndex() const { return m_Family; }

	private:
		const VulkanDevice* m_Device;
		VkCommandPool m_Pool = VK_NULL_HANDLE;
		uint32_t m_Family = 0;
		std::mutex m_Mutex;
		uint32_t m_LiveBuffers = 0;
	};
}
