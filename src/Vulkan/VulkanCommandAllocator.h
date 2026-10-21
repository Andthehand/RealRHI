#pragma once
#include "CommandAllocator.h"
#include <volk.h>
#include <mutex>
#include <cstdint>

namespace RealRHI {
    class VulkanDevice;
    class VulkanCommandAllocator final : public CommandAllocator {
    public:
        explicit VulkanCommandAllocator(const VulkanDevice* device);
        ~VulkanCommandAllocator() override;

        static Result Create(const VulkanDevice* device, Ref<VulkanCommandAllocator>& out);
        
        Result Reset() override;
        VkResult Allocate(VkCommandBuffer& out);
        void Free(VkCommandBuffer buffer);
        VkResult ResetBuffer(VkCommandBuffer buffer);

        VkCommandPool GetPool() const { return m_Pool; }
        uint32_t GetQueueFamilyIndex() const { return m_Family; }
        const VulkanDevice* GetDevice() const { return m_Device; }
    private:
        const VulkanDevice* m_Device;
        VkCommandPool m_Pool = VK_NULL_HANDLE;
        uint32_t m_Family = 0;
        std::mutex m_Mutex;
        uint32_t m_LiveBuffers = 0;
    };
}
