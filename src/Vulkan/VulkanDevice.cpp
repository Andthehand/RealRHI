#include "Device.h"
#define VMA_IMPLEMENTATION
#include "VulkanDevice.h"

#include "VulkanWindow.h"
#include "VulkanSwapchain.h"
#include "VulkanShader.h"
#include "VulkanPipeline.h"

#include <set>
#include <iostream>

namespace RealRHI {
    VulkanDevice::VulkanDevice(const DeviceDesc& desc) 
        : m_EnableDebug(desc.enableDebug), m_ShaderDirectory(desc.shaderDirectory), m_DebugCallback(desc.debugCallback) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            // TODO: Actually handle this error
            return;
        }
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            // TODO: Actually handle this error
            return;
        }

        if (!CreateInstance(desc.applicationName, desc.enableValidationLayers)) {
            // TODO: Actually handle this error
            return;
        }

        if (desc.enableValidationLayers) {
            if (!SetupDebugMessenger()) {
                // TODO: Actually handle this error
                return;
            }
        }

        if (!PickPhysicalDevice()) {
            // TODO: Actually handle this error
            return;
        }

        if (!CreateLogicalDevice()) {
            // TODO: Actually handle this error
            return;
        }

        if (!CreateAllocator()) {
            // TODO: Actually handle this error
            return;
        }
    }

    VulkanDevice::~VulkanDevice() {
        if (m_Device != VK_NULL_HANDLE) {
			vmaDestroyAllocator(m_Allocator);
            vkDestroyDevice(m_Device, nullptr);
        }

        if (m_DebugMessenger != VK_NULL_HANDLE) {
            auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                m_Instance, "vkDestroyDebugUtilsMessengerEXT");
            if (func != nullptr) {
                func(m_Instance, m_DebugMessenger, nullptr);
            }
        }
        
        if (m_Instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_Instance, nullptr);
		}

		SDL_Vulkan_UnloadLibrary();
		SDL_Quit();
    }

    std::unique_ptr<Window> VulkanDevice::CreateWindow(const WindowDesc& desc) {
        return std::make_unique<VulkanWindow>(desc);
    }

    std::unique_ptr<Shader> VulkanDevice::CreateShader(const ShaderDesc& desc) {
        return std::make_unique<VulkanShader>((const VulkanDevice*)this, desc);
    }

    std::unique_ptr<Pipeline> VulkanDevice::CreateGraphicsPipeline(const PipelineDesc& desc) {
        return std::make_unique<VulkanPipeline>((const VulkanDevice*)this, desc);
    }

    std::unique_ptr<Swapchain> VulkanDevice::CreateSwapchain(const SwapchainDesc& desc) {
        return std::make_unique<VulkanSwapchain>((const VulkanDevice*)this, desc);
    }

    bool VulkanDevice::CreateInstance(const char* appName, bool enableValidationLayer) {
        VkApplicationInfo appInfo{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = appName,
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName = "RealEngine",
            .engineVersion = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion = VK_API_VERSION_1_3,
        };

        std::vector<const char*> extensions;
        std::vector<const char*> layers;

        // Add SDL required extensions
        uint32_t sdlExtensionsCount{ 0 };
        char const* const* sdlExtensions{ SDL_Vulkan_GetInstanceExtensions(&sdlExtensionsCount) };
        for (uint32_t i = 0; i < sdlExtensionsCount; ++i) {
            extensions.push_back(sdlExtensions[i]);
        }
        
        if (enableValidationLayer) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            layers.push_back("VK_LAYER_KHRONOS_validation");
        }

        VkInstanceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &appInfo,
            .enabledLayerCount = static_cast<uint32_t>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data()
        };

        if (vkCreateInstance(&createInfo, nullptr, &m_Instance) != VK_SUCCESS) {
            return false;
        }

        return true;
    }

    bool VulkanDevice::SetupDebugMessenger() {
        VkDebugUtilsMessengerCreateInfoEXT createInfo{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = VulkanDebugCallback,
            .pUserData = reinterpret_cast<void*>(m_DebugCallback)
        };
        

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            m_Instance, "vkCreateDebugUtilsMessengerEXT");

        if (func == nullptr) {
            return false;
        }

        if (func(m_Instance, &createInfo, nullptr, &m_DebugMessenger) != VK_SUCCESS) {
            return false;
        }

        return true;
    }

    int VulkanDevice::RatePhysicalDevice(VkPhysicalDevice device) {
        VkPhysicalDeviceProperties properties;
        VkPhysicalDeviceFeatures features;

        vkGetPhysicalDeviceProperties(device, &properties);
        vkGetPhysicalDeviceFeatures(device, &features);

        // Must support required queues
        QueueFamilyIndices indices = FindQueueFamilies(device);
        if (!indices.IsComplete())
            return 0;

        int score = 0;

        // Prefer discrete GPU
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score += 1000;

        // Slight preference for integrated if no discrete
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            score += 100;

        // Prefer larger max image dimension (usually stronger GPU)
        score += properties.limits.maxImageDimension2D;

        return score;
    }

    bool VulkanDevice::PickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);

        if (deviceCount == 0)
            return false;

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

        VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
        int bestScore = 0;

        for (const auto& device : devices) {
            int score = RatePhysicalDevice(device);

            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);

            std::cout << "Found device: " << props.deviceName << std::endl;

            if (score > bestScore) {
                bestScore = score;
                bestDevice = device;
            }
        }

        if (bestDevice == VK_NULL_HANDLE)
            return false;

        m_PhysicalDevice = bestDevice;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_PhysicalDevice, &props);
        std::cout << "Selected device: " << props.deviceName << std::endl;

        return true;
    }

    bool VulkanDevice::CreateLogicalDevice() {
        QueueFamilyIndices indices = FindQueueFamilies(m_PhysicalDevice);

        m_GraphicsQueueFamily = indices.graphicsFamily.value();
        m_PresentQueueFamily = indices.presentFamily.value();

        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::set<uint32_t> uniqueQueueFamilies = {
            m_GraphicsQueueFamily,
            m_PresentQueueFamily
        };

        float queuePriority = 1.0f;
        for (uint32_t queueFamily : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueCreateInfo{
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = queueFamily,
                .queueCount = 1,
                .pQueuePriorities = &queuePriority
            };

            queueCreateInfos.push_back(queueCreateInfo);
        }

        VkPhysicalDeviceVulkan12Features enabledVk12Features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        };
        const VkPhysicalDeviceVulkan13Features enabledVk13Features {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = &enabledVk12Features,
            .synchronization2 = true,
            .dynamicRendering = true,
        };
        VkPhysicalDeviceFeatures deviceFeatures{
            .samplerAnisotropy = VK_TRUE,
        };
        VkDeviceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &enabledVk13Features,
            .queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
            .pQueueCreateInfos = queueCreateInfos.data(),
            .enabledExtensionCount = static_cast<uint32_t>(s_DeviceExtensions.size()),
            .ppEnabledExtensionNames = s_DeviceExtensions.data(),
            .pEnabledFeatures = &deviceFeatures,
        };
        

        if (vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device) != VK_SUCCESS) {
            return false;
        }

        vkGetDeviceQueue(m_Device, m_GraphicsQueueFamily, 0, &m_GraphicsQueue);
        vkGetDeviceQueue(m_Device, m_PresentQueueFamily, 0, &m_PresentQueue);

        return true;
    }

    QueueFamilyIndices VulkanDevice::FindQueueFamilies(VkPhysicalDevice device) {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        int queueFamilyIndex = 0;
        for (const auto& queueFamily : queueFamilies) {
            if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT
                && SDL_Vulkan_GetPresentationSupport(m_Instance, device, queueFamilyIndex)) {
                indices.graphicsFamily = queueFamilyIndex;
                // TODO: Add proper present queue support for different queue families
                // For simplicity, we assume graphics and present queues are in the same family.
                // This works for most hardware but may need to be improved for some configurations.
                indices.presentFamily = queueFamilyIndex;
            }

            if (indices.IsComplete()) {
                break;
            }

            queueFamilyIndex++;
        }

        return indices;
    }

    bool VulkanDevice::CreateAllocator() {
        VmaVulkanFunctions vkFunctions{ 
            .vkGetInstanceProcAddr = vkGetInstanceProcAddr, 
            .vkGetDeviceProcAddr = vkGetDeviceProcAddr, 
            .vkCreateImage = vkCreateImage 
        };
        VmaAllocatorCreateInfo allocatorCI{ 
            .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT, 
            .physicalDevice = m_PhysicalDevice, 
            .device = m_Device, 
            .pVulkanFunctions = &vkFunctions, 
            .instance = m_Instance 
        };
        return vmaCreateAllocator(&allocatorCI, &m_Allocator);
    }

    VKAPI_ATTR VkBool32 VulkanDevice::VulkanDebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity, 
        VkDebugUtilsMessageTypeFlagsEXT type, 
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData, 
        void* userData) {
		DebugCallback debugCallback = reinterpret_cast<DebugCallback>(userData);

        DebugMessage message;
		switch (severity) {
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
                message.severity = DebugSeverity::Info;
                break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
                message.severity = DebugSeverity::Warning;
                break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
                message.severity = DebugSeverity::Error;
                break;
            default:
                message.severity = DebugSeverity::Info;
        }

        switch (type) {
            case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:
                message.type = DebugMessageType::General;
                break;
            case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:
                message.type = DebugMessageType::Validation;
                break;
            case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT:
                message.type = DebugMessageType::Performance;
                break;
            default:
                message.type = DebugMessageType::General;
		}

        message.message = callbackData->pMessage;
        debugCallback(message);

        return VK_FALSE;
    }
}
