#include "ImGuiRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#if IMGUI_VERSION_NUM < 19100 || IMGUI_VERSION_NUM >= 19200
#error RealRHIImGui supports Dear ImGui 1.91.x
#endif
#ifdef IMGUI_USE_BGRA_PACKED_COLOR
#error RealRHIImGui requires the default RGBA packed vertex colors
#endif

namespace RealRHI {
	ImGuiRenderer::~ImGuiRenderer() {
		Shutdown();
	}

	ImTextureID ImGuiRenderer::GetTextureID(Texture* texture) {
		return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture));
	}

	Result ImGuiRenderer::Initialize(Device& device, const char* shaderModule) {
		if (m_Device || !ImGui::GetCurrentContext() || !shaderModule || !shaderModule[0]) {
			return Result::InvalidParameter;
		}
		auto& io = ImGui::GetIO();
		if (io.BackendRendererUserData) {
			return Result::InvalidParameter;
		}
		Ref<Shader> shader;
		auto result = device.CreateShader({.moduleName = shaderModule}, shader);
		if (result != Result::Success) {
			return result;
		}
		Ref<Sampler> sampler;
		result = device.CreateSampler({.addressU = SamplerAddressMode::ClampToEdge,
										  .addressV = SamplerAddressMode::ClampToEdge,
										  .addressW = SamplerAddressMode::ClampToEdge},
			sampler);
		if (result != Result::Success) {
			return result;
		}
		unsigned char* pixels = nullptr;
		int width = 0, height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		if (!pixels || width <= 0 || height <= 0 || uint64_t(width) * uint64_t(height) * 4 > UINT32_MAX) {
			return Result::InvalidParameter;
		}
		Ref<Texture> font;
		result = device.CreateTexture({.width = uint32_t(width),
										  .height = uint32_t(height),
										  .format = TextureFormat::RGBA8_UNorm,
										  .usage = TextureUsage::ShaderResource | TextureUsage::TransferDst,
										  .debugName = "ImGui font atlas"},
			font);
		if (result != Result::Success) {
			return result;
		}
		// UploadData is synchronous, so no upload dependency escapes initialization.
		result = font->UploadData(pixels, uint32_t(uint64_t(width) * height * 4));
		if (result != Result::Success) {
			return result;
		}
		m_Device = &device;
		m_Context = ImGui::GetCurrentContext();
		m_Shader = std::move(shader);
		m_Font = std::move(font);
		m_Sampler = std::move(sampler);
		io.Fonts->SetTexID(GetTextureID(m_Font.Raw()));
		io.BackendRendererUserData = this;
		io.BackendRendererName = "RealRHIImGui";
		io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
		return Result::Success;
	}

	void ImGuiRenderer::Shutdown() {
		if (!m_Context) {
			return;
		}
		auto* previous = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(m_Context);
		auto& io = ImGui::GetIO();
		io.Fonts->SetTexID(0);
		io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
		io.BackendRendererName = nullptr;
		io.BackendRendererUserData = nullptr;
		ImGui::SetCurrentContext(previous);
		m_Font.Reset();
		m_Shader.Reset();
		m_Sampler.Reset();
		m_Context = nullptr;
		m_Device = nullptr;
	}

	Result ImGuiRenderer::RenderDrawData(CommandList& commands, const ImDrawData& data, uint32_t sampleCount) {
		if (!m_Device || ImGui::GetCurrentContext() != m_Context || !data.Valid ||
			commands.GetState() != CommandListState::Recording) {
			return Result::InvalidParameter;
		}
		const float width = data.DisplaySize.x * data.FramebufferScale.x;
		const float height = data.DisplaySize.y * data.FramebufferScale.y;
		if (!std::isfinite(width) || !std::isfinite(height) || width > float(INT32_MAX - 128) ||
			height > float(INT32_MAX - 128)) {
			return Result::InvalidParameter;
		}
		if (width <= 0 || height <= 0 || data.TotalVtxCount == 0) {
			return Result::Success;
		}
		if (data.TotalVtxCount < 0 || data.TotalIdxCount <= 0) {
			return Result::InvalidParameter;
		}

		std::vector<ImDrawVert> vertices;
		std::vector<uint32_t> indices;
		vertices.reserve(data.TotalVtxCount);
		indices.reserve(data.TotalIdxCount);
		for (const auto* list : data.CmdLists) {
			vertices.insert(vertices.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
			// RHI index buffers use uint32_t, independently of ImDrawIdx configuration.
			indices.insert(indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
		}
		Ref<Buffer> vertexBuffer, indexBuffer;
		// Each recording gets immutable buffers. Command-list retention keeps them
		// alive through reset, including when multiple frames are in flight.
		auto result = m_Device->CreateBuffer({.size = vertices.size() * sizeof(ImDrawVert),
												 .usage = BufferUsage::Vertex,
												 .memoryUsage = MemoryUsage::CPUToGPU,
												 .initialData = vertices.data(),
												 .debugName = "ImGui vertices"},
			vertexBuffer);
		if (result != Result::Success) {
			return result;
		}
		result = m_Device->CreateBuffer({.size = indices.size() * sizeof(uint32_t),
											.usage = BufferUsage::Index,
											.memoryUsage = MemoryUsage::CPUToGPU,
											.initialData = indices.data(),
											.debugName = "ImGui indices"},
			indexBuffer);
		if (result != Result::Success) {
			return result;
		}

		GraphicsState state;
		state.raster.cullMode = CullMode::None;
		state.depth.depthTestEnable = false;
		state.depth.depthWriteEnable = false;
		state.multisample.sampleCount = sampleCount;
		state.blend.enable = true;
		state.blend.destinationAlphaFactor = BlendFactor::OneMinusSourceAlpha;
		const VertexLayout layout{
			.bindings = {{.stride = sizeof(ImDrawVert)}},
			.attributes = {
				{.location = 0, .format = VertexFormat::Float2, .offset = offsetof(ImDrawVert, pos)},
				{.location = 1, .format = VertexFormat::Float2, .offset = offsetof(ImDrawVert, uv)},
				{.location = 2, .format = VertexFormat::UNorm8x4, .offset = offsetof(ImDrawVert, col)}
			}
		};
		auto setup = [&]() {
			commands.BindShaderProgram(m_Shader.Raw());
			auto status = commands.SetGraphicsState(state);
			if (status != Result::Success) {
				return status;
			}
			status = commands.SetVertexLayout(layout);
			if (status != Result::Success) {
				return status;
			}
			commands.BindVertexBuffer(vertexBuffer.Raw());
			commands.BindIndexBuffer(indexBuffer.Raw());
			commands.SetViewport({.width = width, .height = height});
			return Result::Success;
		};
		result = setup();
		if (result != Result::Success) {
			return result;
		}
		struct PushData {
			float scale[2];
			float translate[2];
			uint32_t texture;
			uint32_t sampler;
		};
		PushData push{
			{2.f / data.DisplaySize.x, 2.f / data.DisplaySize.y},
			{-1.f - data.DisplayPos.x * 2.f / data.DisplaySize.x, -1.f - data.DisplayPos.y * 2.f / data.DisplaySize.y},
			0,
			m_Sampler->GetDescriptorIndex()
		};
		uint32_t indexOffset = 0;
		int32_t vertexOffset = 0;
		for (const auto* list : data.CmdLists) {
			for (const auto& draw : list->CmdBuffer) {
				if (draw.UserCallback) {
					if (draw.UserCallback == ImDrawCallback_ResetRenderState) {
						result = setup();
						if (result != Result::Success) {
							return result;
						}
					}
					else {
						draw.UserCallback(list, &draw);
					}
					continue;
				}
				const float minX =
					std::clamp((draw.ClipRect.x - data.DisplayPos.x) * data.FramebufferScale.x, 0.f, width);
				const float minY =
					std::clamp((draw.ClipRect.y - data.DisplayPos.y) * data.FramebufferScale.y, 0.f, height);
				const float maxX =
					std::clamp((draw.ClipRect.z - data.DisplayPos.x) * data.FramebufferScale.x, 0.f, width);
				const float maxY =
					std::clamp((draw.ClipRect.w - data.DisplayPos.y) * data.FramebufferScale.y, 0.f, height);
				if (!(maxX > minX && maxY > minY) || draw.ElemCount == 0) {
					continue;
				}
				auto* texture = reinterpret_cast<Texture*>(static_cast<uintptr_t>(draw.GetTexID()));
				if (!texture || texture->GetSampledImageDescriptorIndex() == UINT32_MAX) {
					return Result::InvalidParameter;
				}
				push.texture = texture->GetSampledImageDescriptorIndex();
				commands.ClearResourceDeclarations();
				commands.UseResource(texture);
				commands.UseResource(m_Sampler.Raw());
				commands.SetPushConstants(&push, sizeof(push));
				const auto x = int32_t(std::floor(minX)), y = int32_t(std::floor(minY));
				commands.SetScissor(
					{x, y, uint32_t(std::ceil(maxX)) - uint32_t(x), uint32_t(std::ceil(maxY)) - uint32_t(y)});
				commands.DrawIndexed(
					draw.ElemCount, 1, indexOffset + draw.IdxOffset, vertexOffset + int32_t(draw.VtxOffset));
			}
			indexOffset += uint32_t(list->IdxBuffer.Size);
			vertexOffset += list->VtxBuffer.Size;
		}
		commands.ClearResourceDeclarations();
		return Result::Success;
	}
}
