#pragma once
#include "Device.h"

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

#include <array>
#include <optional>

#undef CreateWindow

namespace RealRHI {
	struct QueueFamilyIndices {
		std::optional<uint32_t> graphicsFamily;
		std::optional<uint32_t> presentFamily;

		bool IsComplete() const {
			return graphicsFamily.has_value() && presentFamily.has_value();
		}
	};

	class VulkanDevice : public Device {
	public:
		VulkanDevice(const DeviceDesc& desc);
		~VulkanDevice();

		//TODO: Remove
		VkInstance GetInstance() const { return m_Instance; }
		VkDevice GetDevice() const { return m_Device; }
		VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
		VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
		VkQueue GetPresentQueue() const { return m_PresentQueue; }
		uint32_t GetGraphicsQueueFamily() const { return m_GraphicsQueueFamily; }
		uint32_t GetPresentQueueFamily() const { return m_PresentQueueFamily; }

		//Allocator
		VmaAllocator GetAllocator() const { return m_Allocator; }

		std::filesystem::path GetShaderDirectory() const override { return m_ShaderDirectory; }
		DebugCallback GetDebugCallback() const override { return m_DebugCallback; }
		bool IsDebugEnabled() const override { return m_EnableDebug; }

		Ref<Window> CreateWindow(const WindowDesc& desc) override;
		Ref<Shader> CreateShader(const ShaderDesc& desc) override;
		Ref<Pipeline> CreateGraphicsPipeline(const PipelineDesc& desc) override;
		Ref<Swapchain> CreateSwapchain(const SwapchainDesc& desc) override;
		Ref<Buffer> CreateBuffer(const BufferDesc& desc) override;
		Ref<CommandList> CreateCommandList() override;

		void Submit(CommandList* cmd, Swapchain* swapchain, const FrameContext& frame) override;
		void WaitIdle() override;
	private:
		bool CreateInstance(const char* appName, bool enableValidationLayer);
		bool SetupDebugMessenger();
		int RatePhysicalDevice(VkPhysicalDevice device);
		bool PickPhysicalDevice();
		bool CreateLogicalDevice();
		QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
		bool CreateAllocator();

		static VKAPI_ATTR VkBool32 VulkanDebugCallback(
			VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT type,
			const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
			void* userData);
	private:
			VkInstance m_Instance;
			VkDebugUtilsMessengerEXT m_DebugMessenger;
			VkPhysicalDevice m_PhysicalDevice;
			VkDevice m_Device;
			uint32_t m_GraphicsQueueFamily;
			uint32_t m_PresentQueueFamily;
			VkQueue m_GraphicsQueue;
			VkQueue m_PresentQueue;

			VmaAllocator m_Allocator;

			std::filesystem::path m_ShaderDirectory;
			DebugCallback m_DebugCallback;

			bool m_EnableDebug;

			static constexpr std::array<const char*, 2> s_DeviceExtensions{
				VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
				VK_KHR_SWAPCHAIN_EXTENSION_NAME
			};
	};
}
