#pragma once
#include "Device.h"

#include <imgui.h>

namespace RealRHI {

	// Renderer only. The application owns the ImGui context and platform/input backend.
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

		// Call after ImGui::Render(), inside an active rendering scope with one color
		// attachment. Caller owns transitions/submission. Overwrites graphics state,
		// bindings, push constants, viewport, scissor, and resource declarations.
		Result RenderDrawData(CommandList& commands, const ImDrawData& data, uint32_t sampleCount = 1);

		// A borrowed Texture* encoded as ImTextureID. Keep it alive through recording;
		// the command list then retains its sampled view. Use single-sampled 2D images
		// with ShaderResource usage, in ShaderRead layout on the graphics queue.
		static ImTextureID GetTextureID(Texture* texture);

	private:
		Device* m_Device = nullptr;
		ImGuiContext* m_Context = nullptr;
		Ref<Shader> m_Shader;
		Ref<Texture> m_Font;
		Ref<Sampler> m_Sampler;
	};
}
