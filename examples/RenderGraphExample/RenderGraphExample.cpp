#include "../ExampleSupport.h"
#include "Device.h"
#include "FrameContext.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/TransientResourcePool.h"

#include <SDL3/SDL.h>

#include <iostream>
#include <memory>

namespace {
	using namespace RealRHI;

	class RenderGraphApplication {
	public:
		~RenderGraphApplication() {
			if (m_Device) {
				m_Device->WaitIdle();
			}
		}

		bool Init() {
			const DeviceDesc desc{
				.applicationName = "RealRHI Render Graph Example",
				.shaderDirectory = "shaders",
				.enableDebug = true,
				.enableValidationLayers = true,
				.debugCallback = Examples::CaptureDebugMessage,
			};
			if (Device::Create(desc, m_Device) != Result::Success) {
				return false;
			}
			if (m_Device->CreateWindow(
					{.title = "RealRHI Render Graph - Triangle -> Postprocess -> Present", .width = 960, .height = 640},
					m_Window) != Result::Success ||
				m_Device->CreateSwapchain({.window = m_Window.Raw()}, m_Swapchain) != Result::Success) {
				return false;
			}

			m_Pool = std::make_unique<TransientResourcePool>(*m_Device, 64ull * 1024 * 1024);
			return CreatePipeline("triangle", TextureFormat::RGBA8_UNorm, m_TrianglePipeline) &&
				CreatePipeline("postprocess", m_Swapchain->GetBackBufferFormat(), m_PostprocessPipeline);
		}

		bool Run(uint32_t frameLimit) {
			uint32_t framesRendered = 0;
			bool running = true;
			while (running && (frameLimit == 0 || framesRendered < frameLimit)) {
				SDL_Event event;
				while (SDL_PollEvent(&event)) {
					if (event.type == SDL_EVENT_QUIT) {
						running = false;
					}
				}
				if (!running) {
					break;
				}
				if (m_Window->IsMinimized() || m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0) {
					SDL_Delay(16);
					continue;
				}

				const FrameContext frame = m_Swapchain->BeginFrame();
				if (!frame.backBufferView) {
					if (m_Window->IsMinimized() || m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0) {
						SDL_Delay(16);
						continue;
					}
					std::cerr << "Failed to acquire a backbuffer.\n";
					return false;
				}
				if (!DrawFrame(frame, framesRendered == 0)) {
					return false;
				}
				++framesRendered;
			}

			m_Device->WaitIdle();
			m_Pool->Collect();
			const auto statistics = m_Pool->GetStatistics();
			std::cout << "Rendered " << framesRendered << " frames; transient texture allocations: "
					  << statistics.textureAllocations << ", reuses: " << statistics.textureReuses << '\n';
			return frameLimit == 0 || framesRendered == frameLimit;
		}

	private:
		bool CreatePipeline(const char* module, TextureFormat format, Ref<Pipeline>& pipeline) {
			Ref<Shader> shader;
			if (m_Device->CreateShader({.moduleName = module}, shader) != Result::Success) {
				return false;
			}
			const PipelineDesc desc{
				.shader = shader.Raw(),
				.rasterState = {.cullMode = CullMode::None},
				.depthState = {.depthTestEnable = false, .depthWriteEnable = false},
				.renderTargetFormats = {.colorFormats = {format}},
			};
			return m_Device->CreateGraphicsPipeline(desc, pipeline) == Result::Success;
		}

		static void SetRenderArea(CommandList& commands, const FrameContext& frame) {
			commands.SetViewport({.width = static_cast<float>(frame.width), .height = static_cast<float>(frame.height)});
			commands.SetScissor({.width = frame.width, .height = frame.height});
		}

		bool DrawFrame(const FrameContext& frame, bool printPlan) {
			// Rebuild logical resources for the current extent. The pool reuses idle
			// physical textures; each Write pass initializes their discarded contents.
			RenderGraph graph(Examples::CaptureDebugMessage);
			graph.SetOptions({.cullUnusedPasses = true});
			const auto scene = graph.CreateTexture({
				.width = frame.width,
				.height = frame.height,
				.format = TextureFormat::RGBA8_UNorm,
				.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource,
				.debugName = "Triangle color",
			});
			const auto backbuffer = graph.ImportBackBuffer(*m_Swapchain, frame);
			const ResourceState colorWrite{PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite};

			graph.AddPass({
				.name = "Draw triangle",
				.resources = {{scene, colorWrite, TextureLayout::ColorAttachment, RenderGraphAccess::Write}},
			}, [&, scene](CommandList& commands) {
				// Graph-created textures resolve only when recording begins.
				commands.BeginRendering({
					.colorAttachments = {{
						.target = graph.GetTexture(scene)->GetTextureView(),
						.clearColor = {0.025f, 0.035f, 0.06f, 1.0f},
					}},
					.renderArea = {.width = frame.width, .height = frame.height},
				});
				SetRenderArea(commands, frame);
				commands.BindPipeline(m_TrianglePipeline.Raw());
				commands.Draw(3);
				commands.EndRendering();
			});

			// Reading scene establishes the dependency on Draw triangle and inserts
			// its ColorAttachment -> ShaderRead barrier without an explicit edge.
			graph.AddPass({
				.name = "Postprocess",
				.resources = {
					{scene, {PipelineStage::FragmentShader, ResourceAccess::ShaderRead}, TextureLayout::ShaderRead,
						RenderGraphAccess::Read},
					{backbuffer, colorWrite, TextureLayout::ColorAttachment, RenderGraphAccess::Write},
				},
			}, [&, scene](CommandList& commands) {
				commands.BeginRendering({
					.colorAttachments = {{.target = frame.backBufferView}},
					.renderArea = {.width = frame.width, .height = frame.height},
				});
				SetRenderArea(commands, frame);
				commands.BindPipeline(m_PostprocessPipeline.Raw());
				auto* texture = graph.GetTexture(scene);
				// Bindless accesses still need a recording-local resource declaration.
				commands.UseResource(texture);
				const uint32_t parameters[] = {
					texture->GetSampledImageDescriptorIndex(), texture->GetSamplerDescriptorIndex(),
				};
				commands.SetPushConstants(parameters, sizeof(parameters));
				commands.Draw(3);
				commands.EndRendering();
			});

			// Submit records both passes, waits for image acquisition, transitions
			// the backbuffer to Present, and presents it. No manual Present call.
			RenderGraphSubmission completion;
			if (graph.Submit(*m_Device, completion, m_Pool.get()) != Result::Success) {
				std::cerr << "Render graph submission failed.\n";
				return false;
			}
			if (printPlan) {
				std::cout << graph.GetDebugDescription() << '\n';
			}
			// Submitted recordings retain resources until completion. BeginFrame
			// waits for the presentation join before reusing its frame slot.
			return true;
		}

		// Reverse destruction order keeps the device alive for every resource.
		std::unique_ptr<Device> m_Device;
		Ref<Window> m_Window;
		Ref<Swapchain> m_Swapchain;
		Ref<Pipeline> m_TrianglePipeline;
		Ref<Pipeline> m_PostprocessPipeline;
		std::unique_ptr<TransientResourcePool> m_Pool;
	};
}

int main(int argc, char** argv) {
	uint32_t frameLimit = 0;
	if (!RealRHI::Examples::ParseFrameLimit(argc, argv, frameLimit)) {
		return 1;
	}
	bool succeeded = false;
	{
		RenderGraphApplication application;
		succeeded = application.Init() && application.Run(frameLimit);
	}
	if (!succeeded || RealRHI::Examples::s_ErrorCount != 0) {
		std::cerr << "Render graph example failed; see diagnostics above.\n";
		return 1;
	}
	return 0;
}
