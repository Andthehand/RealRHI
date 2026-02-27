#pragma once
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanWindow.h"

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
		VulkanSwapchain(const VulkanDevice* device, const SwapchainDesc& desc);
		~VulkanSwapchain();

		TextureFormat GetBackBufferFormat() const override { return m_SwapchainImageFormat; }

		TextureView* GetBackBufferView(uint32_t imageIndex);
		void TransitionToColorAttachment(VkCommandBuffer cmdBuf, uint32_t imageIndex);
		void TransitionToPresent(VkCommandBuffer cmdBuf, uint32_t imageIndex);

		// TODO: Remove these getters
		VkSurfaceKHR GetSurface() const { return m_Surface; }
		VkSwapchainKHR GetSwapchain() const { return m_Swapchain; }
		std::vector<VulkanTexture>& GetSwapchainImages() { return m_SwapchainImages; }
		VkExtent2D GetSwapchainExtent() const { return m_SwapchainExtent; }
	private:
		SwapChainSupportDetails QuerySwapChainSupport();
		VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
		VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
		VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D requestedExtent);
		bool CreateSwapchain(VkExtent2D requestedExtent);
	private:
		const VulkanDevice* m_Device;
		const VulkanWindow* m_Window;
		VkSurfaceKHR m_Surface;
		VkSwapchainKHR m_Swapchain;

		std::vector<VulkanTexture> m_SwapchainImages;
		std::vector<VkImageLayout> m_ImageLayouts;
		TextureFormat m_SwapchainImageFormat;
		VkExtent2D m_SwapchainExtent;
	};
}
