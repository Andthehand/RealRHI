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

		Result SetTitle(const char* title) override;
		std::string GetTitle() const override;

		uint32_t GetWidth() const override;
		uint32_t GetHeight() const override;

		Result GetSize(uint32_t& width, uint32_t& height) const override;
		Result GetSizeInPixels(uint32_t& width, uint32_t& height) const override;
		Result SetSize(uint32_t width, uint32_t height) override;

		Result GetPosition(int32_t& x, int32_t& y) const override;
		Result SetPosition(int32_t x, int32_t y) override;

		float GetDisplayScale() const override;
		float GetPixelDensity() const override;

		Result SetResizable(bool resizable) override;
		Result SetBordered(bool bordered) override;
		Result SetAlwaysOnTop(bool alwaysOnTop) override;

		Result SetFullscreen(bool fullscreen) override;

		Result Show() override;
		Result Hide() override;

		Result Minimize() override;
		Result Maximize() override;
		Result Restore() override;

		bool IsVisible() const override;
		bool IsMinimized() const override;
		bool IsMaximized() const override;
		bool IsFullscreen() const override;
		bool HasInputFocus() const override;

	protected:
		friend class VulkanSwapchain;

		bool CreateVulkanSurface(VkSurfaceKHR* surface) const;
		void DestroyVulkanSurface(VkSurfaceKHR surface) const;

	private:
		const VulkanDevice* m_Device = nullptr;
		SDL_Window* m_Window = nullptr;
	};
}
