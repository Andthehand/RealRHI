#include "../ExampleSupport.h"
#include "Device.h"
#include "FrameContext.h"
#include "FrameResources.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace {
	constexpr uint32_t s_WindowWidth = 800;
	constexpr uint32_t s_WindowHeight = 600;
	constexpr uint32_t s_TextureWidth = 256;
	constexpr uint32_t s_TextureHeight = 256;
	constexpr uint32_t s_TextureCount = 2;
	constexpr uint32_t s_TextureMipCount = 9;

	struct Vertex {
		float pos[2];
		float uv[2];
		uint32_t texIdx;
		uint32_t samplerIdx;
	};

	std::vector<Vertex> BuildQuadVertices(float minX, float maxX, uint32_t textureIndex, uint32_t samplerIndex) {
		return {
			{{minX, -0.75f}, {0.0f, 1.0f}, textureIndex, samplerIndex},
			{{maxX, -0.75f}, {1.0f, 1.0f}, textureIndex, samplerIndex},
			{{maxX, 0.75f}, {1.0f, 0.0f}, textureIndex, samplerIndex},
			{{minX, -0.75f}, {0.0f, 1.0f}, textureIndex, samplerIndex},
			{{maxX, 0.75f}, {1.0f, 0.0f}, textureIndex, samplerIndex},
			{{minX, 0.75f}, {0.0f, 0.0f}, textureIndex, samplerIndex},
		};
	}

	std::vector<uint8_t> BuildCheckerboardTexture() {
		std::vector<uint8_t> pixels(s_TextureWidth * s_TextureHeight * 4);

		for (uint32_t y = 0; y < s_TextureHeight; ++y) {
			for (uint32_t x = 0; x < s_TextureWidth; ++x) {
				const bool checker = ((x / 32) + (y / 32)) % 2 == 0;
				uint8_t red = 40;
				uint8_t green = 180;
				uint8_t blue = 255;
				if (checker) {
					red = 255;
					green = static_cast<uint8_t>((x * 255) / (s_TextureWidth - 1));
					blue = static_cast<uint8_t>((y * 255) / (s_TextureHeight - 1));
				}

				const size_t pixelIndex = static_cast<size_t>((y * s_TextureWidth + x) * 4);
				pixels[pixelIndex + 0] = red;
				pixels[pixelIndex + 1] = green;
				pixels[pixelIndex + 2] = blue;
				pixels[pixelIndex + 3] = 255;
			}
		}

		return pixels;
	}

	std::vector<uint8_t> BuildStripeTexture() {
		std::vector<uint8_t> pixels(s_TextureWidth * s_TextureHeight * 4);

		for (uint32_t y = 0; y < s_TextureHeight; ++y) {
			for (uint32_t x = 0; x < s_TextureWidth; ++x) {
				const bool stripe = ((x / 16) % 2) == 0;
				uint8_t red = 250;
				uint8_t green = 100;
				uint8_t blue = static_cast<uint8_t>((x * 255) / (s_TextureWidth - 1));
				if (stripe) {
					red = 40;
					green = static_cast<uint8_t>((y * 255) / (s_TextureHeight - 1));
					blue = 255;
				}

				const size_t pixelIndex = static_cast<size_t>((y * s_TextureWidth + x) * 4);
				pixels[pixelIndex + 0] = red;
				pixels[pixelIndex + 1] = green;
				pixels[pixelIndex + 2] = blue;
				pixels[pixelIndex + 3] = 255;
			}
		}

		return pixels;
	}

	class TextureApplication {
	public:
		~TextureApplication() {
			if (m_Device) {
				m_Device->WaitIdle();
			}
		}

		bool Init() {
			const RealRHI::DeviceDesc desc{
				.applicationName = "RealRHI Texture Example",
				.shaderDirectory = "shaders",
				.enableDebug = true,
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
			if (!CreateTextures() || RealRHI::Examples::s_ErrorCount != 0) {
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
				{.Title = "RealRHI Texture Example", .Width = s_WindowWidth, .Height = s_WindowHeight}, m_Window);
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
				.moduleName = "texture",

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
			if (m_Textures.size() != s_TextureCount) {
				std::cerr << "Failed to create vertex buffer: textures not created" << std::endl;
				return;
			}

			m_Vertices.clear();
			m_Vertices.reserve(s_TextureCount * 6);

			struct QuadDesc {
				float minX;
				float maxX;
			};

			const QuadDesc quadDescs[s_TextureCount] = {
				{-0.95f, -0.05f},
				{0.05f, 0.95f},
			};

			for (uint32_t i = 0; i < s_TextureCount; ++i) {
				if (m_Textures[i].Raw() == nullptr) {
					std::cerr << "Failed to create vertex buffer: texture not created" << std::endl;
					return;
				}

				const uint32_t textureIndex = m_TextureViews[i]->GetSampledImageDescriptorIndex();
				const uint32_t samplerIndex = m_Samplers[i]->GetDescriptorIndex();
				if (textureIndex == std::numeric_limits<uint32_t>::max() ||
					samplerIndex == std::numeric_limits<uint32_t>::max()) {
					std::cerr << "Failed to create vertex buffer: invalid texture or sampler descriptor index"
							  << std::endl;
					return;
				}

				std::vector<Vertex> quadVertices =
					BuildQuadVertices(quadDescs[i].minX, quadDescs[i].maxX, textureIndex, samplerIndex);
				m_Vertices.insert(m_Vertices.end(), quadVertices.begin(), quadVertices.end());
			}

			RealRHI::Result result = m_Device->CreateBuffer(
				{
					.size = sizeof(Vertex) * m_Vertices.size(),
					.usage = RealRHI::BufferUsage::Vertex,
					.memoryUsage = RealRHI::MemoryUsage::CPUToGPU,
					.initialData = m_Vertices.data(),
				},
				m_VertexBuffer);
			if (result != RealRHI::Result::Success) {
				std::cerr << "Failed to create vertex buffer" << std::endl;
			}
		}

		bool CreateTextures() {
			m_Textures.resize(s_TextureCount);
			m_TextureViews.resize(s_TextureCount);
			m_Samplers.resize(s_TextureCount);
			std::unique_ptr<RealRHI::UploadContext> uploads;
			if (m_Device->CreateUploadContext({}, uploads) != RealRHI::Result::Success ||
				uploads->BeginBatch() != RealRHI::Result::Success) {
				return false;
			}

			const std::vector<uint8_t> texturePixels[s_TextureCount] = {
				BuildCheckerboardTexture(),
				BuildStripeTexture(),
			};

			for (uint32_t i = 0; i < s_TextureCount; ++i) {
				RealRHI::Result result = m_Device->CreateTexture(
					{
						.width = s_TextureWidth,
						.height = s_TextureHeight,
						.depth = 1,
						.mipLevels = s_TextureMipCount,
						.arrayLayers = 1,
						.format = RealRHI::TextureFormat::RGBA8_UNorm,
						.usage = RealRHI::TextureUsage::ShaderResource | RealRHI::TextureUsage::TransferSrc |
							RealRHI::TextureUsage::TransferDst,
					},
					m_Textures[i]);
				if (result != RealRHI::Result::Success) {
					std::cerr << "Failed to create texture" << std::endl;
					return false;
				}

				result = uploads->UploadTexture(
					{
						.destination = m_Textures[i].Raw(),
						.finalLayout = RealRHI::TextureLayout::TransferDst,
						.finalState = {RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferWrite},
					},
					texturePixels[i].data(),
					texturePixels[i].size());
				if (result != RealRHI::Result::Success) {
					std::cerr << "Failed to upload texture data" << std::endl;
					return false;
				}

				RealRHI::TextureViewDesc viewDesc{
					.mipLevelCount = s_TextureMipCount,
				};
				RealRHI::SamplerDesc samplerDesc;
				if (i == 1) {
					viewDesc.baseMipLevel = 2;
					viewDesc.mipLevelCount -= 2;
					samplerDesc.minFilter = RealRHI::Filter::Nearest;
					samplerDesc.magFilter = RealRHI::Filter::Nearest;
					samplerDesc.mipFilter = RealRHI::Filter::Nearest;
				}

				if (m_Device->CreateTextureView(m_Textures[i].Raw(), viewDesc, m_TextureViews[i]) !=
						RealRHI::Result::Success ||
					m_Device->CreateSampler(samplerDesc, m_Samplers[i]) != RealRHI::Result::Success) {
					return false;
				}
			}

			return uploads->Submit(m_PendingTextureUpload) == RealRHI::Result::Success;
		}

		bool RecordCommandBuffer(RealRHI::CommandList* cmd, const RealRHI::FrameContext& frame) {
			if (cmd->Begin() != RealRHI::Result::Success) {
				std::cerr << "Failed to begin command list" << std::endl;
				return false;
			}
			if (m_PendingTextureUpload.completion.IsValid()) {
				cmd->Barrier(m_PendingTextureUpload.acquireBarriers);

				for (auto& texture : m_Textures) {
					cmd->GenerateMipmaps(texture.Raw(),
						{
							.finalState = {RealRHI::PipelineStage::FragmentShader, RealRHI::ResourceAccess::ShaderRead},
						});
				}
			}

			RealRHI::RenderingInfo renderingInfo{
				.colorAttachments = {{
					.target = frame.backBufferView,
					.loadOp = RealRHI::LoadOp::Clear,
					.storeOp = RealRHI::StoreOp::Store,
					.clearColor = {0.08f, 0.10f, 0.14f, 1.0f},
				}},
				.renderArea = {.x = 0, .y = 0, .width = frame.width, .height = frame.height},
			};

			cmd->BeginRendering(renderingInfo);
			cmd->BindPipeline(m_Pipeline.Raw());
			for (uint32_t i = 0; i < s_TextureCount; ++i) {
				cmd->UseResource(m_TextureViews[i].Raw());
				cmd->UseResource(m_Samplers[i].Raw());
			}

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
			cmd->Draw(static_cast<uint32_t>(m_Vertices.size()));
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
			std::span<const RealRHI::SubmissionToken> waits;
			if (m_PendingTextureUpload.completion.IsValid()) {
				waits = {&m_PendingTextureUpload.completion, 1};
			}
			if (m_Device->Submit(cmd, m_Swapchain.Raw(), frame, &token, waits) != RealRHI::Result::Success) {
				std::cerr << "Frame submission failed." << std::endl;
				return false;
			}
			m_PendingTextureUpload = {};
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
		std::vector<Vertex> m_Vertices;
		std::vector<RealRHI::Ref<RealRHI::Texture>> m_Textures;
		std::vector<RealRHI::Ref<RealRHI::TextureView>> m_TextureViews;
		std::vector<RealRHI::Ref<RealRHI::Sampler>> m_Samplers;
		RealRHI::UploadSubmission m_PendingTextureUpload;
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
		TextureApplication application;
		succeeded = application.Init() && application.Run(frameLimit);
	}
	if (!succeeded || RealRHI::Examples::s_ErrorCount != 0) {
		return 1;
	}
	return 0;
}