#pragma once
#include "Vulkan/VulkanDevice.h"

#include "Swapchain.h"
#include "SwapchainDesc.h"

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

		// TODO: Remove these getters
		VkSurfaceKHR GetSurface() const { return m_Surface; }
		VkSwapchainKHR GetSwapchain() const { return m_Swapchain; }
		std::vector<VkImage>& GetSwapchainImages() { return m_SwapchainImages; }
		const VkFormat& GetSwapchainImageFormat() const { return m_SwapchainImageFormat; }
		VkExtent2D GetSwapchainExtent() const { return m_SwapchainExtent; }

		// TODO: Implement these functions
		void BeginFrame() override;
		void Present() override;
		Texture* GetCurrentBackBuffer() override;
	private:
		bool CreateSurface(const WindowHandle& window);
		
		SwapChainSupportDetails QuerySwapChainSupport();
		VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
		VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
		VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D requestedExtent);
		bool CreateSwapchain(VkExtent2D requestedExtent);
	private:
		const VulkanDevice* m_Device;
		VkSurfaceKHR m_Surface;
		VkSwapchainKHR m_Swapchain;

		std::vector<VkImage> m_SwapchainImages;
		VkFormat m_SwapchainImageFormat;
		VkExtent2D m_SwapchainExtent;
	};
}
