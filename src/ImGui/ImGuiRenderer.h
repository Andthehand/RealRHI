#pragma once
#include "Device.h"

#include <imgui.h>

union SDL_Event;

namespace RealRHI {
	// sRGB attachments (and linear scene targets) require linear shader output.
	// Use SRGB only when writing encoded colors directly to a UNorm display target.
	enum class ImGuiColorSpace {
		Linear,
		SRGB
	};

	// The application owns the ImGui context. Optional SDL3 platform support enables
	// docking, input, and secondary OS windows with their own RHI swapchains.
	// Serialize calls; the device and context must outlive the renderer.
	class ImGuiRenderer {
	public:
		ImGuiRenderer() = default;
		~ImGuiRenderer();
		ImGuiRenderer(const ImGuiRenderer&) = delete;
		ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

		// Call with the desired context current, after configuring its fonts.
		// The module must be available in DeviceDesc::shaderDirectory.
		Result Initialize(Device& device, const char* shaderModule = "realrhi_imgui");
		void Shutdown();

		// Optional: call after Initialize, before the first frame, on the main thread.
		// Enables docking and multi-viewports using the bundled SDL3 platform backend.
		Result InitializePlatform(Window& mainWindow);
		bool ProcessEvent(const SDL_Event& event);
		void NewFrame(); // Updates platform input, timing, display size, and scaling.

		// Call after ImGui::Render(), before main viewport acquisition/presentation
		// can block window movement. Returns callback failures.
		Result RenderPlatformWindows();

		// Call after ImGui::Render(), inside an active rendering scope with one color
		// attachment. Caller owns transitions/submission. Overwrites graphics state,
		// bindings, push constants, viewport, scissor, and resource declarations.
		Result RenderDrawData(CommandList& commands,
			const ImDrawData& data,
			uint32_t sampleCount = 1,
			ImGuiColorSpace outputColorSpace = ImGuiColorSpace::Linear);

		// A borrowed Texture* encoded as ImTextureID. Keep it alive through recording;
		// the command list then retains its sampled view. Use single-sampled 2D images
		// with ShaderResource usage, in ShaderRead layout on the graphics queue.
		static ImTextureID GetTextureID(Texture* texture);

	private:
		void ShutdownPlatform();
		static void CreateViewport(ImGuiViewport* viewport);
		static void DestroyViewport(ImGuiViewport* viewport);
		static void ResizeViewport(ImGuiViewport* viewport, ImVec2 size);
		static void RenderViewport(ImGuiViewport* viewport, void*);
		static void PresentViewport(ImGuiViewport* viewport, void*);
		Result RecordViewport(ImGuiViewport* viewport);

		bool m_PlatformInitialized = false;
		bool m_PreviousTransparentPayload = false;
		ImGuiConfigFlags m_AddedConfigFlags = 0;
		Result m_PlatformResult = Result::Success;

		Device* m_Device = nullptr;
		ImGuiContext* m_Context = nullptr;

		Ref<Shader> m_Shader;
		Ref<Texture> m_Font;
		Ref<Sampler> m_Sampler;
	};
}
