#define VMA_IMPLEMENTATION
#include "VulkanDevice.h"

#include "VulkanWindow.h"
#include "VulkanSwapchain.h"
#include "VulkanShader.h"
#include "VulkanPipeline.h"
#include "VulkanBuffer.h"
#include "VulkanCommandList.h"

#include <set>
#include <iostream>

namespace RealRHI {
    VulkanDevice::~VulkanDevice() {
        if (m_DescriptorPool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
        }

		if (m_CommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
        }

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

	Result VulkanDevice::Init(const DeviceDesc& desc) {
		m_EnableDebug = desc.enableDebug;
		m_ShaderDirectory = desc.shaderDirectory;
		m_DebugCallback = desc.debugCallback;

        if (!SDL_Init(SDL_INIT_VIDEO)) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to init SDL video.");
            return Result::Failed;
        }
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to load Vulkan library via SDL.");
            return Result::Failed;
        }

        if (!CreateInstance(desc.applicationName, desc.enableValidationLayers)) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan instance.");
            return Result::Failed;
        }

        if (desc.enableValidationLayers) {
            if (!SetupDebugMessenger()) {
				SendDebugMessage(DebugSeverity::Error, DebugMessageType::Validation, "Failed to setup Vulkan debug messenger.");
                return Result::Failed;
            }
        }

        if (!PickPhysicalDevice()) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to pick Vulkan physical device.");
            return Result::Failed;
        }

        if (!CreateLogicalDevice()) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan logical device.");
            return Result::Failed;
        }

        if (!CreateAllocator()) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan memory allocator.");
            return Result::Failed;
        }

        if (CreateCommandPool() != Result::Success) {
            return Result::Failed;
        }

		if (CreateDescriptorPool() != Result::Success) {
            return Result::Failed;
        }

		return Result::Success;
	}

    Result VulkanDevice::CreateWindow(const WindowDesc& desc, Ref<Window>& outWindow) {
        return VulkanWindow::Create(this, desc, (Ref<VulkanWindow>&)outWindow);
    }

    Result VulkanDevice::CreateShader(const ShaderDesc& desc, Ref<Shader>& outShader) {
        return VulkanShader::Create(this, desc, (Ref<VulkanShader>&)outShader);
    }

    Result VulkanDevice::CreateGraphicsPipeline(const PipelineDesc& desc, Ref<Pipeline>& outPipeline) {
        return VulkanPipeline::Create(this, desc, (Ref<VulkanPipeline>&)outPipeline);
    }

    Result VulkanDevice::CreateSwapchain(const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain) {
        return VulkanSwapchain::Create(this, desc, (Ref<VulkanSwapchain>&)outSwapchain);
    }

    Result VulkanDevice::CreateBuffer(const BufferDesc& desc, Ref<Buffer>& outBuffer) {
        return VulkanBuffer::Create(this, desc, (Ref<VulkanBuffer>&)outBuffer);
    }

    Result VulkanDevice::CreateTexture(const TextureDesc& desc, Ref<Texture>& outTexture) {
        return VulkanTexture::Create(this, desc, (Ref<VulkanTexture>&)outTexture);
    }

    Result VulkanDevice::CreateCommandList(Ref<CommandList>& outCommandList) {
		return VulkanCommandList::Create(this, (Ref<VulkanCommandList>&)outCommandList);
    }

    void VulkanDevice::Submit(CommandList* cmd, Swapchain* sc, const FrameContext& frame) {
        auto* vkSC = static_cast<VulkanSwapchain*>(sc);
        auto* vkCmd = static_cast<VulkanCommandList*>(cmd);

        VkCommandBuffer preCmdBuf = vkSC->RecordPreTransitionCmd(frame.frameIndex, frame.imageIndex);
        VkCommandBuffer postCmdBuf = vkSC->RecordPostTransitionCmd(frame.frameIndex, frame.imageIndex);

        VkCommandBufferSubmitInfo cmdInfos[] = {
            { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = preCmdBuf },
            { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = vkCmd->GetCommandBuffer() },
            { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = postCmdBuf },
        };

        VkSemaphoreSubmitInfo waitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = vkSC->GetImageAvailableSemaphore(frame.frameIndex),
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        };

        VkSemaphoreSubmitInfo signalInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = vkSC->GetRenderFinishedSemaphore(frame.imageIndex),
            .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
        };

        VkSubmitInfo2 submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = 1,
            .pWaitSemaphoreInfos = &waitInfo,
            .commandBufferInfoCount = 3,
            .pCommandBufferInfos = cmdInfos,
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos = &signalInfo,
        };

        if (vkQueueSubmit2(m_GraphicsQueue, 1, &submitInfo, vkSC->GetFence(frame.frameIndex)) != VK_SUCCESS) {
            std::cerr << "Failed to submit command buffer!" << std::endl;
        }
    }

    Result VulkanDevice::ImmediateSubmit(CommandList* cmd) const {
        auto* vkCmd = static_cast<VulkanCommandList*>(cmd);

        VkCommandBufferSubmitInfo cmdInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = vkCmd->GetCommandBuffer(),
        };

        VkSubmitInfo2 submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &cmdInfo,
        };

        VkFenceCreateInfo fenceInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };

        VkFence fence;
        if (vkCreateFence(m_Device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create fence for immediate submit.");
            return Result::Failed;
        }

        if (vkQueueSubmit2(m_GraphicsQueue, 1, &submitInfo, fence) != VK_SUCCESS) {
            SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to submit command buffer for immediate submit.");
            vkDestroyFence(m_Device, fence, nullptr);
            return Result::Failed;
        }

        if (vkWaitForFences(m_Device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to wait for fence during immediate submit.");
            vkDestroyFence(m_Device, fence, nullptr);
            return Result::Failed;
        }

        vkDestroyFence(m_Device, fence, nullptr);
        return Result::Success;
    }

    void VulkanDevice::WaitIdle() {
        vkDeviceWaitIdle(m_Device);
    }

    Result Device::Create(const DeviceDesc& desc, std::unique_ptr<Device>& outDevice) {
        std::unique_ptr<VulkanDevice> device = std::make_unique<VulkanDevice>();
		Result res = device->Init(desc);
		if (res != Result::Success) {
			return res;
		}

		outDevice = std::move(device);
		return Result::Success;
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

            std::string foundDevice = "Found device: " + std::string(props.deviceName);
			SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, foundDevice.c_str());

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
		std::string selectedDevice = "Selected device: " + std::string(props.deviceName);
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, selectedDevice.c_str());

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
            .physicalDevice = m_PhysicalDevice, 
            .device = m_Device, 
            .pVulkanFunctions = &vkFunctions, 
            .instance = m_Instance 
        };

        return vmaCreateAllocator(&allocatorCI, &m_Allocator) == VK_SUCCESS;
    }

    Result VulkanDevice::CreateCommandPool() {
        VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = m_GraphicsQueueFamily,
        };

        if (vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool) != VK_SUCCESS) {
            SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan command pool.");
            return Result::Failed;
        }

		return Result::Success;
    }

    Result VulkanDevice::CreateDescriptorPool() {
        // Mainly taken from https://github.com/shader-slang/slang-rhi/blob/99f18183f41c3aa25e3038a532c5f98d89c16c1c/src/vulkan/vk-descriptor-allocator.cpp#L8
        std::vector<VkDescriptorPoolSize> poolSizes;
        poolSizes.push_back(VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 });

        VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = 4096,
            .poolSizeCount = (uint32_t)poolSizes.size(),
            .pPoolSizes = poolSizes.data(),
        };

        if (vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool) != VK_SUCCESS) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan descriptor pool.");
			return Result::Failed;
        }

        return Result::Success;
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
