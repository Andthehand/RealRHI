#include "VulkanCommandAllocator.h"
#include "VulkanDevice.h"

namespace RealRHI {
    VulkanCommandAllocator::VulkanCommandAllocator(const VulkanDevice* device, uint32_t family)
        : m_Device(device), m_Family(family) {}

    Result VulkanCommandAllocator::Create(const VulkanDevice* device, uint32_t family, Ref<VulkanCommandAllocator>& out) {
        Ref<VulkanCommandAllocator> allocator = Ref<VulkanCommandAllocator>::Create(device, family);
        VkCommandPoolCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = allocator->m_Family,
        };

        if (vkCreateCommandPool(device->GetDevice(), &info, nullptr, &allocator->m_Pool) != VK_SUCCESS) {
            return Result::Failed;
        }
        out = allocator;
        return Result::Success;
    }

    VulkanCommandAllocator::~VulkanCommandAllocator() {
        if (m_Pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_Device->GetDevice(), m_Pool, nullptr);
        }
    }

    VkResult VulkanCommandAllocator::Allocate(VkCommandBuffer& out) {
        std::scoped_lock lock(m_Mutex);
        VkCommandBufferAllocateInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = m_Pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };

        const VkResult result = vkAllocateCommandBuffers(m_Device->GetDevice(), &info, &out);
        if (result == VK_SUCCESS) {
            m_LiveBuffers++;
        }
        
        return result;
    }

    void VulkanCommandAllocator::Free(VkCommandBuffer buffer) {
        if (buffer == VK_NULL_HANDLE) {
            return;
        }
        std::scoped_lock lock(m_Mutex);

        vkFreeCommandBuffers(m_Device->GetDevice(), m_Pool, 1, &buffer);
        m_LiveBuffers--;
    }

    VkResult VulkanCommandAllocator::ResetBuffer(VkCommandBuffer buffer) {
        std::scoped_lock lock(m_Mutex);
        
        return vkResetCommandBuffer(buffer, 0);
    }

    Result VulkanCommandAllocator::Reset() {
        std::scoped_lock lock(m_Mutex);
        // The initial implementation requires all lists to be released.
        // Frame-local recycling will introduce completion-aware list reuse.
        if (m_LiveBuffers != 0) {
            return Result::Failed;
        }

        return vkResetCommandPool(m_Device->GetDevice(), m_Pool, 0) == VK_SUCCESS
            ? Result::Success : Result::Failed;
    }
}
