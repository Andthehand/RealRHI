#pragma once

#include <vector>
#include <optional>
#include <string>

#include <vulkan/vulkan.h>

namespace RealEngine {
    typedef VkBool32(VKAPI_PTR* DebugCallbackFunc)(
        VkDebugUtilsMessageSeverityFlagBitsEXT           messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT                  messageTypes,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData);

    /**
     * @brief Queue family indices for different queue types
     */
    struct QueueFamilyIndices {
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;

        bool IsComplete() const {
            return graphicsFamily.has_value() && presentFamily.has_value();
        }
    };

    struct DeviceCreateInfo {
        const char* EngineName = "RealEngine";
        const char* AppName;
        
		bool EnableValidationLayers;
        DebugCallbackFunc DebugCallback;
        
        // Optional surface extensions (e.g., for windowing systems)
        std::vector<const char*> RequiredExtensions;
    };

    /**
     * @brief Device abstraction for Vulkan instance, physical and logical devices
     */
    class Device {
    public:
        Device() = default;
        ~Device();

        /**
         * @brief Initialize and create device
         * @param appName The application name
         * @param enableValidation Enable Vulkan validation layers
         * @return true if creation succeeded
         */
        bool Create(const DeviceCreateInfo& createInfo);

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
        bool m_ValidationEnabled = false;
        std::vector<const char*> m_RequiredExtensions;

        VkInstance m_Instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkDevice m_Device = VK_NULL_HANDLE;
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
        VkQueue m_PresentQueue = VK_NULL_HANDLE;

        bool CreateInstance(const char* engineName, const char* appName);
        bool SetupDebugMessenger(DebugCallbackFunc debugCallback);
        bool PickPhysicalDevice();
        bool CreateLogicalDevice();
        QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
    };
}
