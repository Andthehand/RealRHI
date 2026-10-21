#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION
#include "VulkanDevice.h"

#include "VulkanWindow.h"
#include "VulkanSwapchain.h"
#include "VulkanShader.h"
#include "VulkanPipeline.h"
#include "VulkanBuffer.h"
#include "VulkanCommandList.h"
#include "VulkanCommandAllocator.h"
#include "VulkanCommandQueue.h"
#include "VulkanDescriptorManager.h"

#include <set>
#include <iostream>
#include <limits>
#include <cstring>

namespace RealRHI {
	VulkanDevice::~VulkanDevice() {
		for (auto& [queueId, queue] : m_Queues) {
			delete queue;
		}
		m_Queues.clear();
		m_GraphicsCommandQueue = nullptr;

		// Guard against partial initialization - only cleanup if constructed
		if (m_DescriptorManager) {
			m_DescriptorManager->Cleanup();
		}

		if (m_Device != VK_NULL_HANDLE) {
			// Ensure all GPU work is complete before destroying device resources
			vkDeviceWaitIdle(m_Device);

			if (m_Allocator != VK_NULL_HANDLE) {
				vmaDestroyAllocator(m_Allocator);
			}
			vkDestroyDevice(m_Device, nullptr);
		}

		if (m_DebugMessenger != VK_NULL_HANDLE) {
			vkDestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
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

		if (volkInitialize() != VK_SUCCESS) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to initialize volk.");
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

		if (!CreateCommandQueues()) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan command queues.");
			return Result::Failed;
		}

		if (!CreateAllocator()) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan memory allocator.");
			return Result::Failed;
		}

		m_DescriptorManager = std::make_unique<VulkanDescriptorManager>(this);
		if (m_DescriptorManager->Init() != Result::Success) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to initialize Vulkan descriptor manager.");
			return Result::Failed;
		}

		return Result::Success;
	}

	Result VulkanDevice::CreateWindow(const WindowDesc& desc, Ref<Window>& outWindow) const {
		Ref<VulkanWindow> vkWindow;
		Result res = VulkanWindow::Create(this, desc, vkWindow);
		if (res == Result::Success) {
			outWindow = vkWindow.template As<Window>();
		}

		return res;
	}

	Result VulkanDevice::CreateShader(const ShaderDesc& desc, Ref<Shader>& outShader) const {
		Ref<VulkanShader> vkShader;
		Result res = VulkanShader::Create(this, desc, vkShader);
		if (res == Result::Success) {
			outShader = vkShader.template As<Shader>();
		}

		return res;
	}

	Result VulkanDevice::CreateGraphicsPipeline(const PipelineDesc& desc, Ref<Pipeline>& outPipeline) const {
		Ref<VulkanPipeline> vkPipeline;
		Result res = VulkanPipeline::Create(this, desc, vkPipeline);
		if (res == Result::Success) {
			outPipeline = vkPipeline.template As<Pipeline>();
		}

		return res;
	}

	Result VulkanDevice::CreateSwapchain(const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain) const {
		Ref<VulkanSwapchain> vkSwapchain;
		Result res = VulkanSwapchain::Create(this, desc, vkSwapchain);
		if (res == Result::Success) {
			outSwapchain = vkSwapchain.template As<Swapchain>();
		}

		return res;
	}

	Result VulkanDevice::CreateBuffer(const BufferDesc& desc, Ref<Buffer>& outBuffer) const {
		Ref<VulkanBuffer> vkBuffer;
		Result res = VulkanBuffer::Create(this, desc, vkBuffer);
		if (res == Result::Success) {
			outBuffer = vkBuffer.template As<Buffer>();
		}

		return res;
	}

	Result VulkanDevice::CreateTexture(const TextureDesc& desc, Ref<Texture>& outTexture) const {
		Ref<VulkanTexture> vkTexture;
		Result res = VulkanTexture::Create(this, desc, vkTexture);
		if (res == Result::Success) {
			outTexture = vkTexture.template As<Texture>();
		}

		return res;
	}

	Result VulkanDevice::CreateCommandAllocator(Ref<CommandAllocator>& outAllocator) const {
		Ref<VulkanCommandAllocator> allocator;
		const Result result = VulkanCommandAllocator::Create(this, allocator);
		if (result == Result::Success) {
			outAllocator = allocator.As<CommandAllocator>();
		}
		return result;
	}

	Result VulkanDevice::CreateCommandList(CommandAllocator* allocator, Ref<CommandList>& outCommandList) const {
		auto* vkAllocator = dynamic_cast<VulkanCommandAllocator*>(allocator);
		if (vkAllocator == nullptr || vkAllocator->GetDevice() != this) {
			return Result::Failed;
		}
		Ref<VulkanCommandList> list;
		const Result result = VulkanCommandList::Create(this, Ref<VulkanCommandAllocator>(vkAllocator), list);
		if (result == Result::Success) {
			outCommandList = list.As<CommandList>();
		}
		return result;
	}

	CommandQueue* VulkanDevice::GetGraphicsCommandQueue() const {
		return m_GraphicsCommandQueue;
	}

	VulkanCommandQueue* VulkanDevice::GetCommandQueue(const QueueId& queueId) const {
		auto it = m_Queues.find(queueId);
		return it == m_Queues.end() ? nullptr : it->second;
	}

	void VulkanDevice::BindDescriptorHeaps(VkCommandBuffer commandBuffer) const {
		m_DescriptorManager->BindDescriptorHeaps(commandBuffer);
	}


	Result VulkanDevice::Submit(CommandList* cmd, Swapchain* sc, const FrameContext& frame, SubmissionToken* outToken) {
		if (outToken) {
			*outToken = {};
		}
		if (cmd == nullptr || sc == nullptr || m_GraphicsCommandQueue == nullptr) {
			return Result::Failed;
		}

		if (frame.imageIndex == std::numeric_limits<uint32_t>::max() || frame.backBufferView == nullptr) {
			return Result::Failed;
		}

		auto* vkSC = static_cast<VulkanSwapchain*>(sc);
		auto* vkCmd = dynamic_cast<VulkanCommandList*>(cmd);
		if (vkCmd == nullptr) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Submit received a command list from the wrong backend.");
			return Result::Failed;
		}

		VkCommandBuffer preCmdBuf = vkSC->RecordPreTransitionCmd(frame.frameIndex, frame.imageIndex);
		VkCommandBuffer postCmdBuf = vkSC->RecordPostTransitionCmd(frame.frameIndex, frame.imageIndex);

		std::array<VkCommandBufferSubmitInfo, 3> cmdInfos = {
			VkCommandBufferSubmitInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = preCmdBuf },
			VkCommandBufferSubmitInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = vkCmd->GetCommandBuffer() },
			VkCommandBufferSubmitInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = postCmdBuf },
		};

		VkSemaphoreSubmitInfo waitInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = vkSC->GetImageAvailableSemaphore(frame.frameIndex),
			// Include the pre-render image layout transition in the acquire wait.
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};

		VkSemaphoreSubmitInfo signalInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = vkSC->GetRenderFinishedSemaphore(frame.imageIndex),
			// The post-render transition is a pipeline barrier, not a graphics stage.
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};

		std::array<CommandList*, 1> tracked = { cmd };

		if (!vkSC->PrepareFrameSubmission(frame.frameIndex)) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Frame was not ready for submission.");
			return Result::Failed;
		}

		const SubmissionToken token = m_GraphicsCommandQueue->SubmitTracked(
			{ cmdInfos.data(), cmdInfos.size() },
			{ &waitInfo, 1 },
			{ &signalInfo, 1 },
			tracked);
		vkSC->FinishFrameSubmission(frame.frameIndex, token);
		if (outToken && token.IsValid()) {
			*outToken = token;
		}
		return token.IsValid() ? Result::Success : Result::Failed;
	}

	Result VulkanDevice::ImmediateSubmit(CommandList* cmd) const {
		if (cmd == nullptr || m_GraphicsCommandQueue == nullptr) {
			return Result::Failed;
		}

		auto* vkCmd = dynamic_cast<VulkanCommandList*>(cmd);
		if (vkCmd == nullptr) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "ImmediateSubmit received a command list from the wrong backend.");
			return Result::Failed;
		}

		VkCommandBufferSubmitInfo cmdInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			.commandBuffer = vkCmd->GetCommandBuffer(),
		};

		VkFenceCreateInfo fenceInfo{
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		};

		VkFence fence = VK_NULL_HANDLE;
		if (vkCreateFence(m_Device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create fence for immediate submit.");
			return Result::Failed;
		}

		std::array<CommandList*, 1> tracked = { cmd };
		const SubmissionToken token = m_GraphicsCommandQueue->SubmitTracked(
			{ &cmdInfo, 1 },
			{},
			{},
			tracked,
			fence);

		if (token.value == 0) {
			vkDestroyFence(m_Device, fence, nullptr);
			return Result::Failed;
		}

		if (vkWaitForFences(m_Device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to wait for fence during immediate submit.");
			vkDestroyFence(m_Device, fence, nullptr);
			return Result::Failed;
		}

		m_GraphicsCommandQueue->Wait(token);
		vkDestroyFence(m_Device, fence, nullptr);
		return Result::Success;
	}

	void VulkanDevice::WaitIdle() {
		if (m_GraphicsCommandQueue != nullptr) {
			m_GraphicsCommandQueue->WaitIdle();
		}
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
        
        VkValidationFeatureEnableEXT syncValidation =
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validationFeatures{
            .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
            .enabledValidationFeatureCount = 1,
            .pEnabledValidationFeatures = &syncValidation,
        };

        if (enableValidationLayer) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            layers.push_back("VK_LAYER_KHRONOS_validation");

            uint32_t extensionCount = 0;
            constexpr const char* layerName = "VK_LAYER_KHRONOS_validation";
            if (vkEnumerateInstanceExtensionProperties(layerName, &extensionCount, nullptr) != VK_SUCCESS) {
                return false;
            }
            std::vector<VkExtensionProperties> layerExtensions(extensionCount);
            if (vkEnumerateInstanceExtensionProperties(layerName, &extensionCount, layerExtensions.data()) != VK_SUCCESS) {
                return false;
            }
            bool supported = false;
            for (const auto& extension : layerExtensions) {
                if (std::strcmp(extension.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0) {
                    supported = true;
                    break;
                }
            }
            if (!supported) {
                SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
                    "VK_EXT_validation_features is not advertised by the validation layer.");
                return false;
            }
            extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        }

        VkInstanceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = enableValidationLayer ? &validationFeatures : nullptr,
            .pApplicationInfo = &appInfo,
            .enabledLayerCount = static_cast<uint32_t>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data()
        };

        if (vkCreateInstance(&createInfo, nullptr, &m_Instance) != VK_SUCCESS) {
            return false;
        }

        volkLoadInstance(m_Instance);

        return true;
    }

    VkPhysicalDeviceDescriptorHeapPropertiesEXT VulkanDevice::GetDescriptorHeapProperties() const {
        VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,
            .pNext = nullptr,
        };

        VkPhysicalDeviceProperties2 physicalDeviceProperties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &descriptorHeapProperties,
        };

        vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &physicalDeviceProperties);

        return descriptorHeapProperties;
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

        if (vkCreateDebugUtilsMessengerEXT(m_Instance, &createInfo, nullptr, &m_DebugMessenger) != VK_SUCCESS) {
            return false;
        }

        return true;
    }

    int VulkanDevice::RatePhysicalDevice(VkPhysicalDevice device) {
        VkPhysicalDeviceFeatures features;

        VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT
        };
		VkPhysicalDeviceProperties2 properties2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &descriptorHeapProperties
		};

        vkGetPhysicalDeviceProperties2(device, &properties2);
        vkGetPhysicalDeviceFeatures(device, &features);

        // Temp
		// Print out heap properties for debugging
        SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, "Heap properties:");
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Sampler heap alignment: " + std::to_string(descriptorHeapProperties.samplerHeapAlignment)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Resource heap alignment: " + std::to_string(descriptorHeapProperties.resourceHeapAlignment)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Max sampler heap size: " + std::to_string(descriptorHeapProperties.maxSamplerHeapSize)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Max resource heap size: " + std::to_string(descriptorHeapProperties.maxResourceHeapSize)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Min sampler heap reserved range: " + std::to_string(descriptorHeapProperties.minSamplerHeapReservedRange)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Min sampler heap reserved range with embedded: " + std::to_string(descriptorHeapProperties.minSamplerHeapReservedRangeWithEmbedded)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Min resource heap reserved range: " + std::to_string(descriptorHeapProperties.minResourceHeapReservedRange)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Sampler descriptor size: " + std::to_string(descriptorHeapProperties.samplerDescriptorSize)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Image descriptor size: " + std::to_string(descriptorHeapProperties.imageDescriptorSize)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Buffer descriptor size: " + std::to_string(descriptorHeapProperties.bufferDescriptorSize)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Max push data size: " + std::to_string(descriptorHeapProperties.maxPushDataSize)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Max descriptor heap embedded samplers: " + std::to_string(descriptorHeapProperties.maxDescriptorHeapEmbeddedSamplers)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Sampler YCbCr conversion count: " + std::to_string(descriptorHeapProperties.samplerYcbcrConversionCount)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Sparse descriptor heaps: " + std::to_string(descriptorHeapProperties.sparseDescriptorHeaps)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, ("Protected descriptor heaps: " + std::to_string(descriptorHeapProperties.protectedDescriptorHeaps)).c_str());
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, "End of heap properties.");

		// Must support required queues
		QueueFamilyIndices indices = FindQueueFamilies(device);
		if (!indices.IsComplete()) {
			return 0;
		}

		int score = 0;

		// Must support required features
		if (descriptorHeapProperties.bufferDescriptorSize > 0) {
			score += 1000;
		}

		// Prefer discrete GPU
		if (properties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			score += 1000;
		}

		// Slight preference for integrated if no discrete
		if (properties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
			score += 100;
		}

        // Prefer larger max image dimension (usually stronger GPU)
        score += properties2.properties.limits.maxImageDimension2D;

        return score;
    }

    bool VulkanDevice::PickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);

        if (deviceCount == 0) {
            return false;
        }

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

        if (bestDevice == VK_NULL_HANDLE) {
            return false;
        }

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

        VkPhysicalDeviceDescriptorHeapFeaturesEXT heapFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
			.descriptorHeap = VK_TRUE,
        };

        VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedPtrFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR,
            .pNext = &heapFeatures,
            .shaderUntypedPointers = VK_TRUE,
		};
        
        VkPhysicalDeviceVulkan13Features enabledVk13Features {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = &untypedPtrFeatures,
            .synchronization2 = VK_TRUE,
            .dynamicRendering = VK_TRUE,
        };
		VkPhysicalDeviceVulkan12Features enabledVk12Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			.pNext = &enabledVk13Features,
		};
		enabledVk12Features.bufferDeviceAddress = VK_TRUE;
		enabledVk12Features.timelineSemaphore = VK_TRUE;
        VkPhysicalDeviceFeatures deviceFeatures{
            .samplerAnisotropy = VK_TRUE,
        };
        VkDeviceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &enabledVk12Features,
            .queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
            .pQueueCreateInfos = queueCreateInfos.data(),
            .enabledExtensionCount = static_cast<uint32_t>(s_DeviceExtensions.size()),
            .ppEnabledExtensionNames = s_DeviceExtensions.data(),
            .pEnabledFeatures = &deviceFeatures,
        };
        

        if (vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device) != VK_SUCCESS) {
            return false;
        }

        volkLoadDevice(m_Device);
        // Queues are created once in Init(), after logical device creation.
        return true;
    }

    QueueFamilyIndices VulkanDevice::FindQueueFamilies(VkPhysicalDevice device) {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        for (uint32_t queueFamilyIndex = 0; queueFamilyIndex < queueFamilyCount; ++queueFamilyIndex) {
            const auto& queueFamily = queueFamilies[queueFamilyIndex];

            if (!indices.graphicsFamily.has_value() && (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                indices.graphicsFamily = queueFamilyIndex;
            }

            if (SDL_Vulkan_GetPresentationSupport(m_Instance, device, queueFamilyIndex) && !indices.presentFamily.has_value()) {
                indices.presentFamily = queueFamilyIndex;
            }

            if (indices.IsComplete()) {
                break;
            }
        }

        if (!indices.presentFamily.has_value() && indices.graphicsFamily.has_value()) {
            indices.presentFamily = indices.graphicsFamily;
        }

        return indices;
    }

    bool VulkanDevice::CreateCommandQueues() {
        vkGetDeviceQueue(m_Device, m_GraphicsQueueFamily, 0, &m_GraphicsQueue);
        vkGetDeviceQueue(m_Device, m_PresentQueueFamily, 0, &m_PresentQueue);

        if (!m_Queues.empty()) {
            SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Command queues are already initialized.");
            return false;
        }
        m_GraphicsCommandQueue = new VulkanCommandQueue(this, m_GraphicsQueueFamily, 0, m_GraphicsQueue);
        m_Queues[QueueId{ m_GraphicsQueueFamily, 0 }] = m_GraphicsCommandQueue;

        if (m_PresentQueueFamily != m_GraphicsQueueFamily) {
            auto* presentQueue = new VulkanCommandQueue(this, m_PresentQueueFamily, 0, m_PresentQueue);
            m_Queues[QueueId{ m_PresentQueueFamily, 0 }] = presentQueue;
        }

        return true;
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

        return vmaCreateAllocator(&allocatorCI, &m_Allocator) == VK_SUCCESS;
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
