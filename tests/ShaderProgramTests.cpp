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
			Expect(
				expectingError && message.type == DebugMessageType::General, "Only expected command validation errors");
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
	VertexLayout layout{
		.bindings =
			{{.stride = sizeof(Position)},
				{.binding = 1, .stride = sizeof(Color), .inputRate = VertexInputRate::Instance}},
		.attributes = {
			{.format = VertexFormat::Float2, .offset = offsetof(Position, position)},
			{.location = 1, .binding = 1, .format = VertexFormat::UNorm8x4, .offset = offsetof(Color, color)}
		}
	};
	const GraphicsState state{
		.raster = {.cullMode = CullMode::None}, .depth = {.depthTestEnable = false, .depthWriteEnable = false}
	};
	Ref<Shader> compute, vertexOnly;
	Expect(device->CreateShader({.moduleName = "shader_object_compute"}, compute) == Result::Success,
		"Create compute program");
	Expect(device->CreateShader({.moduleName = "packed", .entryPoints = {{"vertexMain", ShaderStage::Vertex}}},
			   vertexOnly) == Result::Success,
		"Create vertex-only program");
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
	expectingError = true;
	Expect(commands->SetGraphicsState(state) == Result::InvalidParameter, "Reject state outside recording");
	Expect(commands->SetVertexLayout(layout) == Result::InvalidParameter, "Reject layout outside recording");
	commands->BindShaderProgram(shader.Raw());
	expectingError = false;

	for (uint32_t recording = 0; recording < 2; ++recording) {
		Expect(commands->Begin() == Result::Success, "Begin commands");
		commands->BindShaderProgram(compute.Raw());
		commands->DispatchCompute(1, 1, 1);
		commands->Barrier(
			{.textures = {
				 {.texture = target.Raw(),
					 .before = recording == 0 ? ResourceState{}
											  : ResourceState{PipelineStage::Transfer, ResourceAccess::TransferRead},
					 .after = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
					 .oldLayout = recording == 0 ? TextureLayout::Undefined : TextureLayout::TransferSrc,
					 .newLayout = TextureLayout::ColorAttachment}
			 }});
		commands->BeginRendering(
			{.colorAttachments = {{.target = target->GetTextureView()}}, .renderArea = {.width = 16, .height = 16}});
		commands->SetViewport({.width = 16, .height = 16});
		commands->SetScissor({.width = 16, .height = 16});
		commands->BindShaderProgram(shader.Raw());
		expectingError = true;
		const auto beforeMissingState = errors;
		commands->Draw(3);
		expectingError = false;
		Expect(errors == beforeMissingState + 1, "Each recording requires graphics state before drawing");
		Expect(commands->SetGraphicsState(state) == Result::Success, "Set independent graphics state");
		auto invalidState = state;
		invalidState.raster.cullMode = static_cast<CullMode>(999);
		expectingError = true;
		Expect(commands->SetGraphicsState(invalidState) == Result::InvalidParameter, "Reject invalid raster state");
		expectingError = false;
		// The following successful draw must still use the previous valid state.
		commands->SetVertexLayout(layout);
		commands->BindVertexBuffer(vertices.Raw(), 0, sizeof(Position));
		commands->BindVertexBuffer(instances.Raw(), 1, sizeof(Color));
		if (recording == 0) {
			auto reject = [&](const VertexLayout& invalid) {
				const auto before = errors;
				expectingError = true;
				if (commands->SetVertexLayout(invalid) == Result::Success) {
					commands->Draw(3);
				}
				expectingError = false;
				Expect(errors == before + 1, "Invalid vertex layout reports one error");
			};
			auto invalid = layout;
			invalid.attributes.pop_back();
			reject(invalid);
			invalid = layout;
			invalid.attributes[1].format = VertexFormat::UInt;
			reject(invalid);
			invalid = layout;
			invalid.attributes[1].location = 0;
			reject(invalid);
			invalid = layout;
			invalid.bindings[1].binding = 0;
			reject(invalid);
			invalid = layout;
			invalid.attributes[0].offset = sizeof(Position);
			reject(invalid);
			commands->SetVertexLayout(layout);
		}

		commands->Draw(3);
		commands->BindShaderProgram(vertexOnly.Raw());
		commands->Draw(3);
		commands->BindShaderProgram(compute.Raw());
		commands->BindShaderProgram(shader.Raw());
		commands->Draw(3);
		commands->BindShaderProgram(shader.Raw());
		commands->Draw(3);
		commands->EndRendering();
		commands->DispatchCompute(1, 1, 1);
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
		if (recording == 1) {
			shader.Reset();
			compute.Reset();
			vertexOnly.Reset();
			device->ClearShaderCache();
		}
		const auto token = device->GetGraphicsCommandQueue()->Submit(*commands);
		Expect(token.IsValid(), "Submit padded and packed draw");
		device->GetGraphicsCommandQueue()->Wait(token);
		std::array<uint8_t, 16 * 16 * 4> pixels{};
		Expect(readback->ReadData(pixels.data(), pixels.size()) == Result::Success, "Read pixels");
		const size_t center = (8 * 16 + 8) * 4;
		Expect(
			pixels[center] == 51 && pixels[center + 1] == 153 && pixels[center + 2] == 204 && pixels[center + 3] == 255,
			"Explicit offsets, strides, multiple bindings, instance rate, and normalized packed format produce correct "
			"pixels");
		Expect(commands->Reset() == Result::Success, "Reset shader object bindings and dynamic state");
	}
	std::cout << "ShaderProgramTests passed\n";
}
