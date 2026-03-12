#pragma once
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanWindow.h"
#include "Result.h"

#include "Swapchain.h"
#include "SwapchainDesc.h"

#include "VulkanTexture.h"

#include <Vulkan/vulkan.h>

namespace RealRHI {
	struct SwapChainSupportDetails {
		VkSurfaceCapabilitiesKHR Capabilities;
		std::vector<VkSurfaceFormatKHR> Formats;
		std::vector<VkPresentModeKHR> PresentModes;
	};

	class VulkanSwapchain : public Swapchain {
	public:
		VulkanSwapchain(const VulkanDevice* device);
		~VulkanSwapchain();

		static Result Create(const VulkanDevice* device, const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain);
		Result Init(const SwapchainDesc& desc);

		TextureFormat GetBackBufferFormat() const override { return m_SwapchainImageFormat; }

		FrameContext BeginFrame() override;
		void Present(const FrameContext& frame) override;
		uint32_t GetMaxFramesInFlight() const override { return MAX_FRAMES_IN_FLIGHT; }
	protected:
		friend class VulkanDevice;
		TextureView* GetBackBufferView(uint32_t imageIndex);

		// Internal methods used by VulkanDevice::Submit
		VkCommandBuffer RecordPreTransitionCmd(uint32_t frameIndex, uint32_t imageIndex);
		VkCommandBuffer RecordPostTransitionCmd(uint32_t frameIndex, uint32_t imageIndex);
		VkSemaphore GetImageAvailableSemaphore(uint32_t frameIndex) const { return m_FrameSync[frameIndex].imageAvailableSemaphore; }
		VkSemaphore GetRenderFinishedSemaphore(uint32_t imageIndex) const { return m_RenderFinishedSemaphores[imageIndex]; }
		VkFence GetFence(uint32_t frameIndex) const { return m_FrameSync[frameIndex].fence; }
	private:
		SwapChainSupportDetails QuerySwapChainSupport();
		VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
		VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
		VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D requestedExtent);

		void RecreateSwapchain();
		void TransitionToColorAttachment(VkCommandBuffer cmdBuf, uint32_t imageIndex);
		void TransitionToPresent(VkCommandBuffer cmdBuf, uint32_t imageIndex);
	private:
		struct FrameSync {
			VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
			VkFence fence = VK_NULL_HANDLE;
			VkCommandBuffer preCmdBuf = VK_NULL_HANDLE;
			VkCommandBuffer postCmdBuf = VK_NULL_HANDLE;
		};

		static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
		const VulkanDevice* m_Device = nullptr;
		const VulkanWindow* m_Window = nullptr;
		VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
		VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;

		std::vector<Ref<VulkanTexture>> m_SwapchainImages;
		std::vector<VkImageLayout> m_ImageLayouts;
		TextureFormat m_SwapchainImageFormat = TextureFormat::Unknown;
		VkExtent2D m_SwapchainExtent = { .width = 0, .height = 0 };

		std::vector<FrameSync> m_FrameSync;
		std::vector<VkSemaphore> m_RenderFinishedSemaphores;
		VkCommandPool m_TransitionPool = VK_NULL_HANDLE;
		uint32_t m_CurrentFrameIndex = 0;
	};
}
