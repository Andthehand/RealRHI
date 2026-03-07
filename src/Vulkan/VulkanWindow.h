#pragma once
#include "Window.h"
#include "WindowDesc.h"

#include "VulkanDevice.h"
#include "Result.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace RealRHI {
	class VulkanWindow : public Window {
	public:
		static Result Create(const WindowDesc& desc, Ref<Window>& outWindow);
		~VulkanWindow();

		uint32_t GetWidth() const override { return m_Width; }
		uint32_t GetHeight() const override { return m_Height; }
	protected:
		bool CreateVulkanSurface(const VulkanDevice& device, VkSurfaceKHR* surface) const;
		void DestroyVulkanSurface(const VulkanDevice& device, VkSurfaceKHR surface) const;
	private:
		VulkanWindow(SDL_Window* window, uint32_t width, uint32_t height);
		SDL_Window* m_Window;

		uint32_t m_Width;
		uint32_t m_Height;

		friend class VulkanSwapchain;
	};
}