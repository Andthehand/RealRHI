#pragma once

#include <vulkan/vulkan.h>

namespace RealEngine {
    /**
     * @brief Command buffer abstraction for recording GPU commands
     */
    class CommandBuffer {
    public:
        CommandBuffer();
        ~CommandBuffer();

        /**
         * @brief Create command buffer
         * @param device The logical device
         * @param queueFamilyIndex The queue family index
         * @return true if creation succeeded
         */
        bool Create(VkDevice device, uint32_t queueFamilyIndex);

        /**
         * @brief Destroy the command buffer
         */
        void Destroy();

        /**
         * @brief Begin recording commands
         * @return true if begin succeeded
         */
        bool Begin();

        /**
         * @brief End recording commands
         * @return true if end succeeded
         */
        bool End();

        /**
         * @brief Get the Vulkan command buffer
         */
        VkCommandBuffer GetCommandBuffer() const { return m_CommandBuffer; }

    private:
        VkDevice m_Device;
        VkCommandPool m_CommandPool;
        VkCommandBuffer m_CommandBuffer;

        bool CreateCommandPool(uint32_t queueFamilyIndex);
        bool AllocateCommandBuffer();
    };
}
