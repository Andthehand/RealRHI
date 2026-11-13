#include "VulkanWindow.h"

#include <limits>

namespace RealRHI {
	VulkanWindow::VulkanWindow(const VulkanDevice* device)
		: m_Device(device) { }

	VulkanWindow::~VulkanWindow() {
		if (m_Window && m_OwnsWindow) {
			SDL_DestroyWindow(m_Window);
		}
	}

	Ref<VulkanWindow> VulkanWindow::Borrow(const VulkanDevice* device, SDL_Window* window) {
		auto borrowed = Ref<VulkanWindow>::Create(device);
		borrowed->m_Window = window;
		borrowed->m_OwnsWindow = false;
		return borrowed;
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
		if (m_Window || !desc.title || desc.width == 0 || desc.height == 0 ||
			desc.width > uint32_t(std::numeric_limits<int>::max()) ||
			desc.height > uint32_t(std::numeric_limits<int>::max())) {
			return Result::InvalidParameter;
		}
		SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
		if (desc.enableHighPixelDensity) {
			flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
		}

		m_Window = SDL_CreateWindow(desc.title, int(desc.width), int(desc.height), flags);
		if (!m_Window) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create SDL window.");
			return Result::Failed;
		}

		return Result::Success;
	}

	Result VulkanWindow::SetTitle(const char* title) {
		if (!title) {
			return Result::InvalidParameter;
		}
		if (!SDL_SetWindowTitle(m_Window, title)) {
			return Result::Failed;
		}

		return Result::Success;
	}

	std::string VulkanWindow::GetTitle() const {
		const char* title = SDL_GetWindowTitle(m_Window);
		return title ? title : "";
	}

	Result VulkanWindow::GetSize(uint32_t& width, uint32_t& height) const {
		int w = 0, h = 0;
		if (!SDL_GetWindowSize(m_Window, &w, &h) || w < 0 || h < 0) {
			return Result::Failed;
		}
		width = uint32_t(w);
		height = uint32_t(h);

		return Result::Success;
	}

	Result VulkanWindow::GetSizeInPixels(uint32_t& width, uint32_t& height) const {
		int w = 0, h = 0;
		if (!SDL_GetWindowSizeInPixels(m_Window, &w, &h) || w < 0 || h < 0) {
			return Result::Failed;
		}
		width = uint32_t(w);
		height = uint32_t(h);

		return Result::Success;
	}

	Result VulkanWindow::SetSize(uint32_t width, uint32_t height) {
		if (width == 0 || height == 0 || width > uint32_t(std::numeric_limits<int>::max()) ||
			height > uint32_t(std::numeric_limits<int>::max())) {
			return Result::InvalidParameter;
		}
		return SDL_SetWindowSize(m_Window, int(width), int(height)) ? Result::Success : Result::Failed;
	}

	Result VulkanWindow::GetPosition(int32_t& x, int32_t& y) const {
		int px = 0, py = 0;
		if (!SDL_GetWindowPosition(m_Window, &px, &py)) {
			return Result::Failed;
		}
		x = px;
		y = py;

		return Result::Success;
	}

	Result VulkanWindow::SetPosition(int32_t x, int32_t y) {
		return SDL_SetWindowPosition(m_Window, x, y) ? Result::Success : Result::Failed;
	}

	float VulkanWindow::GetDisplayScale() const {
		return SDL_GetWindowDisplayScale(m_Window);
	}

	float VulkanWindow::GetPixelDensity() const {
		return SDL_GetWindowPixelDensity(m_Window);
	}

	Result VulkanWindow::SetResizable(bool resizable) {
		return SDL_SetWindowResizable(m_Window, resizable) ? Result::Success : Result::Failed;
	}

	Result VulkanWindow::SetBordered(bool bordered) {
		return SDL_SetWindowBordered(m_Window, bordered) ? Result::Success : Result::Failed;
	}

	Result VulkanWindow::SetAlwaysOnTop(bool alwaysOnTop) {
		return SDL_SetWindowAlwaysOnTop(m_Window, alwaysOnTop) ? Result::Success : Result::Failed;
	}

	Result VulkanWindow::SetFullscreen(bool fullscreen) {
		return SDL_SetWindowFullscreen(m_Window, fullscreen) ? Result::Success : Result::Failed;
	}

	Result VulkanWindow::Show() {
		return SDL_ShowWindow(m_Window) ? Result::Success : Result::Failed;
	}

	Result VulkanWindow::Hide() {
		return SDL_HideWindow(m_Window) ? Result::Success : Result::Failed;
	}

	Result VulkanWindow::Minimize() {
		return SDL_MinimizeWindow(m_Window) ? Result::Success : Result::Failed;
	}

	Result VulkanWindow::Maximize() {
		return SDL_MaximizeWindow(m_Window) ? Result::Success : Result::Failed;
	}

	Result VulkanWindow::Restore() {
		return SDL_RestoreWindow(m_Window) ? Result::Success : Result::Failed;
	}

	bool VulkanWindow::IsVisible() const {
		return m_Window && (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_HIDDEN) == 0;
	}

	bool VulkanWindow::IsMaximized() const {
		return (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MAXIMIZED) != 0;
	}

	bool VulkanWindow::IsFullscreen() const {
		return (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN) != 0;
	}

	bool VulkanWindow::HasInputFocus() const {
		return (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS) != 0;
	}

	uint32_t VulkanWindow::GetWidth() const {
		int width = 0;
		int height = 0;
		if (m_Window == nullptr || !SDL_GetWindowSizeInPixels(m_Window, &width, &height)) {
			return 0;
		}
		if (width > 0) {
			return static_cast<uint32_t>(width);
		}

		return 0;
	}

	uint32_t VulkanWindow::GetHeight() const {
		int width = 0;
		int height = 0;
		if (m_Window == nullptr || !SDL_GetWindowSizeInPixels(m_Window, &width, &height)) {
			return 0;
		}
		if (height > 0) {
			return static_cast<uint32_t>(height);
		}

		return 0;
	}

	bool VulkanWindow::IsMinimized() const {
		return (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) != 0;
	}

	bool VulkanWindow::CreateVulkanSurface(VkSurfaceKHR* surface) const {
		return SDL_Vulkan_CreateSurface(m_Window, m_Device->GetInstance(), nullptr, surface);
	}

	void VulkanWindow::DestroyVulkanSurface(VkSurfaceKHR surface) const {
		SDL_Vulkan_DestroySurface(m_Device->GetInstance(), surface, nullptr);
	}
}
