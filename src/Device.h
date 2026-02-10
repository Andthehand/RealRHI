#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <optional>
#include <string>

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
     * @brief Device abstraction for Vulkan instance, physical and logical devices
     */
    class Device
    {
    public:
        Device();
        ~Device();

        /**
         * @brief Initialize and create device
         * @param appName The application name
         * @param enableValidation Enable Vulkan validation layers
         * @return true if creation succeeded
         */
        bool Create(const std::string& appName, bool enableValidation = true);

        /**
         * @brief Destroy the device and instance
         */
        void Destroy();

        /**
         * @brief Get the Vulkan instance
         */
        VkInstance GetInstance() const { return m_Instance; }

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
        VkInstance m_Instance;
        VkDebugUtilsMessengerEXT m_DebugMessenger;
        VkPhysicalDevice m_PhysicalDevice;
        VkDevice m_Device;
        VkQueue m_GraphicsQueue;
        VkQueue m_PresentQueue;
        bool m_ValidationEnabled;

        bool CreateInstance(const std::string& appName);
        bool SetupDebugMessenger();
        bool PickPhysicalDevice();
        bool CreateLogicalDevice();
        QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
    };
}
