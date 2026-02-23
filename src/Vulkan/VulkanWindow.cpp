#include "VulkanWindow.h"

namespace RealRHI {
	VulkanWindow::VulkanWindow(const WindowDesc& desc) 
        : m_Width(desc.Width), m_Height(desc.Height) {
        m_Window = SDL_CreateWindow(desc.Title, desc.Width, desc.Height, SDL_WINDOW_VULKAN);
        if (!m_Window) {
			return; // TODO: Handle this error properly
        }
	}

    VulkanWindow::~VulkanWindow() {
        if (m_Window) {
            SDL_DestroyWindow(m_Window);
        }
    }

    bool VulkanWindow::CreateVulkanSurface(const VulkanDevice& device, VkSurfaceKHR* surface) const {
        return SDL_Vulkan_CreateSurface(m_Window, device.GetInstance(), nullptr, surface);
    }
    void VulkanWindow::DestroyVulkanSurface(const VulkanDevice& device, VkSurfaceKHR surface) const {
		SDL_Vulkan_DestroySurface(device.GetInstance(), surface, nullptr);
    }
}