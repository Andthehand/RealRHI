#include "VulkanSwapchain.h"
#include <iostream>
#include <algorithm>

namespace RealRHI {
	VulkanSwapchain::VulkanSwapchain(const VulkanDevice* device, const SwapchainDesc& desc) 
        : m_Device(device) {
		CreateSurface(desc.Window);
        CreateSwapchain(VkExtent2D{ .width = desc.Width, .height = desc.Height });
	}

    VulkanSwapchain::~VulkanSwapchain() {
        vkDestroySwapchainKHR(m_Device->GetDevice(), m_Swapchain, nullptr);
		vkDestroySurfaceKHR(m_Device->GetInstance(), m_Surface, nullptr);
    }

    void VulkanSwapchain::BeginFrame() {
    }

    void VulkanSwapchain::Present() {
    }

    Texture* VulkanSwapchain::GetCurrentBackBuffer() {
        return nullptr;
    }
	
	bool VulkanSwapchain::CreateSurface(const WindowHandle& window) {
		switch (window.Type) {
		case WindowHandleType::HWND: {
			VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {};
			surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
			surfaceCreateInfo.hinstance = ::GetModuleHandle(nullptr);
			surfaceCreateInfo.hwnd = (HWND)window.HandleValues[0];
			if (vkCreateWin32SurfaceKHR(m_Device->GetInstance(), &surfaceCreateInfo, nullptr, &m_Surface)) {
				std::cerr << "Failed to create Win32 surface for VulkanSwapchain." << std::endl;
				return true;
			}
			break;
		}
		default:
			std::cerr << "Unsupported window handle type for VulkanSwapchain." << std::endl;
			return true;
		}

		return false;
	}

    SwapChainSupportDetails VulkanSwapchain::QuerySwapChainSupport() {
        SwapChainSupportDetails details;

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_Device->GetPhysicalDevice(), m_Surface, &details.Capabilities);

        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_Device->GetPhysicalDevice(), m_Surface, &formatCount, nullptr);
        if (formatCount != 0) {
            details.Formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(m_Device->GetPhysicalDevice(), m_Surface, &formatCount, details.Formats.data());
        }

        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_Device->GetPhysicalDevice(), m_Surface, &presentModeCount, nullptr);
        if (presentModeCount != 0) {
            details.PresentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_Device->GetPhysicalDevice(), m_Surface, &presentModeCount, details.PresentModes.data());
        }

        return details;
    }

    VkSurfaceFormatKHR VulkanSwapchain::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
        for (const auto& availableFormat : availableFormats) {
            if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
                availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return availableFormat;
            }
        }

        return availableFormats[0];
    }

    VkPresentModeKHR VulkanSwapchain::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
        for (const auto& availablePresentMode : availablePresentModes) {
            if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return availablePresentMode;
            }
        }

        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D VulkanSwapchain::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D requestedExtent) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        }
        else {
            requestedExtent.width = std::clamp(requestedExtent.width,
                capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            requestedExtent.height = std::clamp(requestedExtent.height,
                capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

            return requestedExtent;
        }
    }

    bool VulkanSwapchain::CreateSwapchain(VkExtent2D requestedExtent) {
        SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport();

        VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapChainSupport.Formats);
        VkPresentModeKHR presentMode = ChooseSwapPresentMode(swapChainSupport.PresentModes);
        VkExtent2D extent = ChooseSwapExtent(swapChainSupport.Capabilities, requestedExtent);

        uint32_t imageCount = swapChainSupport.Capabilities.minImageCount + 1;
        if (swapChainSupport.Capabilities.maxImageCount > 0 &&
            imageCount > swapChainSupport.Capabilities.maxImageCount) {
            imageCount = swapChainSupport.Capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = m_Surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = swapChainSupport.Capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        if (vkCreateSwapchainKHR(m_Device->GetDevice(), &createInfo, nullptr, &m_Swapchain) != VK_SUCCESS) {
			std::cerr << "Failed to create swapchain." << std::endl;
            return true;
        }

        vkGetSwapchainImagesKHR(m_Device->GetDevice(), m_Swapchain, &imageCount, nullptr);
        m_SwapchainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(m_Device->GetDevice(), m_Swapchain, &imageCount, m_SwapchainImages.data());

        m_SwapchainImageFormat = surfaceFormat.format;
        m_SwapchainExtent = extent;

		return false;
	}
}