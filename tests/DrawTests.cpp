#include "Device.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
	using namespace RealRHI;
	uint32_t s_RejectedDraws = 0;

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void CaptureDebugMessage(const DebugMessage& message) {
		DefaultDebugCallback(message);
		if (message.severity == DebugSeverity::Error) {
			Expect(message.type == DebugMessageType::General &&
					std::string_view(message.message).starts_with("DrawIndexed: Bind an index buffer"),
				"Only the intentional invalid indexed draws report errors");
			++s_RejectedDraws;
		}
	}

	struct Parameters {
		std::array<float, 4> color;
		std::array<float, 4> depth;
	};

	void Transition(CommandList& commands, Texture* texture, TextureLayout before, TextureLayout after) {
		BarrierDesc barriers;
		barriers.textures.push_back({
			.texture = texture,
			.before = {PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite},
			.after = {PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite},
			.oldLayout = before,
			.newLayout = after,
		});
		commands.Barrier(barriers);
	}

	void TestDraws(Device& device) {
		Ref<Shader> shader;
		Expect(device.CreateShader({.moduleName = "draw",
									   .entryPoints =
										   {{"vertexMain", RealRHI::ShaderStage::Vertex},
											   {"fragmentMain", RealRHI::ShaderStage::Fragment}}},
				   shader) == Result::Success,
			"Compile draw test shader");
		PipelineDesc pipelineDesc{
			.shader = shader.Raw(),

			.rasterState = {.cullMode = CullMode::None},
			.renderTargetFormats = {
				.colorFormats = {TextureFormat::RGBA8_UNorm},
				.depthFormat = TextureFormat::D32_Float,
			},
		};
		Ref<Pipeline> opaque;
		Ref<Pipeline> transparent;
		Expect(device.CreateGraphicsPipeline(pipelineDesc, opaque) == Result::Success,
			"Create depth-only graphics pipeline");
		pipelineDesc.depthState.depthWriteEnable = false;
		pipelineDesc.blendState.enable = true;
		Expect(
			device.CreateGraphicsPipeline(pipelineDesc, transparent) == Result::Success, "Create alpha blend pipeline");

		Ref<Shader> instanceShader;
		Ref<Pipeline> instances;
		Expect(device.CreateShader({.moduleName = "instances",
									   .entryPoints =
										   {{"vertexMain", RealRHI::ShaderStage::Vertex},
											   {"fragmentMain", RealRHI::ShaderStage::Fragment}}},
				   instanceShader) == Result::Success,
			"Compile procedural instance shader");
		pipelineDesc.shader = instanceShader.Raw();
		pipelineDesc.depthState.depthWriteEnable = true;
		pipelineDesc.blendState.enable = false;
		Expect(device.CreateGraphicsPipeline(pipelineDesc, instances) == Result::Success,
			"Create pipeline without vertex input bindings");

		// Prefix geometry and indices make a silently ignored firstIndex/vertexOffset draw the wrong pixels.
		const float positions[][2] = {
			{2, 2},
			{3, 2},
			{2, 3},
			{-0.9f, -0.8f},
			{-0.1f, -0.8f},
			{-0.5f, 0.8f},
			{0.1f, -0.8f},
			{0.9f, -0.8f},
			{0.5f, 0.8f},
		};
		const uint32_t indices[] = {0, 0, 0, 0, 1, 2};
		Ref<Buffer> vertices;
		Ref<Buffer> indexBuffer;
		Ref<Buffer> readback;
		Expect(device.CreateBuffer(
				   {
					   .size = sizeof(positions),
					   .usage = BufferUsage::Vertex,
					   .memoryUsage = MemoryUsage::CPUToGPU,
					   .initialData = positions,
				   },
				   vertices) == Result::Success,
			"Create offset vertex geometry");
		Expect(device.CreateBuffer(
				   {
					   .size = sizeof(indices),
					   .usage = BufferUsage::Index,
					   .memoryUsage = MemoryUsage::CPUToGPU,
					   .initialData = indices,
				   },
				   indexBuffer) == Result::Success,
			"Create offset indices");
		Expect(device.CreateBuffer(
				   {.size = 64 * 64 * 8, .usage = BufferUsage::TransferDst, .memoryUsage = MemoryUsage::GPUToCPU},
				   readback) == Result::Success,
			"Create draw readback");

		Ref<Texture> color;
		Ref<Texture> depth;
		Expect(device.CreateTexture(
				   {
					   .width = 64,
					   .height = 64,
					   .format = TextureFormat::RGBA8_UNorm,
					   .usage = TextureUsage::RenderTarget | TextureUsage::TransferSrc,
				   },
				   color) == Result::Success,
			"Create draw output");
		Expect(device.CreateTexture(
				   {.width = 64, .height = 64, .format = TextureFormat::D32_Float, .usage = TextureUsage::DepthStencil},
				   depth) == Result::Success,
			"Create D32 depth attachment");

		Ref<CommandAllocator> allocator;
		Ref<CommandList> commands;
		Expect(
			device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success, "Create draw allocator");
		Expect(device.CreateCommandList(allocator.Raw(), commands) == Result::Success, "Create draw command list");
		Expect(commands->Begin() == Result::Success, "Begin draw commands");
		Transition(*commands, color.Raw(), TextureLayout::Undefined, TextureLayout::ColorAttachment);
		Transition(*commands, depth.Raw(), TextureLayout::Undefined, TextureLayout::DepthStencilAttachment);
		commands->BeginRendering({
			.colorAttachments = {{.target = color->GetTextureView()}},
			.hasDepth = true,
			.depthAttachment = {.target = depth->GetTextureView()},
			.renderArea = {.width = 64, .height = 64},
		});
		commands->SetViewport({.width = 64, .height = 64});
		commands->SetScissor({.width = 64, .height = 64});
		commands->BindPipeline(opaque.Raw());
		commands->BindVertexBuffer(vertices.Raw());

		Parameters parameters{.color = {1, 0, 0, 1}, .depth = {0.3f}};
		commands->SetPushConstants(&parameters, sizeof(parameters));
		commands->DrawIndexed(3);
		Expect(s_RejectedDraws == 1, "Reject missing index buffer immediately");
		commands->BindIndexBuffer(indexBuffer.Raw());
		commands->DrawIndexed(3, 1, std::numeric_limits<uint32_t>::max());
		commands->DrawIndexed(4, 1, 3);
		Expect(s_RejectedDraws == 3, "Reject out-of-bounds indexed ranges without unsigned overflow");
		commands->DrawIndexed(3, 1, 3, 3, 4);

		parameters.color = {0, 0, 1, 1};
		parameters.depth[0] = 0.8f;
		commands->SetPushConstants(&parameters, sizeof(parameters));
		commands->DrawIndexed(3, 1, 3, 3);
		commands->Draw(3, 1, 6, 2);

		commands->BindPipeline(transparent.Raw());
		parameters.color = {0, 1, 0, 0.5f};
		parameters.depth[0] = 0.2f;
		commands->SetPushConstants(&parameters, sizeof(parameters));
		commands->DrawIndexed(3, 1, 3, 3);
		commands->EndRendering();

		Transition(*commands, color.Raw(), TextureLayout::ColorAttachment, TextureLayout::TransferSrc);
		commands->CopyTextureToBuffer(color.Raw(), readback.Raw(), {.width = 64, .height = 64});

		Transition(*commands, color.Raw(), TextureLayout::TransferSrc, TextureLayout::ColorAttachment);
		Transition(
			*commands, depth.Raw(), TextureLayout::DepthStencilAttachment, TextureLayout::DepthStencilAttachment);
		commands->BeginRendering({
			.colorAttachments = {{.target = color->GetTextureView()}},
			.hasDepth = true,
			.depthAttachment = {.target = depth->GetTextureView()},
			.renderArea = {.width = 64, .height = 64},
		});
		commands->BindPipeline(instances.Raw());
		parameters.color = {1, 1, 1, 1};
		parameters.depth[0] = 0.3f;
		commands->SetPushConstants(&parameters, sizeof(parameters));
		commands->DrawIndexed(3, 2, 3, 5, 7);
		commands->EndRendering();
		Transition(*commands, color.Raw(), TextureLayout::ColorAttachment, TextureLayout::TransferSrc);
		commands->CopyTextureToBuffer(
			color.Raw(), readback.Raw(), {.width = 64, .height = 64, .bufferOffset = 64 * 64 * 4});
		Expect(commands->End() == Result::Success, "End commands after rejected and valid draws");
		const SubmissionToken completion = device.GetGraphicsCommandQueue()->Submit(*commands);
		Expect(completion.IsValid(), "Rejected draws do not poison subsequent valid recording");
		device.GetGraphicsCommandQueue()->Wait(completion);

		std::array<uint8_t, 64 * 64 * 8> pixels{};
		Expect(readback->ReadData(pixels.data(), pixels.size()) == Result::Success, "Read draw output");
		const size_t left = (24 * 64 + 16) * 4;
		Expect(std::abs(pixels[left] - 128) <= 1 && std::abs(pixels[left + 1] - 128) <= 1 && pixels[left + 2] == 0,
			"Depth rejects the farther blue triangle and alpha blends green over the nearer red triangle");
		const size_t right = (24 * 64 + 48) * 4;
		for (size_t offset : {left, right}) {
			const size_t instancePixel = 64 * 64 * 4 + offset;
			Expect(pixels[instancePixel] == 255 && pixels[instancePixel + 1] == 255 && pixels[instancePixel + 2] == 255,
				"Indexed instancing applies instanceCount, firstIndex, vertexOffset, and firstInstance");
		}
		Expect(pixels[right] == 0 && pixels[right + 1] == 0 && pixels[right + 2] == 255,
			"Non-indexed firstVertex selects the right triangle");
		Expect(pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 0, "Pixels outside geometry retain the clear color");
	}
}

int main() {
	std::unique_ptr<RealRHI::Device> device;
	Expect(RealRHI::Device::Create(
			   {
				   .applicationName = "DrawTests",
				   .shaderDirectory = REALRHI_TEST_SHADER_DIRECTORY,
				   .enableDebug = true,
				   .enableProfiling = false,
				   .enableValidationLayers = true,
				   .debugCallback = CaptureDebugMessage,
			   },
			   device) == RealRHI::Result::Success,
		"Create validated draw test device");
	TestDraws(*device);
	device->WaitIdle();
	device.reset();
	Expect(s_RejectedDraws == 3, "Only the three invalid draws were rejected");
	std::cout << "DrawTests passed\n";
}
