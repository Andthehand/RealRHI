#include "FrameResources.h"
#include "ImGuiRenderer.h"
#include "Vulkan/VulkanWindow.h"

#include <imgui_impl_sdl3.h>

namespace RealRHI {
	namespace {
		struct ViewportData {
			Ref<VulkanWindow> window;
			Ref<Swapchain> swapchain;
			FrameResources frames;

			FrameContext frame;
			bool submitted = false;
			bool failed = false;
		};

		ImGuiRenderer* Renderer() {
			return static_cast<ImGuiRenderer*>(ImGui::GetIO().BackendRendererUserData);
		}
	}

	Result ImGuiRenderer::InitializePlatform(Window& mainWindow) {
		if (!m_Device || ImGui::GetCurrentContext() != m_Context || m_PlatformInitialized ||
			ImGui::GetIO().BackendPlatformUserData) {
			return Result::InvalidParameter;
		}

		auto* window = dynamic_cast<VulkanWindow*>(&mainWindow);
		if (!window || !dynamic_cast<VulkanDevice*>(m_Device)) {
			return Result::InvalidParameter;
		}

		auto& io = ImGui::GetIO();
		constexpr auto flags = ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
		m_AddedConfigFlags = flags & ~io.ConfigFlags;
		io.ConfigFlags |= flags;

		if (!ImGui_ImplSDL3_InitForVulkan(window->GetSDLWindow())) {
			io.ConfigFlags &= ~m_AddedConfigFlags;
			m_AddedConfigFlags = 0;
			return Result::Failed;
		}

		// SDL's viewport platform backend is unavailable on some window systems.
		if (!(io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports)) {
			ImGui_ImplSDL3_Shutdown();
			io.ConfigFlags &= ~m_AddedConfigFlags;
			m_AddedConfigFlags = 0;
			return Result::Failed;
		}

		m_PlatformInitialized = true;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;

		// A docking preview spanning independently presented OS windows can split
		// while one window moves. ImGui's transparent-payload mode draws the preview
		// only in the destination viewport and reveals it through the dragged window.
		m_PreviousTransparentPayload = io.ConfigDockingTransparentPayload;
		io.ConfigDockingTransparentPayload = true;

		auto& platform = ImGui::GetPlatformIO();
		platform.Renderer_CreateWindow = CreateViewport;
		platform.Renderer_DestroyWindow = DestroyViewport;
		platform.Renderer_SetWindowSize = ResizeViewport;
		platform.Renderer_RenderWindow = RenderViewport;
		platform.Renderer_SwapBuffers = PresentViewport;

		return Result::Success;
	}

	bool ImGuiRenderer::ProcessEvent(const SDL_Event& event) {
		return m_PlatformInitialized && ImGui::GetCurrentContext() == m_Context && ImGui_ImplSDL3_ProcessEvent(&event);
	}

	void ImGuiRenderer::NewFrame() {
		if (m_PlatformInitialized && ImGui::GetCurrentContext() == m_Context) {
			ImGui_ImplSDL3_NewFrame();
		}
	}

	Result ImGuiRenderer::RenderPlatformWindows() {
		if (!m_PlatformInitialized || ImGui::GetCurrentContext() != m_Context) {
			return Result::InvalidParameter;
		}

		m_PlatformResult = Result::Success;
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();

		return m_PlatformResult;
	}

	void ImGuiRenderer::ShutdownPlatform() {
		if (!m_PlatformInitialized) {
			return;
		}

		// Renderer callbacks must run before SDL destroys the borrowed OS windows.
		m_Device->WaitIdle();
		ImGui::DestroyPlatformWindows();

		auto& platform = ImGui::GetPlatformIO();
		platform.Renderer_CreateWindow = nullptr;
		platform.Renderer_DestroyWindow = nullptr;
		platform.Renderer_SetWindowSize = nullptr;
		platform.Renderer_RenderWindow = nullptr;
		platform.Renderer_SwapBuffers = nullptr;

		ImGui_ImplSDL3_Shutdown();

		auto& io = ImGui::GetIO();
		io.BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;
		io.ConfigFlags &= ~m_AddedConfigFlags;
		io.ConfigDockingTransparentPayload = m_PreviousTransparentPayload;
		m_AddedConfigFlags = 0;
		m_PlatformInitialized = false;
	}

	void ImGuiRenderer::CreateViewport(ImGuiViewport* viewport) {
		auto* renderer = Renderer();
		auto* data = new ViewportData;
		viewport->RendererUserData = data;

		// SDL3 stores the window ID, rather than SDL_Window*, in PlatformHandle.
		auto* window =
			SDL_GetWindowFromID(static_cast<SDL_WindowID>(reinterpret_cast<uintptr_t>(viewport->PlatformHandle)));
		if (window) {
			data->window = VulkanWindow::Borrow(static_cast<VulkanDevice*>(renderer->m_Device), window);
		}

		// The main viewport paces the application. Secondary FIFO swapchains add
		// independent presentation waits and make dragging/docking trail the mouse.
		if (!window ||
			renderer->m_Device->CreateSwapchain(
				{.window = data->window.Raw(), .enableVSync = false}, data->swapchain) != Result::Success ||
			data->frames.Initialize(
				*renderer->m_Device, QueueType::Graphics, data->swapchain->GetMaxFramesInFlight()) != Result::Success) {
			data->failed = true;
			renderer->m_PlatformResult = Result::Failed;
		}
	}

	void ImGuiRenderer::DestroyViewport(ImGuiViewport* viewport) {
		if (auto* data = static_cast<ViewportData*>(viewport->RendererUserData)) {
			Renderer()->m_Device->WaitIdle();
			delete data;
			viewport->RendererUserData = nullptr;
		}
	}

	void ImGuiRenderer::ResizeViewport(ImGuiViewport*, ImVec2) {
		// RHI swapchains detect the current pixel extent and recreate at BeginFrame.
	}

	Result ImGuiRenderer::RecordViewport(ImGuiViewport* viewport) {
		auto* data = static_cast<ViewportData*>(viewport->RendererUserData);
		if (!data || data->failed) {
			return Result::Failed;
		}

		data->submitted = false;
		if (data->window->IsMinimized() || data->window->GetWidth() == 0 || data->window->GetHeight() == 0) {
			return Result::Success;
		}

		data->frame = data->swapchain->BeginFrame();
		const auto& frame = data->frame;
		if (!frame.backBufferView) {
			return Result::Failed;
		}

		CommandList* commands = nullptr;
		if (data->frames.BeginSlot(frame.frameIndex, commands) != Result::Success ||
			commands->Begin() != Result::Success) {
			return Result::Failed;
		}

		commands->BeginRendering(
			{.colorAttachments =
					{{.target = frame.backBufferView,
						.loadOp = (viewport->Flags & ImGuiViewportFlags_NoRendererClear) ? LoadOp::Load : LoadOp::Clear,
						.storeOp = StoreOp::Store,
						.clearColor = {0, 0, 0, 1}}},
				.renderArea = {.width = frame.width, .height = frame.height}});

		const auto format = data->swapchain->GetBackBufferFormat();
		const auto outputColorSpace = (format == TextureFormat::RGBA8_SRGB || format == TextureFormat::BGRA8_SRGB)
			? ImGuiColorSpace::Linear
			: ImGuiColorSpace::SRGB;

		auto result = RenderDrawData(*commands, *viewport->DrawData, 1, outputColorSpace);
		commands->EndRendering();

		auto endResult = commands->End();
		if (result != Result::Success || endResult != Result::Success) {
			return Result::Failed;
		}

		SubmissionToken token;
		result = m_Device->Submit(commands, data->swapchain.Raw(), frame, &token);
		if (result != Result::Success) {
			return result;
		}

		data->submitted = true;

		return data->frames.TrackSubmission(frame.frameIndex, token);
	}

	void ImGuiRenderer::RenderViewport(ImGuiViewport* viewport, void*) {
		auto* renderer = Renderer();
		auto result = renderer->RecordViewport(viewport);
		if (result != Result::Success) {
			renderer->m_PlatformResult = result;
			if (auto* data = static_cast<ViewportData*>(viewport->RendererUserData)) {
				data->failed = true; // Never reuse an acquisition after a recording/submission failure.
			}
		}
	}

	void ImGuiRenderer::PresentViewport(ImGuiViewport* viewport, void*) {
		auto* data = static_cast<ViewportData*>(viewport->RendererUserData);
		if (data && data->submitted) {
			data->submitted = false;
			auto result = data->swapchain->Present(data->frame);
			if (result != Result::Success) {
				data->failed = true;
				Renderer()->m_PlatformResult = result;
			}
		}
	}
}
