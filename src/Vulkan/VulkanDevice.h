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

		//Allocator
		VmaAllocator GetAllocator() const { return m_Allocator; }

		std::filesystem::path GetShaderDirectory() const override { return m_ShaderDirectory; }
		bool IsDebugEnabled() const override { return m_EnableDebug; }
		void SendDebugMessage(DebugSeverity severity, DebugMessageType type, const char* message) const { m_DebugCallback({ .severity = severity, .type = type, .message = message }); }

		Result CreateWindow(const WindowDesc& desc, Ref<Window>& outWindow) override;
		Result CreateShader(const ShaderDesc& desc, Ref<Shader>& outShader) override;
		Result CreateGraphicsPipeline(const PipelineDesc& desc, Ref<Pipeline>& outPipeline) override;
		Result CreateSwapchain(const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain) override;
		Result CreateBuffer(const BufferDesc& desc, Ref<Buffer>& outBuffer) override;
		Result CreateTexture(const TextureDesc& desc, Ref<Texture>& outTexture) override;
		Result CreateCommandList(Ref<CommandList>& outCommandList) override;

		void Submit(CommandList* cmd, Swapchain* swapchain, const FrameContext& frame) override;
		Result ImmediateSubmit(CommandList* cmd) const override;
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
