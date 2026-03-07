#include "VulkanWindow.h"

namespace RealRHI {
	Result VulkanWindow::Create(const WindowDesc& desc, Ref<Window>& outWindow) {
        SDL_Window* window = SDL_CreateWindow(desc.Title, desc.Width, desc.Height, SDL_WINDOW_VULKAN);
        if (!window) {
			return Result::Failed;
        }
		outWindow = Ref<Window>(new VulkanWindow(window, desc.Width, desc.Height));
		return Result::Success;
	}

	VulkanWindow::VulkanWindow(SDL_Window* window, uint32_t width, uint32_t height) 
        : m_Window(window), m_Width(width), m_Height(height) {
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