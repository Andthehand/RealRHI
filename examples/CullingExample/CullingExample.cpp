#include "Device.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {
	using namespace RealRHI;
	uint32_t s_ExpectedErrors = 0;
	uint32_t s_UnexpectedErrors = 0;
	bool s_ExpectError = false;

	void Check(bool condition, const char* message) {
		if (!condition) {
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void CaptureDebugMessage(const DebugMessage& message) {
		if (message.severity == DebugSeverity::Error) {
			if (s_ExpectError && message.type == DebugMessageType::General &&
				(std::string_view(message.message).starts_with("DrawIndirect:") ||
					std::string_view(message.message).starts_with("DrawIndexedIndirect:") ||
					std::string_view(message.message).starts_with("DispatchComputeIndirect:"))) {
				++s_ExpectedErrors;
				return;
			}
			++s_UnexpectedErrors;
		}
		DefaultDebugCallback(message);
	}

	// Prefixes and padding exercise byte offsets and strides rather than assuming packed arguments.
	struct DrawRecord {
		DrawIndirectArguments draw;
		DrawIndexedIndirectArguments indexed;
		std::array<uint32_t, 3> padding{};
	};
	static_assert(sizeof(DrawRecord) == 48);
	constexpr uint64_t s_ArgumentSize = 16 + 4 * sizeof(DrawRecord);

	void RunCulling(Device& device, bool indexed, float viewHalfWidth, QueueType computeQueue, bool multiDraw) {
		Ref<Shader> cullShader;
		Ref<Shader> drawShader;
		Ref<Pipeline> cullPipeline;
		Ref<Pipeline> drawPipeline;
		Check(device.CreateShader({.moduleName = "cull"}, cullShader) == Result::Success, "Compile culling shader");
		Check(device.CreateShader({.moduleName = "culled_draw"}, drawShader) == Result::Success, "Compile draw shader");
		Check(device.CreateComputePipeline({.shader = cullShader.Raw()}, cullPipeline) == Result::Success,
			"Create culling pipeline");
		Check(device.CreateGraphicsPipeline(
				  {
					  .shader = drawShader.Raw(),
					  .rasterState = {.cullMode = CullMode::None},
					  .depthState = {.depthTestEnable = false, .depthWriteEnable = false},
					  .renderTargetFormats = {.colorFormats = {TextureFormat::RGBA8_UNorm}},
				  },
				  drawPipeline) == Result::Success,
			"Create indirect graphics pipeline");

		Ref<Buffer> arguments;
		Ref<Buffer> dispatch;
		Ref<Buffer> indices;
		Ref<Buffer> readback;
		const std::array<uint32_t, 4> dispatchData{0, 1, 1, 1};
		const std::array<uint32_t, 12> indexData{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
		Check(device.CreateBuffer({.size = s_ArgumentSize,
									  .usage = BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::TransferSrc},
				  arguments) == Result::Success,
			"Create GPU-generated draw arguments");
		Check(device.CreateBuffer({.size = sizeof(dispatchData),
									  .usage = BufferUsage::Indirect,
									  .memoryUsage = MemoryUsage::CPUToGPU,
									  .initialData = dispatchData.data()},
				  dispatch) == Result::Success,
			"Create indirect dispatch arguments");
		Check(device.CreateBuffer({.size = sizeof(indexData),
									  .usage = BufferUsage::Index,
									  .memoryUsage = MemoryUsage::CPUToGPU,
									  .initialData = indexData.data()},
				  indices) == Result::Success,
			"Create triangle indices");
		Check(device.CreateBuffer({.size = 64 * 64 * 4 + s_ArgumentSize,
									  .usage = BufferUsage::TransferDst,
									  .memoryUsage = MemoryUsage::GPUToCPU},
				  readback) == Result::Success,
			"Create result readback");

		Ref<Texture> color;
		Check(device.CreateTexture({.width = 64,
									   .height = 64,
									   .format = TextureFormat::RGBA8_UNorm,
									   .usage = TextureUsage::RenderTarget | TextureUsage::TransferSrc},
				  color) == Result::Success,
			"Create offscreen target");

		Ref<CommandAllocator> computeAllocator;
		Ref<CommandAllocator> graphicsAllocator;
		Ref<CommandList> compute;
		Ref<CommandList> graphics;
		Check(device.CreateCommandAllocator(computeQueue, computeAllocator) == Result::Success &&
				device.CreateCommandAllocator(QueueType::Graphics, graphicsAllocator) == Result::Success &&
				device.CreateCommandList(computeAllocator.Raw(), compute) == Result::Success &&
				device.CreateCommandList(graphicsAllocator.Raw(), graphics) == Result::Success,
			"Create compute and graphics command lists");

		Check(compute->Begin() == Result::Success, "Begin culling commands");
		compute->BindPipeline(cullPipeline.Raw());
		compute->UseResource(arguments.Raw());
		struct CullParameters {
			uint32_t output;
			float viewHalfWidth;
		} parameters{arguments->GetDescriptorIndex(), viewHalfWidth};
		compute->SetPushConstants(&parameters, sizeof(parameters));
#ifdef REALRHI_INDIRECT_TESTS
		s_ExpectError = true;
		compute->DispatchComputeIndirect(nullptr);
		compute->DispatchComputeIndirect(dispatch.Raw(), 2);
		compute->DispatchComputeIndirect(dispatch.Raw(), 8);
		compute->DispatchComputeIndirect(readback.Raw());
		s_ExpectError = false;
#endif
		compute->DispatchComputeIndirect(dispatch.Raw(), 4);
		// Transfer ownership as well as visibility when the queue roles use different families.
		BarrierDesc handoff;
		handoff.buffers.push_back(
			{.buffer = arguments.Raw(),
				.before = {PipelineStage::ComputeShader, ResourceAccess::ShaderWrite},
				.after =
					{PipelineStage::DrawIndirect | PipelineStage::Transfer,
						ResourceAccess::IndirectCommandRead | ResourceAccess::TransferRead},
				.size = s_ArgumentSize,
				.sourceQueue = computeQueue,
				.destinationQueue = QueueType::Graphics});
		compute->Barrier(handoff);
		Check(compute->End() == Result::Success, "End culling commands");
		auto* producer = device.GetCommandQueue(computeQueue);
		const auto ready = producer->Submit(*compute);
		Check(ready.IsValid(), "Submit compute culling");

		Check(graphics->Begin() == Result::Success, "Begin indirect rendering");
		if (producer->GetQueueId().familyIndex != device.GetGraphicsCommandQueue()->GetQueueId().familyIndex) {
			graphics->Barrier(handoff);
		}
		BarrierDesc transition;
		transition.textures.push_back(
			{.texture = color.Raw(),
				.after = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
				.oldLayout = TextureLayout::Undefined,
				.newLayout = TextureLayout::ColorAttachment});
		graphics->Barrier(transition);
		graphics->BeginRendering(
			{.colorAttachments = {{.target = color->GetTextureView()}}, .renderArea = {.width = 64, .height = 64}});
		graphics->SetViewport({.width = 64, .height = 64});
		graphics->SetScissor({.width = 64, .height = 64});
		graphics->BindPipeline(drawPipeline.Raw());
#ifdef REALRHI_INDIRECT_TESTS
		s_ExpectError = true;
		graphics->DispatchComputeIndirect(dispatch.Raw());
		graphics->DrawIndexedIndirect(arguments.Raw(), 32);
		graphics->DrawIndirect(nullptr);
		graphics->DrawIndirect(arguments.Raw(), 2);
		graphics->DrawIndirect(arguments.Raw(), s_ArgumentSize - 4);
		graphics->DrawIndirect(arguments.Raw(), 16, 2, 12);
		graphics->DrawIndirect(arguments.Raw(), 16, 2, 17);
		graphics->DrawIndirect(arguments.Raw(), 16, UINT32_MAX, UINT32_MAX - 3);
		s_ExpectError = false;
#endif
		graphics->BindIndexBuffer(indices.Raw());
#ifdef REALRHI_INDIRECT_TESTS
		s_ExpectError = true;
		graphics->DrawIndexedIndirect(nullptr);
		graphics->DrawIndexedIndirect(arguments.Raw(), 2);
		graphics->DrawIndexedIndirect(arguments.Raw(), s_ArgumentSize - 4);
		graphics->DrawIndexedIndirect(arguments.Raw(), 32, 2, 16);
		graphics->DrawIndexedIndirect(arguments.Raw(), 32, 2, 21);
		s_ExpectError = false;
#endif
		graphics->DrawIndirect(arguments.Raw(), s_ArgumentSize, 0);
		graphics->DrawIndexedIndirect(arguments.Raw(), s_ArgumentSize, 0);

		if (multiDraw && device.GetCapabilities().features.multiDrawIndirect &&
			device.GetCapabilities().limits.maxDrawIndirectCount >= 4) {
			if (indexed) {
				graphics->DrawIndexedIndirect(arguments.Raw(), 32, 4, sizeof(DrawRecord));
			}
			else {
				graphics->DrawIndirect(arguments.Raw(), 16, 4, sizeof(DrawRecord));
			}
		}
		else {
			for (uint32_t i = 0; i < 4; ++i) {
				if (indexed) {
					graphics->DrawIndexedIndirect(arguments.Raw(), 32 + i * sizeof(DrawRecord));
				}
				else {
					graphics->DrawIndirect(arguments.Raw(), 16 + i * sizeof(DrawRecord));
				}
			}
		}
		graphics->EndRendering();
		transition.textures[0].before = transition.textures[0].after;
		transition.textures[0].after = {PipelineStage::Transfer, ResourceAccess::TransferRead};
		transition.textures[0].oldLayout = TextureLayout::ColorAttachment;
		transition.textures[0].newLayout = TextureLayout::TransferSrc;
		graphics->Barrier(transition);
		graphics->CopyTextureToBuffer(color.Raw(), readback.Raw(), {.width = 64, .height = 64});
		graphics->CopyBuffer(
			arguments.Raw(), readback.Raw(), {.destinationOffset = 64 * 64 * 4, .size = s_ArgumentSize});
		Check(graphics->End() == Result::Success, "End indirect rendering");

		// Recorded lists retain argument and index buffers through GPU execution.
		arguments.Reset();
		dispatch.Reset();
		indices.Reset();

		std::array<CommandList*, 1> lists{graphics.Raw()};
		const std::array<SubmissionToken, 1> waits{ready};
		// The GPU wait covers both distinct queues and queue roles mapped to the same queue.
		const auto finished = device.GetGraphicsCommandQueue()->Submit({.commandLists = lists, .waitFor = waits});
		Check(finished.IsValid(), "Submit graphics with compute dependency");
		device.GetGraphicsCommandQueue()->Wait(finished);
		producer->Wait(ready);

		std::array<uint8_t, 64 * 64 * 4> pixels{};
		std::array<DrawRecord, 4> records{};
		Check(readback->ReadData(pixels.data(), pixels.size()) == Result::Success, "Read rendered pixels");
		Check(readback->ReadData(records.data(), sizeof(records), pixels.size() + 16) == Result::Success,
			"Read GPU culling decisions");
		for (uint32_t i = 0; i < 4; ++i) {
			uint32_t visible = 0;
			if (i < 2 && viewHalfWidth > 0) {
				visible = 1;
			}
			Check(records[i].draw.vertexCount == 3 && records[i].draw.instanceCount == visible &&
					records[i].draw.firstVertex == i * 3 && records[i].draw.firstInstance == 0 &&
					records[i].indexed.indexCount == 3 && records[i].indexed.instanceCount == visible &&
					records[i].indexed.firstIndex == i * 3 && records[i].indexed.vertexOffset == 0 &&
					records[i].indexed.firstInstance == 0,
				"GPU emits complete arguments and culls invisible objects");
		}
		uint8_t green = 0;
		if (viewHalfWidth > 0) {
			green = 255;
		}
		for (uint32_t x : {16u, 48u}) {
			const auto offset = (32 * 64 + x) * 4;
			Check(pixels[offset] == 0 && pixels[offset + 1] == green && pixels[offset + 2] == 0,
				"Only visible triangles produce green pixels");
		}
		Check(pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 0, "Background retains clear color");
	}
}

int main() {
	std::filesystem::path shaderDirectory = std::filesystem::current_path() / "shaders";
#ifdef REALRHI_INDIRECT_TESTS
	shaderDirectory = REALRHI_CULLING_SHADER_DIRECTORY;
#endif
	std::unique_ptr<RealRHI::Device> device;
	Check(RealRHI::Device::Create({.applicationName = "CullingExample",
									  .shaderDirectory = shaderDirectory,
									  .enableDebug = true,
									  .enableValidationLayers = true,
									  .debugCallback = CaptureDebugMessage},
			  device) == RealRHI::Result::Success,
		"Create culling device");
	for (auto queue : {RealRHI::QueueType::Graphics, RealRHI::QueueType::Compute}) {
		for (bool indexed : {false, true}) {
			for (bool multiDraw : {false, true}) {
				RunCulling(*device, indexed, 1.f, queue, multiDraw);
				RunCulling(*device, indexed, 0.f, queue, multiDraw);
			}
		}
	}
	device->WaitIdle();
	device.reset();
#ifdef REALRHI_INDIRECT_TESTS
	Check(s_ExpectedErrors == 16 * 17, "Invalid indirect operations were all rejected");
#endif
	Check(s_UnexpectedErrors == 0, "Culling completes without unexpected errors");
	std::cout
		<< "CullingExample passed: compute-generated indexed and non-indexed draws, two visible / four candidates.\n";
}
