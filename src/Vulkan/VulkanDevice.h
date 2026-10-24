#pragma once
#include "DeferredDeletionQueue.h"
#include "Device.h"
#include "Queue.h"

#include <volk.h>

#include <vma/vk_mem_alloc.h>

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#undef CreateWindow  // Windows.h defines a macro for CreateWindow, which conflicts with our Device::CreateWindow method
#undef MemoryBarrier // Windows.h also defines MemoryBarrier, which conflicts with the RHI barrier type.

namespace RealRHI {
	class VulkanDescriptorManager;
	class VulkanCommandQueue;

	struct QueueFamilyIndices {
		std::optional<uint32_t> graphicsFamily;
		std::optional<uint32_t> presentFamily;
		std::optional<uint32_t> computeFamily;
		std::optional<uint32_t> transferFamily;

		bool IsComplete() const { return graphicsFamily.has_value() && presentFamily.has_value(); }
	};

	class VulkanDevice : public Device {
	public:
		VulkanDevice();
		~VulkanDevice();

		Result Init(const DeviceDesc& desc);

		VkInstance GetInstance() const { return m_Instance; }
		VkDevice GetDevice() const { return m_Device; }
		VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
		VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
		VkQueue GetPresentQueue() const { return m_PresentQueue; }
		uint32_t GetGraphicsQueueFamily() const { return m_GraphicsQueueFamily; }
		uint32_t GetPresentQueueFamily() const { return m_PresentQueueFamily; }
		CommandQueue* GetGraphicsCommandQueue() const override;
		CommandQueue* GetCommandQueue(QueueType type) const override;
		VulkanCommandQueue* GetCommandQueue(const QueueId& queueId) const;
		std::recursive_mutex& GetStateTrackingMutex() const { return m_StateTrackingMutex; }

		VmaAllocator GetAllocator() const { return m_Allocator; }
		VulkanDescriptorManager* GetDescriptorManager() const { return m_DescriptorManager.get(); }

		std::filesystem::path GetShaderDirectory() const override { return m_ShaderDirectory; }
		bool IsDebugEnabled() const override { return m_EnableDebug; }
		void SendDebugMessage(DebugSeverity severity, DebugMessageType type, const char* message) const {
			m_DebugCallback({.severity = severity, .type = type, .message = message});
		}

		Result CreateWindow(const WindowDesc& desc, Ref<Window>& outWindow) const override;
		Result CreateShader(const ShaderDesc& desc, Ref<Shader>& outShader) const override;
		Result CreateGraphicsPipeline(const PipelineDesc& desc, Ref<Pipeline>& outPipeline) const override;
		Result CreateComputePipeline(const ComputePipelineDesc& desc, Ref<Pipeline>& outPipeline) const override;
		Result CreateSwapchain(const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain) const override;
		Result CreateBuffer(const BufferDesc& desc, Ref<Buffer>& outBuffer) const override;
		Result CreateTexture(const TextureDesc& desc, Ref<Texture>& outTexture) const override;
		Result CreateCommandAllocator(QueueType type, Ref<CommandAllocator>& outAllocator) const override;
		Result CreateCommandList(CommandAllocator* allocator, Ref<CommandList>& outCommandList) const override;

		void BindDescriptorHeaps(VkCommandBuffer commandBuffer) const;

		Result Submit(CommandList* cmd,
			Swapchain* swapchain,
			const FrameContext& frame,
			SubmissionToken* outToken = nullptr,
			std::span<const SubmissionToken> waitFor = {}) override;
		Result ImmediateSubmit(CommandList* cmd) const override;
		void WaitIdle() override;
		void DeferDeletion(std::vector<SubmissionToken> dependencies, std::function<void()> destroy) const override;
		void ProcessDeferredDeletions() override;

		VkPhysicalDeviceDescriptorHeapPropertiesEXT GetDescriptorHeapProperties() const;
		uint32_t GetMaxPushDataSize() const { return m_MaxPushDataSize; }
		bool SupportsWireframe() const { return m_SupportsWireframe; }

	private:
		bool CreateInstance(const char* appName, bool enableValidationLayer);
		bool SetupDebugMessenger();
		bool SupportsRequiredExtensions(VkPhysicalDevice device) const;
		bool SupportsRequiredFeatures(VkPhysicalDevice device) const;
		int RatePhysicalDevice(VkPhysicalDevice device);
		bool PickPhysicalDevice();
		bool CreateLogicalDevice();
		QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
		bool CreateAllocator();
		bool CreateCommandQueues();

		static VKAPI_ATTR VkBool32 VulkanDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT type,
			const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
			void* userData);

	private:
		VkInstance m_Instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
		VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_Device = VK_NULL_HANDLE;
		uint32_t m_GraphicsQueueFamily = 0;
		uint32_t m_PresentQueueFamily = 0;
		uint32_t m_ComputeQueueFamily = 0;
		uint32_t m_TransferQueueFamily = 0;
		VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
		VkQueue m_PresentQueue = VK_NULL_HANDLE;
		VulkanCommandQueue* m_GraphicsCommandQueue = nullptr;
		std::unordered_map<QueueId, VulkanCommandQueue*> m_Queues;
		mutable DeferredDeletionQueue m_DeferredDeletions;
		mutable std::recursive_mutex m_StateTrackingMutex;

		std::unique_ptr<VulkanDescriptorManager> m_DescriptorManager;

		VmaAllocator m_Allocator = VK_NULL_HANDLE;
		uint32_t m_MaxPushDataSize = 0;
		bool m_SupportsWireframe = false;

		std::filesystem::path m_ShaderDirectory;
		DebugCallback m_DebugCallback;
		bool m_EnableDebug;

		static constexpr std::array<const char*, 5> s_DeviceExtensions{VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
			VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
			VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME};
	};
}
