#include "Device.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
	using namespace RealRHI;
	bool expectingError = false;
	uint32_t errors = 0;
	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}
	void Capture(const DebugMessage& message) {
		DefaultDebugCallback(message);
		if (message.severity == DebugSeverity::Error) {
			Expect(expectingError && message.type == DebugMessageType::General &&
					std::string_view(message.message).starts_with("CreateGraphicsPipeline:"),
				"Only expected pipeline validation errors");
			++errors;
		}
	}
	struct Position {
		uint32_t padding;
		float position[2];
		uint32_t trailing;
	};
	struct Color {
		uint32_t padding;
		std::array<uint8_t, 4> color;
	};
}

int main() {
	using namespace RealRHI;
	std::unique_ptr<Device> device;
	Expect(Device::Create({.shaderDirectory = REALRHI_TEST_SHADER_DIRECTORY,
							  .enableDebug = true,
							  .enableValidationLayers = true,
							  .debugCallback = Capture},
			   device) == Result::Success,
		"Create validation device");
	Ref<Shader> shader;
	Expect(device->CreateShader(
			   {.moduleName = "packed",
				   .entryPoints = {{"vertexMain", ShaderStage::Vertex}, {"fragmentMain", ShaderStage::Fragment}}},
			   shader) == Result::Success,
		"Compile packed input shader");
	PipelineDesc desc{
		.shader = shader.Raw(),
		.vertexLayout =
			{.bindings =
					{{.stride = sizeof(Position)},
						{.binding = 1, .stride = sizeof(Color), .inputRate = VertexInputRate::Instance}},
				.attributes =
					{{.format = VertexFormat::Float2, .offset = offsetof(Position, position)},
						{.location = 1,
							.binding = 1,
							.format = VertexFormat::UNorm8x4,
							.offset = offsetof(Color, color)}}},
		.rasterState = {.cullMode = CullMode::None},
		.depthState = {.depthTestEnable = false, .depthWriteEnable = false},
		.renderTargetFormats = {.colorFormats = {TextureFormat::RGBA8_UNorm}}
	};
	Ref<Pipeline> pipeline;
	Expect(device->CreateGraphicsPipeline(desc, pipeline) == Result::Success,
		"Create explicit padded and packed input pipeline");
	auto invalid = desc;
	Ref<Pipeline> rejected;
	expectingError = true;
	invalid.vertexLayout.attributes.pop_back();
	Expect(device->CreateGraphicsPipeline(invalid, rejected) == Result::InvalidParameter,
		"Reject missing shader location");
	invalid = desc;
	invalid.vertexLayout.attributes[1].format = VertexFormat::UInt;
	Expect(
		device->CreateGraphicsPipeline(invalid, rejected) == Result::InvalidParameter, "Reject numeric class mismatch");
	invalid = desc;
	invalid.vertexLayout.attributes[1].location = 0;
	Expect(device->CreateGraphicsPipeline(invalid, rejected) == Result::InvalidParameter, "Reject duplicate locations");
	invalid = desc;
	invalid.vertexLayout.bindings[1].binding = 0;
	Expect(device->CreateGraphicsPipeline(invalid, rejected) == Result::InvalidParameter, "Reject duplicate bindings");
	invalid = desc;
	invalid.vertexLayout.attributes[0].offset = sizeof(Position);
	Expect(device->CreateGraphicsPipeline(invalid, rejected) == Result::InvalidParameter,
		"Reject attribute outside stride");
	expectingError = false;
	Expect(errors == 5 && !rejected, "Invalid pipelines are not published");
	const Position positions[] = {{}, {0, {-0.9f, -0.9f}}, {0, {0.9f, -0.9f}}, {0, {0, 0.9f}}};
	const Color colors[] = {{}, {0, {51, 153, 204, 255}}};
	Ref<Buffer> vertices, instances, readback;
	Expect(device->CreateBuffer({.size = sizeof(positions),
									.usage = BufferUsage::Vertex,
									.memoryUsage = MemoryUsage::CPUToGPU,
									.initialData = positions},
			   vertices) == Result::Success,
		"Create padded positions");
	Expect(device->CreateBuffer({.size = sizeof(colors),
									.usage = BufferUsage::Vertex,
									.memoryUsage = MemoryUsage::CPUToGPU,
									.initialData = colors},
			   instances) == Result::Success,
		"Create packed instance colors");
	Expect(device->CreateBuffer(
			   {.size = 16 * 16 * 4, .usage = BufferUsage::TransferDst, .memoryUsage = MemoryUsage::GPUToCPU},
			   readback) == Result::Success,
		"Create readback");
	Ref<Texture> target;
	Expect(device->CreateTexture({.width = 16,
									 .height = 16,
									 .format = TextureFormat::RGBA8_UNorm,
									 .usage = TextureUsage::RenderTarget | TextureUsage::TransferSrc},
			   target) == Result::Success,
		"Create render target");
	Ref<CommandAllocator> allocator;
	Ref<CommandList> commands;
	Expect(device->CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success, "Create allocator");
	Expect(device->CreateCommandList(allocator.Raw(), commands) == Result::Success, "Create commands");
	Expect(commands->Begin() == Result::Success, "Begin commands");
	commands->Barrier(
		{.textures = {
			 {.texture = target.Raw(),
				 .after = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
				 .newLayout = TextureLayout::ColorAttachment}
		 }});
	commands->BeginRendering(
		{.colorAttachments = {{.target = target->GetTextureView()}}, .renderArea = {.width = 16, .height = 16}});
	commands->SetViewport({.width = 16, .height = 16});
	commands->SetScissor({.width = 16, .height = 16});
	commands->BindPipeline(pipeline.Raw());
	commands->BindVertexBuffer(vertices.Raw(), 0, sizeof(Position));
	commands->BindVertexBuffer(instances.Raw(), 1, sizeof(Color));
	commands->Draw(3);
	commands->EndRendering();
	commands->Barrier(
		{.textures = {
			 {.texture = target.Raw(),
				 .before = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
				 .after = {PipelineStage::Transfer, ResourceAccess::TransferRead},
				 .oldLayout = TextureLayout::ColorAttachment,
				 .newLayout = TextureLayout::TransferSrc}
		 }});
	commands->CopyTextureToBuffer(target.Raw(), readback.Raw(), {.width = 16, .height = 16});
	Expect(commands->End() == Result::Success, "End commands");
	const auto token = device->GetGraphicsCommandQueue()->Submit(*commands);
	Expect(token.IsValid(), "Submit padded and packed draw");
	device->GetGraphicsCommandQueue()->Wait(token);
	std::array<uint8_t, 16 * 16 * 4> pixels{};
	Expect(readback->ReadData(pixels.data(), pixels.size()) == Result::Success, "Read pixels");
	const size_t center = (8 * 16 + 8) * 4;
	Expect(pixels[center] == 51 && pixels[center + 1] == 153 && pixels[center + 2] == 204 && pixels[center + 3] == 255,
		"Explicit offsets, strides, multiple bindings, instance rate, and normalized packed format produce correct "
		"pixels");
	std::cout << "ShaderPipelineTests passed\n";
}
