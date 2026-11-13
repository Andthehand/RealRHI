#include "../ExampleSupport.h"
#include "FrameResources.h"
#include "ImGui/ImGuiRenderer.h"

#include <SDL3/SDL.h>

namespace {
	using namespace RealRHI;

	class Application {
	public:
		~Application() {
			if (device) {
				device->WaitIdle();
			}

			ui.Shutdown();
			if (ImGui::GetCurrentContext()) {
				ImGui::DestroyContext();
			}
		}

		bool Initialize(bool smoke) {
			if (Device::Create({.applicationName = "RealRHI ImGui docking",
								   .shaderDirectory = "shaders",
								   .enableDebug = true,
								   .enableValidationLayers = true,
								   .debugCallback = Examples::CaptureDebugMessage},
					device) != Result::Success ||
				device->CreateWindow(
					{.title = "RealRHI ImGui docking", .width = 960, .height = 640, .enableHighPixelDensity = true},
					window) != Result::Success ||
				device->CreateSwapchain({.window = window.Raw()}, swapchain) != Result::Success ||
				frames.Initialize(*device, QueueType::Graphics, swapchain->GetMaxFramesInFlight()) != Result::Success) {
				return false;
			}

			ImGui::CreateContext();
			if (smoke) {
				ImGui::GetIO().IniFilename = nullptr;
			}

			ImGui::StyleColorsDark();
			ImGui::GetStyle().WindowRounding = 0;
			ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1;

			return ui.Initialize(*device) == Result::Success && ui.InitializePlatform(*window) == Result::Success;
		}

		bool Run(uint32_t frameLimit) {
			bool detachedOpen = true;
			bool sawDocked = false, sawSecondary = false, sawClosed = false, sawRecreated = false;
			ImGuiID detachedViewport = 0;

			for (uint32_t i = 0; frameLimit == 0 || i < frameLimit; ++i) {
				SDL_Event event;
				while (SDL_PollEvent(&event)) {
					ui.ProcessEvent(event);
					if (event.type == SDL_EVENT_QUIT ||
						(event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
							event.window.windowID ==
								static_cast<SDL_WindowID>(
									reinterpret_cast<uintptr_t>(ImGui::GetMainViewport()->PlatformHandle)))) {
						return frameLimit == 0;
					}
				}

				ui.NewFrame();
				ImGui::NewFrame();

				const auto dockspace = ImGui::DockSpaceOverViewport();
				ImGui::SetNextWindowDockID(dockspace, ImGuiCond_FirstUseEver);
				ImGui::Begin("Dockable panel");
				sawDocked |= ImGui::IsWindowDocked();
				ImGui::TextUnformatted("Drag a tab to dock it, or drag a panel outside this window.");
				ImGui::Checkbox("Show detached panel", &detachedOpen);
				ImGui::End();

				if (frameLimit && i == 14) {
					detachedOpen = false;
				}
				if (frameLimit && i == 19) {
					detachedOpen = true;
				}

				if (detachedOpen) {
					auto* main = ImGui::GetMainViewport();
					ImGui::SetNextWindowPos({main->Pos.x + main->Size.x + 20, main->Pos.y + 40},
						frameLimit ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
					ImGui::SetNextWindowSize({360, 240}, ImGuiCond_FirstUseEver);
					ImGui::Begin("Detached panel", &detachedOpen);
					ImGui::TextUnformatted("This panel has its own RHI swapchain.");
					ImGui::ColorEdit3("Color", color);
					ImGui::End();
				}

				if (!frameLimit) {
					ImGui::ShowDemoWindow();
				}

				ImGui::Render();
				// Move and draw detached windows before the main swapchain can block
				// on acquisition or presentation. All windows use this frame's input.
				if (ui.RenderPlatformWindows() != Result::Success || !DrawMain()) {
					return false;
				}

				const auto& viewports = ImGui::GetPlatformIO().Viewports;
				for (int v = 1; v < viewports.Size; ++v) {
					if (!viewports[v]->RendererUserData || !viewports[v]->PlatformHandle) {
						return false;
					}

					sawSecondary = true;
					detachedViewport = viewports[v]->ID;
					if (i >= 19) {
						sawRecreated = true;
					}

					if (frameLimit && i == 6) {
						auto* native = SDL_GetWindowFromID(
							static_cast<SDL_WindowID>(reinterpret_cast<uintptr_t>(viewports[v]->PlatformHandle)));
						if (!SDL_SetWindowSize(native, 420, 300)) {
							return false;
						}
					}

					if (frameLimit && (i == 9 || i == 11)) {
						auto* native = SDL_GetWindowFromID(
							static_cast<SDL_WindowID>(reinterpret_cast<uintptr_t>(viewports[v]->PlatformHandle)));
						if (!(i == 9 ? SDL_MinimizeWindow(native) : SDL_RestoreWindow(native))) {
							return false;
						}
					}
				}

				if (frameLimit && i == 18) {
					auto* old = ImGui::FindViewportByID(detachedViewport);
					sawClosed = !old || !old->RendererUserData;
				}

				// VSync on the main viewport already paces visible frames. Only
				// throttle explicitly when every viewport is minimized or hidden.
				bool drawable = window->IsVisible() && !window->IsMinimized();
				for (int v = 1; v < viewports.Size && !drawable; ++v) {
					auto* native = SDL_GetWindowFromID(
						static_cast<SDL_WindowID>(reinterpret_cast<uintptr_t>(viewports[v]->PlatformHandle)));
					if (native) {
						const auto flags = SDL_GetWindowFlags(native);
						drawable = (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) == 0;
					}
				}

				if (!drawable) {
					SDL_Delay(10);
				}
			}

			if (frameLimit >= 24 && !(sawDocked && sawSecondary && sawClosed && sawRecreated)) {
				std::cerr << "Docking or secondary viewport creation/closure/recreation was not observed\n";
				return false;
			}

			return true;
		}

	private:
		bool DrawMain() {
			if (window->IsMinimized() || !window->GetWidth() || !window->GetHeight()) {
				return true;
			}

			auto frame = swapchain->BeginFrame();
			if (!frame.backBufferView) {
				return false;
			}

			CommandList* commands = nullptr;
			if (frames.BeginSlot(frame.frameIndex, commands) != Result::Success ||
				commands->Begin() != Result::Success) {
				return false;
			}

			commands->BeginRendering(
				{.colorAttachments = {{.target = frame.backBufferView, .clearColor = {0.08f, 0.08f, 0.1f, 1}}},
					.renderArea = {.width = frame.width, .height = frame.height}});

			const auto format = swapchain->GetBackBufferFormat();
			const auto outputColorSpace = (format == TextureFormat::RGBA8_SRGB || format == TextureFormat::BGRA8_SRGB)
				? ImGuiColorSpace::Linear
				: ImGuiColorSpace::SRGB;

			auto result = ui.RenderDrawData(*commands, *ImGui::GetDrawData(), 1, outputColorSpace);
			commands->EndRendering();

			if (commands->End() != Result::Success || result != Result::Success) {
				return false;
			}

			SubmissionToken token;
			return device->Submit(commands, swapchain.Raw(), frame, &token) == Result::Success &&
				frames.TrackSubmission(frame.frameIndex, token) == Result::Success &&
				swapchain->Present(frame) == Result::Success;
		}

		std::unique_ptr<Device> device;
		Ref<Window> window;
		Ref<Swapchain> swapchain;
		FrameResources frames;

		ImGuiRenderer ui;

		float color[3] = {0.2f, 0.6f, 0.9f};
	};
}

int main(int argc, char** argv) {
	uint32_t frameLimit;
	if (!RealRHI::Examples::ParseFrameLimit(argc, argv, frameLimit)) {
		return 1;
	}

	bool success;
	{
		Application app;
		success = app.Initialize(frameLimit != 0) && app.Run(frameLimit);
	}

	return success && RealRHI::Examples::s_ErrorCount == 0 ? 0 : 1;
}
