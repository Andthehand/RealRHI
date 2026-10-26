#include "../ExampleSupport.h"
#include "Device.h"
#include "FrameContext.h"
#include "FrameResources.h"

#include <SDL3/SDL.h>

#include <iostream>
#include <vector>

namespace {
	constexpr uint32_t s_WindowWidth = 800;
	constexpr uint32_t s_WindowHeight = 600;

	struct Vertex {
		float pos[2];
		float color[3];
	};

	const std::vector<Vertex> vertices = {
		{{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}},
		{{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}},
		{{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}}
	};

	class TriangleApplication {
	public:
		~TriangleApplication() {
			if (m_Device) {
				m_Device->WaitIdle();
			}
		}

		bool Init() {
			const RealRHI::DeviceDesc desc{
				.applicationName = "RealRHI Triangle Example",
				.shaderDirectory = "shaders",
				.enableDebug = true,
				.enableProfiling = false,
				.enableValidationLayers = true,
				.debugCallback = RealRHI::Examples::CaptureDebugMessage,
			};
			if (RealRHI::Device::Create(desc, m_Device) != RealRHI::Result::Success) {
				return false;
			}
			CreateSwapChain();
			if (!m_Swapchain) {
				return false;
			}
			CreateGraphicsPipeline();
			if (!m_Pipeline) {
				return false;
			}

			CreateVertexBuffer();
			if (!m_VertexBuffer) {
				return false;
			}
			return m_FrameResources.Initialize(*m_Device,
					   RealRHI::QueueType::Graphics,
					   m_Swapchain->GetMaxFramesInFlight()) == RealRHI::Result::Success;
		}

		bool Run(uint32_t frameLimit) {
			bool running = true;
			uint32_t framesRendered = 0;
			while (running && (frameLimit == 0 || framesRendered < frameLimit)) {
				SDL_Event event;
				while (SDL_PollEvent(&event)) {
					if (event.type == SDL_EVENT_QUIT) {
						running = false;
					}
				}
				if (!DrawFrame(framesRendered)) {
					return false;
				}
			}
			return frameLimit == 0 || framesRendered == frameLimit;
		}

	private:
		void CreateSwapChain() {
			RealRHI::Result result = m_Device->CreateWindow(
				{.Title = "RealRHI Triangle Example", .Width = s_WindowWidth, .Height = s_WindowHeight}, m_Window);
			if (result != RealRHI::Result::Success) {
				std::cerr << "Failed to create window" << std::endl;
				return;
			}

			result = m_Device->CreateSwapchain(
				{
					.window = m_Window.Raw(),
				},
				m_Swapchain);
			if (result != RealRHI::Result::Success) {
				std::cerr << "Failed to create swapchain" << std::endl;
			}
		}

		void CreateGraphicsPipeline() {
			const RealRHI::ShaderDesc shaderDesc{
				.moduleName = "shader",

			};

			RealRHI::Ref<RealRHI::Shader> shader;
			RealRHI::Result result = m_Device->CreateShader(shaderDesc, shader);
			if (result != RealRHI::Result::Success) {
				std::cerr << "Failed to create shader" << std::endl;
				return;
			}

			RealRHI::PipelineDesc desc{
				.shader = shader.Raw(),

				.rasterState =
					{
						.cullMode = RealRHI::CullMode::Back,
						.fillMode = RealRHI::FillMode::Solid,
						.frontCounterClockwise = false,
					},
				.depthState =
					{
						.depthTestEnable = false,
						.depthWriteEnable = false,
						.compareOp = RealRHI::CompareOp::Less,
					},
				.blendState =
					{
						.enable = false,
					},
				.renderTargetFormats = {
					.colorFormats = {m_Swapchain->GetBackBufferFormat()},
					.depthFormat = RealRHI::TextureFormat::Unknown,
				},
			};

			result = m_Device->CreateGraphicsPipeline(desc, m_Pipeline);
			if (result != RealRHI::Result::Success) {
				std::cerr << "Failed to create pipeline" << std::endl;
			}
		}

		void CreateVertexBuffer() {
			RealRHI::Result result = m_Device->CreateBuffer(
				{
					.size = sizeof(vertices[0]) * vertices.size(),
					.usage = RealRHI::BufferUsage::Vertex,
					.memoryUsage = RealRHI::MemoryUsage::CPUToGPU,
					.initialData = vertices.data(),
				},
				m_VertexBuffer);
			if (result != RealRHI::Result::Success) {
				std::cerr << "Failed to create vertex buffer" << std::endl;
			}
		}

		bool RecordCommandBuffer(RealRHI::CommandList* cmd, const RealRHI::FrameContext& frame) {
			if (cmd->Begin() != RealRHI::Result::Success) {
				std::cerr << "Failed to begin command list" << std::endl;
				return false;
			}

			RealRHI::RenderingInfo renderingInfo{
				.colorAttachments = {{
					.target = frame.backBufferView,
					.loadOp = RealRHI::LoadOp::Clear,
					.storeOp = RealRHI::StoreOp::Store,
					.clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
				}},
				.renderArea = {.x = 0, .y = 0, .width = frame.width, .height = frame.height},
			};

			cmd->BeginRendering(renderingInfo);
			cmd->BindPipeline(m_Pipeline.Raw());

			RealRHI::Viewport viewport{
				.x = 0.0f,
				.y = 0.0f,
				.width = static_cast<float>(frame.width),
				.height = static_cast<float>(frame.height),
				.minDepth = 0.0f,
				.maxDepth = 1.0f,
			};
			cmd->SetViewport(viewport);

			RealRHI::Rect scissor{
				.x = 0,
				.y = 0,
				.width = frame.width,
				.height = frame.height,
			};
			cmd->SetScissor(scissor);

			cmd->BindVertexBuffer(m_VertexBuffer.Raw());
			cmd->Draw(static_cast<uint32_t>(vertices.size()));
			cmd->EndRendering();

			if (cmd->End() != RealRHI::Result::Success) {
				std::cerr << "Failed to end command list" << std::endl;
				return false;
			}
			return true;
		}

		bool DrawFrame(uint32_t& framesRendered) {
			if (m_Window->IsMinimized() || m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0) {
				SDL_Delay(16);
				return true;
			}

			RealRHI::FrameContext frame = m_Swapchain->BeginFrame();
			if (frame.backBufferView == nullptr) {
				if (m_Window->IsMinimized() || m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0) {
					SDL_Delay(16);
					return true;
				}
				std::cerr << "Frame acquisition returned no backbuffer; stopping to avoid unsafe semaphore reuse."
						  << std::endl;
				return false;
			}
			RealRHI::CommandList* cmd = nullptr;
			if (m_FrameResources.BeginSlot(frame.frameIndex, cmd) != RealRHI::Result::Success) {
				std::cerr << "Failed to recycle frame command list" << std::endl;
				return false;
			}
			if (!RecordCommandBuffer(cmd, frame)) {
				m_FrameResources.CancelSlot(frame.frameIndex);
				std::cerr << "Frame command recording failed." << std::endl;
				return false;
			}
			RealRHI::SubmissionToken token{};
			if (m_Device->Submit(cmd, m_Swapchain.Raw(), frame, &token) != RealRHI::Result::Success) {
				std::cerr << "Frame submission failed." << std::endl;
				return false;
			}
			if (m_FrameResources.TrackSubmission(frame.frameIndex, token) != RealRHI::Result::Success) {
				std::cerr << "Failed to track frame submission" << std::endl;
				return false;
			}
			m_Swapchain->Present(frame);
			++framesRendered;
			return true;
		}

	private:
		std::unique_ptr<RealRHI::Device> m_Device;
		RealRHI::Ref<RealRHI::Window> m_Window;
		RealRHI::Ref<RealRHI::Swapchain> m_Swapchain;
		RealRHI::Ref<RealRHI::Pipeline> m_Pipeline;
		RealRHI::Ref<RealRHI::Buffer> m_VertexBuffer;
		RealRHI::FrameResources m_FrameResources;
	};
}

int main(int argc, char** argv) {
	uint32_t frameLimit = 0;
	if (!RealRHI::Examples::ParseFrameLimit(argc, argv, frameLimit)) {
		return 1;
	}

	bool succeeded = false;
	{
		TriangleApplication application;
		succeeded = application.Init() && application.Run(frameLimit);
	}
	if (!succeeded || RealRHI::Examples::s_ErrorCount != 0) {
		return 1;
	}
	return 0;
}