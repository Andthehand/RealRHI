#pragma once
#include "Device.h"

#include <volk.h>
#include <vma/vk_mem_alloc.h>

#include <array>
#include <memory>
#include <optional>

#undef CreateWindow // Windows.h defines a macro for CreateWindow, which conflicts with our Device::CreateWindow method

namespace RealRHI {
	class VulkanDescriptorManager;

	struct QueueFamilyIndices {
		std::optional<uint32_t> graphicsFamily;
		std::optional<uint32_t> presentFamily;

		bool IsComplete() const {
			return graphicsFamily.has_value() && presentFamily.has_value();
		}
	};

	class VulkanDevice : public Device {
	public:
		VulkanDevice() = default;
		~VulkanDevice();

		Result Init(const DeviceDesc& desc);

		// Getters for Vulkan objects, used internally by other Vulkan classes
		VkInstance GetInstance() const { return m_Instance; }
		VkDevice GetDevice() const { return m_Device; }
		VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
		VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
		VkQueue GetPresentQueue() const { return m_PresentQueue; }
		uint32_t GetGraphicsQueueFamily() const { return m_GraphicsQueueFamily; }
		uint32_t GetPresentQueueFamily() const { return m_PresentQueueFamily; }

		// Pools
		VkCommandPool GetCommandPool() const { return m_CommandPool; }

		// Allocator
		VmaAllocator GetAllocator() const { return m_Allocator; }
		VulkanDescriptorManager* GetDescriptorManager() const { return m_DescriptorManager.get(); }

		std::filesystem::path GetShaderDirectory() const override { return m_ShaderDirectory; }
		bool IsDebugEnabled() const override { return m_EnableDebug; }
		void SendDebugMessage(DebugSeverity severity, DebugMessageType type, const char* message) const { m_DebugCallback({ .severity = severity, .type = type, .message = message }); }

		// Device Creations
		Result CreateWindow(const WindowDesc& desc, Ref<Window>& outWindow) const override;
		Result CreateShader(const ShaderDesc& desc, Ref<Shader>& outShader) const override;
		Result CreateGraphicsPipeline(const PipelineDesc& desc, Ref<Pipeline>& outPipeline) const override;
		Result CreateSwapchain(const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain) const override;
		Result CreateBuffer(const BufferDesc& desc, Ref<Buffer>& outBuffer) const override;
		Result CreateTexture(const TextureDesc& desc, Ref<Texture>& outTexture) const override;
		Result CreateCommandList(Ref<CommandList>& outCommandList) const override;

		// TODO: Temp?
		void BindDescriptorHeaps(VkCommandBuffer commandBuffer) const;

		// Command submission and synchronization
		void Submit(CommandList* cmd, Swapchain* swapchain, const FrameContext& frame) override;
		Result ImmediateSubmit(CommandList* cmd) const override;
		void WaitIdle() override;

		// Properties
		VkPhysicalDeviceDescriptorHeapPropertiesEXT GetDescriptorHeapProperties() const;
	private:
		bool CreateInstance(const char* appName, bool enableValidationLayer);
		bool SetupDebugMessenger();
		int RatePhysicalDevice(VkPhysicalDevice device);
		bool PickPhysicalDevice();
		bool CreateLogicalDevice();
		QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
		bool CreateAllocator();
		Result CreateCommandPool();

		static VKAPI_ATTR VkBool32 VulkanDebugCallback(
			VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT type,
			const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
			void* userData);
	private:
			// Vulkan instance and devices
			VkInstance m_Instance;
			VkDebugUtilsMessengerEXT m_DebugMessenger;
			VkPhysicalDevice m_PhysicalDevice;
			VkDevice m_Device;
			uint32_t m_GraphicsQueueFamily;
			uint32_t m_PresentQueueFamily;
			VkQueue m_GraphicsQueue;
			VkQueue m_PresentQueue;

			// Pools
			VkCommandPool m_CommandPool;
			std::unique_ptr<VulkanDescriptorManager> m_DescriptorManager;

			// Allocator
			VmaAllocator m_Allocator;

			// User Defined settings
			std::filesystem::path m_ShaderDirectory;
			DebugCallback m_DebugCallback;
			bool m_EnableDebug;

			static constexpr std::array<const char*, 5> s_DeviceExtensions{
				VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
				VK_KHR_SWAPCHAIN_EXTENSION_NAME,
				VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
				VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
				VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME
			};
	};
}
