#pragma once
#include "Result.h"
#include "VulkanDevice.h"
#include "Window.h"
#include "WindowDesc.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace RealRHI {
	class VulkanWindow : public Window {
	public:
		VulkanWindow(const VulkanDevice* device);
		~VulkanWindow();

		static Result Create(const VulkanDevice* device, const WindowDesc& desc, Ref<VulkanWindow>& outWindow);
		Result Init(const WindowDesc& desc);

		uint32_t GetWidth() const override;
		uint32_t GetHeight() const override;
		bool IsMinimized() const override;

	protected:
		friend class VulkanSwapchain;

		bool CreateVulkanSurface(VkSurfaceKHR* surface) const;
		void DestroyVulkanSurface(VkSurfaceKHR surface) const;

	private:
		const VulkanDevice* m_Device = nullptr;
		SDL_Window* m_Window = nullptr;

		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
	};
}
