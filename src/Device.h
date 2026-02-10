#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <optional>

namespace RealEngine
{
    /**
     * @brief Queue family indices for different queue types
     */
    struct QueueFamilyIndices
    {
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;

        bool IsComplete() const
        {
            return graphicsFamily.has_value() && presentFamily.has_value();
        }
    };

    /**
     * @brief Device abstraction for Vulkan physical and logical devices
     */
    class Device
    {
    public:
        Device();
        ~Device();

        /**
         * @brief Create device from instance
         * @param instance The Vulkan instance
         * @return true if creation succeeded
         */
        bool Create(VkInstance instance);

        /**
         * @brief Destroy the device
         */
        void Destroy();

        /**
         * @brief Get the logical device
         */
        VkDevice GetDevice() const { return m_Device; }

        /**
         * @brief Get the physical device
         */
        VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }

        /**
         * @brief Get the graphics queue
         */
        VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }

    private:
        VkPhysicalDevice m_PhysicalDevice;
        VkDevice m_Device;
        VkQueue m_GraphicsQueue;
        VkQueue m_PresentQueue;

        bool PickPhysicalDevice(VkInstance instance);
        bool CreateLogicalDevice();
        QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
    };
}
