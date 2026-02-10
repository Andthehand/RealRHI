#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace RealEngine
{
    /**
     * @brief Main RHI (Rendering Hardware Interface) class
     * Provides a Vulkan abstraction layer for RealEngine
     */
    class RealRHI
    {
    public:
        RealRHI();
        ~RealRHI();

        /**
         * @brief Initialize the RHI system
         * @param appName The application name
         * @param enableValidation Enable Vulkan validation layers
         * @return true if initialization succeeded
         */
        bool Initialize(const std::string& appName, bool enableValidation = true);

        /**
         * @brief Shutdown the RHI system
         */
        void Shutdown();

        /**
         * @brief Get the Vulkan instance
         * @return VkInstance handle
         */
        VkInstance GetInstance() const { return m_Instance; }

    private:
        VkInstance m_Instance;
        VkDebugUtilsMessengerEXT m_DebugMessenger;
        bool m_ValidationEnabled;

        bool CreateInstance(const std::string& appName);
        bool SetupDebugMessenger();
    };
}
