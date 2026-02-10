#include "CommandBuffer.h"

namespace RealEngine {
    CommandBuffer::CommandBuffer()
        : m_Device(VK_NULL_HANDLE)
        , m_CommandPool(VK_NULL_HANDLE)
        , m_CommandBuffer(VK_NULL_HANDLE) {}

    CommandBuffer::~CommandBuffer() {
        Destroy();
    }

    bool CommandBuffer::Create(VkDevice device, uint32_t queueFamilyIndex) {
        m_Device = device;

        if (!CreateCommandPool(queueFamilyIndex)) {
            return false;
        }

        if (!AllocateCommandBuffer()) {
            return false;
        }

        return true;
    }

    void CommandBuffer::Destroy() {
        if (m_CommandBuffer != VK_NULL_HANDLE && m_CommandPool != VK_NULL_HANDLE && m_Device != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &m_CommandBuffer);
            m_CommandBuffer = VK_NULL_HANDLE;
        }

        if (m_CommandPool != VK_NULL_HANDLE && m_Device != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
            m_CommandPool = VK_NULL_HANDLE;
        }
    }

    bool CommandBuffer::Begin() {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        if (vkBeginCommandBuffer(m_CommandBuffer, &beginInfo) != VK_SUCCESS) {
            return false;
        }

        return true;
    }

    bool CommandBuffer::End() {
        if (vkEndCommandBuffer(m_CommandBuffer) != VK_SUCCESS) {
            return false;
        }

        return true;
    }

    bool CommandBuffer::CreateCommandPool(uint32_t queueFamilyIndex) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        if (vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool) != VK_SUCCESS) {
            return false;
        }

        return true;
    }

    bool CommandBuffer::AllocateCommandBuffer() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_CommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(m_Device, &allocInfo, &m_CommandBuffer) != VK_SUCCESS) {
            return false;
        }

        return true;
    }
}
