#include "Device.h"
#include "Vulkan/VulkanDevice.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
	using namespace RealRHI;
	bool s_ExpectError = false;
	uint32_t s_Errors = 0;

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void Capture(const DebugMessage& message) {
		DefaultDebugCallback(message);
		if (message.severity == DebugSeverity::Error) {
			Expect(s_ExpectError && message.type == DebugMessageType::General &&
					std::string_view(message.message).starts_with("CreateGraphicsPipeline:"),
				"Only intentional pipeline errors");
			++s_Errors;
		}
	}

	struct Parameters {
		std::array<float, 4> color;
		std::array<float, 4> depthAndSlope{0.5f};
	};

	struct Draw {
		Pipeline* pipeline;
		Parameters parameters;
		uint32_t vertexCount = 4;
		Buffer* indices = nullptr;
	};

	class RenderTest {
	public:
		explicit RenderTest(Device& device)
			: m_Device(device) {
			Expect(device.CreateShader({.moduleName = "pipeline_state"}, m_Shader) == Result::Success,
				"Compile pipeline state shader");

			for (auto& target : m_Targets) {
				Expect(device.CreateTexture({.width = 16,
												.height = 16,
												.format = TextureFormat::RGBA8_UNorm,
												.usage = TextureUsage::RenderTarget | TextureUsage::TransferSrc},
						   target) == Result::Success,
					"Create color target");
			}

			Expect(device.CreateTexture({.width = 16,
											.height = 16,
											.format = TextureFormat::D32_Float,
											.usage = TextureUsage::DepthStencil},
					   m_Depth) == Result::Success,
				"Create depth target");
			Expect(
				device.CreateBuffer(
					{.size = 2 * 16 * 16 * 4, .usage = BufferUsage::TransferDst, .memoryUsage = MemoryUsage::GPUToCPU},
					m_Readback) == Result::Success,
				"Create pixel readback");
		}

		PipelineDesc Description() {
			return {
				.shader = m_Shader.Raw(),
				.rasterState = {.cullMode = CullMode::None},
				.depthState = {.depthTestEnable = false, .depthWriteEnable = false},
				.topology = PrimitiveTopology::TriangleStrip,
				.renderTargetFormats = {
					.colorFormats = {TextureFormat::RGBA8_UNorm, TextureFormat::RGBA8_UNorm},
					.depthFormat = TextureFormat::D32_Float
				}
			};
		}

		Ref<Pipeline> Create(const PipelineDesc& desc) const {
			Ref<Pipeline> pipeline;
			Expect(m_Device.CreateGraphicsPipeline(desc, pipeline) == Result::Success, "Create pipeline state variant");
			return pipeline;
		}

		void Reject(const PipelineDesc& desc) const {
			Ref<Pipeline> pipeline;
			const uint32_t errorsBefore = s_Errors;
			s_ExpectError = true;
			Expect(m_Device.CreateGraphicsPipeline(desc, pipeline) == Result::InvalidParameter && !pipeline,
				"Reject invalid pipeline before Vulkan creation");
			s_ExpectError = false;
			Expect(s_Errors == errorsBefore + 1, "Invalid state has one actionable diagnostic");
		}

		void Render(std::initializer_list<Draw> draws, ClearColorValue clear = {0.2f, 0.4f, 0.6f, 0.8f}) {
			Ref<CommandAllocator> allocator;
			Ref<CommandList> commands;
			Expect(
				m_Device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success, "Create allocator");
			Expect(m_Device.CreateCommandList(allocator.Raw(), commands) == Result::Success, "Create commands");
			Expect(commands->Begin() == Result::Success, "Begin commands");

			BarrierDesc barriers;
			for (auto& target : m_Targets) {
				TextureLayout oldLayout = TextureLayout::Undefined;
				if (m_Initialized) {
					oldLayout = TextureLayout::TransferSrc;
				}
				barriers.textures.push_back(
					{.texture = target.Raw(),
						.before =
							{PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite},
						.after = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
						.oldLayout = oldLayout,
						.newLayout = TextureLayout::ColorAttachment});
			}
			TextureLayout oldDepth = TextureLayout::Undefined;
			if (m_Initialized) {
				oldDepth = TextureLayout::DepthStencilAttachment;
			}
			barriers.textures.push_back(
				{.texture = m_Depth.Raw(),
					.before = {PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite},
					.after =
						{PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests,
							ResourceAccess::DepthStencilRead | ResourceAccess::DepthStencilWrite},
					.oldLayout = oldDepth,
					.newLayout = TextureLayout::DepthStencilAttachment});
			commands->Barrier(barriers);

			commands->BeginRendering(
				{.colorAttachments =
						{{.target = m_Targets[0]->GetTextureView(), .clearColor = clear},
							{.target = m_Targets[1]->GetTextureView(), .clearColor = clear}},
					.hasDepth = true,
					.depthAttachment = {.target = m_Depth->GetTextureView()},
					.renderArea = {.width = 16, .height = 16}});
			commands->SetViewport({.width = 16, .height = 16});
			commands->SetScissor({.width = 16, .height = 16});

			for (const auto& draw : draws) {
				commands->BindPipeline(draw.pipeline);
				commands->SetPushConstants(&draw.parameters, sizeof(draw.parameters));
				if (draw.indices) {
					commands->BindIndexBuffer(draw.indices);
					commands->DrawIndexed(draw.vertexCount);
				}
				else {
					commands->Draw(draw.vertexCount);
				}
			}
			commands->EndRendering();

			barriers.textures.clear();
			for (auto& target : m_Targets) {
				barriers.textures.push_back(
					{.texture = target.Raw(),
						.before = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
						.after = {PipelineStage::Transfer, ResourceAccess::TransferRead},
						.oldLayout = TextureLayout::ColorAttachment,
						.newLayout = TextureLayout::TransferSrc});
			}
			commands->Barrier(barriers);
			for (uint32_t i = 0; i < 2; ++i) {
				commands->CopyTextureToBuffer(
					m_Targets[i].Raw(), m_Readback.Raw(), {.width = 16, .height = 16, .bufferOffset = i * 16 * 16 * 4});
			}

			Expect(commands->End() == Result::Success, "End commands");
			const auto completion = m_Device.GetGraphicsCommandQueue()->Submit(*commands);
			Expect(completion.IsValid(), "Submit state test draw");
			m_Device.GetGraphicsCommandQueue()->Wait(completion);
			Expect(m_Readback->ReadData(m_Pixels.data(), m_Pixels.size()) == Result::Success, "Read state test pixels");
			m_Initialized = true;
		}

		void Pixel(uint32_t target, uint32_t x, uint32_t y, std::array<float, 4> expected, const char* message) const {
			const size_t offset = (target * 16 * 16 + y * 16 + x) * 4;
			for (uint32_t channel = 0; channel < 4; ++channel) {
				const int value = static_cast<int>(std::round(expected[channel] * 255.f));
				if (std::abs(static_cast<int>(m_Pixels[offset + channel]) - value) > 1) {
					std::cerr << "Channel " << channel << ": expected " << value << ", got "
							  << static_cast<int>(m_Pixels[offset + channel]) << '\n';
					Expect(false, message);
				}
			}
		}

	private:
		Device& m_Device;
		Ref<Shader> m_Shader;
		std::array<Ref<Texture>, 2> m_Targets;
		Ref<Texture> m_Depth;
		Ref<Buffer> m_Readback;
		std::array<uint8_t, 2 * 16 * 16 * 4> m_Pixels{};
		bool m_Initialized = false;
	};

	void TestBlending(RenderTest& test, VulkanDevice& device) {
		auto desc = test.Description();
		auto pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), {.color = {0.3f, 0.2f, 0.1f, 0.5f}}}});
		test.Pixel(0, 8, 8, {0.3f, 0.2f, 0.1f, 0.5f}, "Disabled blending replaces destination");

		desc.blendState.enable = true;
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), {.color = {0.3f, 0.2f, 0.1f, 0.5f}}}});
		test.Pixel(0, 8, 8, {0.25f, 0.3f, 0.35f, 0.5f}, "Default factors preserve existing alpha blending");

		desc.blendState.sourceColorFactor = BlendFactor::One;
		desc.blendState.destinationColorFactor = BlendFactor::One;
		desc.blendState.destinationAlphaFactor = BlendFactor::One;
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), {.color = {0.3f, 0.2f, 0.1f, 0.5f}}}});
		test.Pixel(0, 8, 8, {0.5f, 0.6f, 0.7f, 1.f}, "Additive color and alpha blending");

		desc.blendState.colorOperation = BlendOperation::Subtract;
		desc.blendState.alphaOperation = BlendOperation::ReverseSubtract;
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), {.color = {0.7f, 0.6f, 0.5f, 0.4f}}}});
		test.Pixel(0, 8, 8, {0.5f, 0.2f, 0.f, 0.4f}, "Separate color subtraction and reverse alpha subtraction");

		desc.blendState.colorOperation = BlendOperation::ReverseSubtract;
		desc.blendState.alphaOperation = BlendOperation::Subtract;
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), {.color = {0.7f, 0.6f, 0.5f, 0.4f}}}});
		test.Pixel(0, 8, 8, {0.f, 0.f, 0.1f, 0.f}, "Reverse color subtraction and alpha subtraction");

		for (auto operation : {BlendOperation::Min, BlendOperation::Max}) {
			desc.blendState.colorOperation = operation;
			desc.blendState.alphaOperation = operation;
			pipeline = test.Create(desc);
			test.Render({{pipeline.Raw(), {.color = {0.7f, 0.2f, 0.5f, 0.9f}}}});
			if (operation == BlendOperation::Min) {
				test.Pixel(0, 8, 8, {0.2f, 0.2f, 0.5f, 0.8f}, "Minimum blend operation");
			}
			else {
				test.Pixel(0, 8, 8, {0.7f, 0.4f, 0.6f, 0.9f}, "Maximum blend operation");
			}
		}

		desc.blendState = {
			.enable = true,
			.sourceColorFactor = BlendFactor::ConstantColor,
			.destinationColorFactor = BlendFactor::OneMinusConstantColor,
			.sourceAlphaFactor = BlendFactor::ConstantAlpha,
			.destinationAlphaFactor = BlendFactor::OneMinusConstantAlpha
		};
		desc.blendConstants = {0.5f, 0.25f, 0.75f, 0.4f};
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), {.color = {0.8f, 0.6f, 0.4f, 0.2f}}}});
		test.Pixel(0, 8, 8, {0.5f, 0.45f, 0.45f, 0.56f}, "Constant color and alpha blend factors");

		desc.blendState = {
			.enable = true,
			.sourceColorFactor = BlendFactor::SourceColor,
			.destinationColorFactor = BlendFactor::DestinationColor,
			.sourceAlphaFactor = BlendFactor::SourceAlpha,
			.destinationAlphaFactor = BlendFactor::DestinationAlpha
		};
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), {.color = {0.3f, 0.2f, 0.1f, 0.5f}}}});
		test.Pixel(0, 8, 8, {0.13f, 0.2f, 0.37f, 0.89f}, "Source and destination color and alpha factors");

		desc.blendState = {
			.enable = true,
			.sourceColorFactor = BlendFactor::OneMinusSourceColor,
			.destinationColorFactor = BlendFactor::OneMinusDestinationColor,
			.sourceAlphaFactor = BlendFactor::OneMinusSourceAlpha,
			.destinationAlphaFactor = BlendFactor::OneMinusDestinationAlpha
		};
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), {.color = {0.3f, 0.2f, 0.1f, 0.5f}}}});
		test.Pixel(0, 8, 8, {0.37f, 0.4f, 0.33f, 0.41f}, "Complementary source and destination factors");

		desc.blendState = {
			.enable = true,
			.sourceColorFactor = BlendFactor::SourceAlphaSaturate,
			.destinationColorFactor = BlendFactor::Zero
		};
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), {.color = {0.3f, 0.2f, 0.1f, 0.5f}}}});
		test.Pixel(0, 8, 8, {0.06f, 0.04f, 0.02f, 0.5f}, "Saturated source alpha factor");

		desc.blendState = {
			.enable = true,
			.sourceColorFactor = BlendFactor::DestinationAlpha,
			.destinationColorFactor = BlendFactor::OneMinusDestinationAlpha
		};
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), {.color = {0.3f, 0.2f, 0.1f, 0.5f}}}});
		test.Pixel(0, 8, 8, {0.28f, 0.24f, 0.2f, 0.5f}, "Destination alpha color factors");

		desc.blendState = {.writeMask = ColorWriteMask::None};
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), {.color = {1, 1, 1, 1}}}});
		test.Pixel(0, 8, 8, {0.2f, 0.4f, 0.6f, 0.8f}, "Empty write mask preserves target");

		desc.colorBlendStates = {
			{.writeMask = ColorWriteMask::Red | ColorWriteMask::Alpha},
			{.enable = true,
				.sourceColorFactor = BlendFactor::One,
				.destinationColorFactor = BlendFactor::One,
				.writeMask = ColorWriteMask::Green | ColorWriteMask::Blue}
		};
		if (device.SupportsIndependentBlend()) {
			pipeline = test.Create(desc);
			test.Render({{pipeline.Raw(), {.color = {0.8f, 0.2f, 0.1f, 0.2f}}}});
			test.Pixel(0, 8, 8, {0.8f, 0.4f, 0.6f, 0.2f}, "First target has its own color mask and disabled blend");
			test.Pixel(1, 8, 8, {0.2f, 0.6f, 0.7f, 0.8f}, "Second target has independent mask and additive blend");
		}
		else {
			test.Reject(desc);
		}
	}

	void TestTopology(RenderTest& test, Device& device) {
		auto desc = test.Description();
		const Parameters white{.color = {1, 1, 1, 1}};
		const ClearColorValue black{0, 0, 0, 0};
		auto pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), white}}, black);
		test.Pixel(0, 12, 12, {1, 1, 1, 1}, "Triangle strip assembles the second triangle");

		desc.topology = PrimitiveTopology::TriangleList;
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), white, 3}}, black);
		test.Pixel(0, 12, 12, {0, 0, 0, 0}, "Triangle list assembles only the first triangle");
		test.Pixel(0, 4, 4, {1, 1, 1, 1}, "Triangle list renders its first triangle");

		desc.topology = PrimitiveTopology::LineList;
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), white, 2}}, black);
		test.Pixel(0, 8, 1, {1, 1, 1, 1}, "Line list renders a horizontal edge");
		test.Pixel(0, 8, 8, {0, 0, 0, 0}, "Line list does not fill triangles");

		desc.topology = PrimitiveTopology::LineStrip;
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), white}}, black);
		test.Pixel(0, 7, 8, {1, 1, 1, 1}, "Line strip connects consecutive vertices");

		desc.topology = PrimitiveTopology::PointList;
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), white}}, black);
		test.Pixel(0, 1, 1, {1, 1, 1, 1}, "Point list renders one-pixel points");
		test.Pixel(0, 8, 8, {0, 0, 0, 0}, "Points do not fill triangles");

		// Split strips must discard the restart index before fetching vertex input.
		const uint32_t indices[] = {0, 1, 2, UINT32_MAX, 1, 3, 2};
		Ref<Buffer> indexBuffer;
		Expect(device.CreateBuffer({.size = sizeof(indices),
									   .usage = BufferUsage::Index,
									   .memoryUsage = MemoryUsage::CPUToGPU,
									   .initialData = indices},
				   indexBuffer) == Result::Success,
			"Create restart indices");
		desc.topology = PrimitiveTopology::TriangleStrip;
		desc.primitiveRestartEnable = true;
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), white, 7, indexBuffer.Raw()}}, black);
		test.Pixel(0, 4, 4, {1, 1, 1, 1}, "Restarted strip renders its first segment");
		test.Pixel(0, 12, 12, {1, 1, 1, 1}, "Restarted strip renders its second segment");

		desc.topology = PrimitiveTopology::TriangleFan;
		desc.primitiveRestartEnable = false;
		pipeline = test.Create(desc);
		test.Render({{pipeline.Raw(), white, 3}}, black);
		test.Pixel(0, 4, 4, {1, 1, 1, 1}, "Triangle fan renders its first triangle");
	}

	void TestDepthBias(RenderTest& test, VulkanDevice& device) {
		auto desc = test.Description();
		desc.depthState = {};
		auto normal = test.Create(desc);
		const Parameters green{.color = {0, 1, 0, 1}};
		const Parameters red{.color = {1, 0, 0, 1}};
		test.Render({{normal.Raw(), green}, {normal.Raw(), red}});
		test.Pixel(0, 8, 8, {0, 1, 0, 1}, "Equal depth fails the default Less comparison");

		desc.rasterState.depthBias = {.enable = true, .constantFactor = -10000.f};
		auto biased = test.Create(desc);
		test.Render({{normal.Raw(), green}, {biased.Raw(), red}});
		test.Pixel(0, 8, 8, {1, 0, 0, 1}, "Constant depth bias moves the second surface forward");

		desc.rasterState.depthBias.enable = false;
		biased = test.Create(desc);
		test.Render({{normal.Raw(), green}, {biased.Raw(), red}});
		test.Pixel(0, 8, 8, {0, 1, 0, 1}, "Disabled depth bias ignores nonzero factors");

		const Parameters reference{.color = {0, 1, 0, 1}, .depthAndSlope = {0.49f}};
		const Parameters sloped{.color = {1, 0, 0, 1}, .depthAndSlope = {0.5f, 0.2f}};
		desc.rasterState.depthBias = {.enable = true, .slopeFactor = -1.f};
		biased = test.Create(desc);
		test.Render({{normal.Raw(), reference}, {biased.Raw(), sloped}});
		test.Pixel(0, 8, 8, {1, 0, 0, 1}, "Slope-scaled depth bias moves the sloped surface forward");

		desc.rasterState.depthBias.clamp = -0.001f;
		if (device.SupportsDepthBiasClamp()) {
			biased = test.Create(desc);
			test.Render({{normal.Raw(), reference}, {biased.Raw(), sloped}});
			test.Pixel(0, 8, 8, {0, 1, 0, 1}, "Depth bias clamp limits the slope contribution");
		}
		else {
			test.Reject(desc);
		}
	}

	void TestInvalidStates(RenderTest& test) {
		auto desc = test.Description();
		desc.topology = static_cast<PrimitiveTopology>(999);
		test.Reject(desc);
		desc = test.Description();
		desc.topology = PrimitiveTopology::TriangleList;
		desc.primitiveRestartEnable = true;
		test.Reject(desc);
		desc = test.Description();
		desc.colorBlendStates = {{}};
		test.Reject(desc);
		desc = test.Description();
		desc.blendState.sourceColorFactor = static_cast<BlendFactor>(999);
		test.Reject(desc);
		desc = test.Description();
		desc.blendState.colorOperation = static_cast<BlendOperation>(999);
		test.Reject(desc);
		desc = test.Description();
		desc.blendState.writeMask = static_cast<ColorWriteMask>(16);
		test.Reject(desc);
		desc = test.Description();
		desc.blendConstants[0] = std::numeric_limits<float>::quiet_NaN();
		test.Reject(desc);
		desc = test.Description();
		desc.rasterState.depthBias.slopeFactor = std::numeric_limits<float>::infinity();
		test.Reject(desc);
	}
}

int main() {
	std::unique_ptr<RealRHI::Device> device;
	Expect(RealRHI::Device::Create({.applicationName = "PipelineStateTests",
									   .shaderDirectory = REALRHI_TEST_SHADER_DIRECTORY,
									   .enableDebug = true,
									   .enableValidationLayers = true,
									   .debugCallback = Capture},
			   device) == RealRHI::Result::Success,
		"Create validation-enabled pipeline test device");
	{
		RenderTest test(*device);
		auto& vulkanDevice = *static_cast<RealRHI::VulkanDevice*>(device.get());
		TestBlending(test, vulkanDevice);
		TestTopology(test, *device);
		TestDepthBias(test, vulkanDevice);
		TestInvalidStates(test);
	}
	device->WaitIdle();
	device.reset();
	std::cout << "PipelineStateTests passed\n";
}
