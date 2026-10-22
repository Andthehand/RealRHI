#include "VulkanWindow.h"

namespace RealRHI {
	VulkanWindow::VulkanWindow(const VulkanDevice* device) : m_Device(device) {
    }

    VulkanWindow::~VulkanWindow() {
        if (m_Window) {
            SDL_DestroyWindow(m_Window);
        }
    }

	Result VulkanWindow::Create(const VulkanDevice* device, const WindowDesc& desc, Ref<VulkanWindow>& outWindow) {
        Ref<VulkanWindow> window = Ref<VulkanWindow>::Create(device);
        Result res = window->Init(desc);
        if (res != Result::Success) {
            return res;
        }

        outWindow = window;
        return Result::Success;
	}

    Result VulkanWindow::Init(const WindowDesc& desc) {
		m_Width = desc.Width;
		m_Height = desc.Height;

        m_Window = SDL_CreateWindow(desc.Title, m_Width, m_Height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!m_Window) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create SDL window.");
            return Result::Failed;
        }

		return Result::Success;
    }

	uint32_t VulkanWindow::GetWidth() const {
		int width = 0;
		int height = 0;
		if (m_Window == nullptr || !SDL_GetWindowSizeInPixels(m_Window, &width, &height)) {
			return 0;
		}
		return width > 0 ? static_cast<uint32_t>(width) : 0;
	}

	uint32_t VulkanWindow::GetHeight() const {
		int width = 0;
		int height = 0;
		if (m_Window == nullptr || !SDL_GetWindowSizeInPixels(m_Window, &width, &height)) {
			return 0;
		}
		return height > 0 ? static_cast<uint32_t>(height) : 0;
	}

	bool VulkanWindow::CreateVulkanSurface(VkSurfaceKHR* surface) const {
		return SDL_Vulkan_CreateSurface(m_Window, m_Device->GetInstance(), nullptr, surface);
	}

	void VulkanWindow::DestroyVulkanSurface(VkSurfaceKHR surface) const {
		SDL_Vulkan_DestroySurface(m_Device->GetInstance(), surface, nullptr);
	}
}
