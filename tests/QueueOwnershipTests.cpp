#include "Device.h"
#include "Vulkan/VulkanCommandQueue.h"
#include "Vulkan/VulkanDevice.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
	using namespace RealRHI;

	class GraphicsQueueTestDevice final : public VulkanDevice {
	public:
		using VulkanDevice::GetCommandQueue;

		CommandQueue* GetCommandQueue(QueueType type) const override {
			switch (type) {
				case QueueType::Graphics:
				case QueueType::Compute:
				case QueueType::Transfer:
					return GetGraphicsCommandQueue();
				default:
					return nullptr;
			}
		}
	};

	uint32_t g_ProgrammerErrors = 0;
	uint32_t g_ExpectedErrors = 0;
	std::string g_LastError;

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void CaptureDebugMessage(const DebugMessage& message) {
		DefaultDebugCallback(message);
		if (message.severity == DebugSeverity::Error) {
			Expect(message.type == DebugMessageType::General, "Unexpected Vulkan validation or shader error");
			++g_ProgrammerErrors;
			g_LastError = message.message;
		}
	}

	Ref<CommandList> MakeList(Device& device, QueueType type) {
		Ref<CommandAllocator> allocator;
		Ref<CommandList> list;
		Expect(device.CreateCommandAllocator(type, allocator) == Result::Success, "Create allocator");
		Expect(device.CreateCommandList(allocator.Raw(), list) == Result::Success, "Create command list");
		Expect(list->Begin() == Result::Success, "Begin list");
		return list;
	}

	Ref<Buffer> MakeBuffer(Device& device,
		uint64_t size,
		BufferUsage usage,
		MemoryUsage memory = MemoryUsage::GPUOnly,
		const void* data = nullptr) {
		Ref<Buffer> buffer;
		Expect(device.CreateBuffer({.size = size, .usage = usage, .memoryUsage = memory, .initialData = data},
				   buffer) == Result::Success,
			"Create buffer");
		return buffer;
	}

	Ref<Texture> MakeTexture(Device& device, TextureUsage usage, uint32_t mips = 1, uint32_t layers = 1) {
		Ref<Texture> texture;
		Expect(device.CreateTexture({.width = 4,
										.height = 4,
										.mipLevels = mips,
										.arrayLayers = layers,
										.format = TextureFormat::RGBA8_UNorm,
										.usage = usage},
				   texture) == Result::Success,
			"Create texture");
		return texture;
	}

	Ref<Pipeline> MakeCompute(Device& device, const char* module) {
		Ref<Shader> shader;
		Ref<Pipeline> pipeline;
		Expect(
			device.CreateShader({.moduleName = module, .entryPoints = {{"computeMain", RealRHI::ShaderStage::Compute}}},
				shader) == Result::Success,
			"Compile compute shader");
		Expect(device.CreateComputePipeline({.shader = shader.Raw()}, pipeline) == Result::Success,
			"Create compute pipeline");
		return pipeline;
	}

	SubmissionToken Submit(Device& device,
		QueueType type,
		CommandList& list,
		std::span<const SubmissionToken> waits = {}) {
		CommandList* lists[] = {&list};
		return device.GetCommandQueue(type)->Submit({.commandLists = lists, .waitFor = waits});
	}

	void End(CommandList& list) {
		Expect(list.End() == Result::Success, "End command list");
	}

	void ExpectRejected(Device& device,
		QueueType type,
		CommandList& list,
		const char* cause,
		std::span<const SubmissionToken> waits = {}) {
		const auto before = g_ProgrammerErrors;
		Expect(!Submit(device, type, list, waits).IsValid(), "Invalid submission must be rejected");
		Expect(g_ProgrammerErrors == before + 1 && g_LastError.find(cause) != std::string::npos,
			"Rejected submission must explain its cause");
		Expect(list.GetState() == CommandListState::Executable, "Rejection preserves executable state");
		++g_ExpectedErrors;
	}

	template <typename Operation>
	void ExpectRecordingError(Operation operation, const char* cause) {
		const auto before = g_ProgrammerErrors;
		operation();
		if (g_ProgrammerErrors != before + 1 || g_LastError.find(cause) == std::string::npos) {
			std::cerr << "Expected diagnostic containing '" << cause << "', got '" << g_LastError << "'; error count "
					  << before << " -> " << g_ProgrammerErrors << '\n';
		}
		Expect(g_ProgrammerErrors == before + 1 && g_LastError.find(cause) != std::string::npos,
			"Invalid recording logs its specific cause");
		++g_ExpectedErrors;
	}

	void BufferBarrierFor(CommandList& list,
		Buffer* buffer,
		uint64_t offset,
		uint64_t size,
		QueueType source,
		QueueType destination,
		ResourceState before,
		ResourceState after) {
		BarrierDesc barriers;
		barriers.buffers.push_back(
			{.buffer = buffer,
				.before = before,
				.after = after,
				.offset = offset,
				.size = size,
				.sourceQueue = source,
				.destinationQueue = destination});
		list.Barrier(barriers);
	}

	void TextureBarrierFor(CommandList& list,
		Texture* texture,
		TextureLayout oldLayout,
		TextureLayout newLayout,
		ResourceState before,
		ResourceState after,
		std::optional<QueueType> source = {},
		std::optional<QueueType> destination = {},
		TextureSubresourceRange range = {}) {
		BarrierDesc barriers;
		barriers.textures.push_back(
			{.texture = texture,
				.before = before,
				.after = after,
				.oldLayout = oldLayout,
				.newLayout = newLayout,
				.subresources = range,
				.sourceQueue = source,
				.destinationQueue = destination});
		list.Barrier(barriers);
	}

	constexpr ResourceState transferWrite{PipelineStage::Transfer, ResourceAccess::TransferWrite};
	constexpr ResourceState transferRead{PipelineStage::Transfer, ResourceAccess::TransferRead};
	constexpr ResourceState computeReadWrite{
		PipelineStage::ComputeShader,
		ResourceAccess::ShaderRead | ResourceAccess::ShaderWrite
	};
	constexpr ResourceState fragmentRead{PipelineStage::FragmentShader, ResourceAccess::ShaderRead};

	bool DifferentFamilies(Device& device, QueueType first, QueueType second) {
		return device.GetCommandQueue(first)->GetQueueId().familyIndex !=
			device.GetCommandQueue(second)->GetQueueId().familyIndex;
	}

	VKAPI_ATTR VkResult VKAPI_CALL FailSubmit(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	void TestBufferChain(Device& device) {
		std::array<uint32_t, 64> input{};
		for (size_t i = 0; i < input.size(); ++i) {
			input[i] = static_cast<uint32_t>(i + 10);
		}
		input[3] = 240;
		auto staging = MakeBuffer(device, sizeof(input), BufferUsage::TransferSrc, MemoryUsage::CPUToGPU, input.data());
		auto values = MakeBuffer(
			device, sizeof(input), BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc);
		auto result = MakeBuffer(device, sizeof(input), BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		auto pixels = MakeBuffer(device, 64, BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		auto target = MakeTexture(device, TextureUsage::RenderTarget | TextureUsage::TransferSrc);
		const float positions[] = {-1, -1, -1, 3, 3, -1};
		auto vertices = MakeBuffer(device, sizeof(positions), BufferUsage::Vertex, MemoryUsage::CPUToGPU, positions);
		auto computePipeline = MakeCompute(device, "ownership_compute");
		Ref<Shader> graphicsShader;
		Ref<Pipeline> graphicsPipeline;
		Expect(device.CreateShader({.moduleName = "ownership_graphics",
									   .entryPoints =
										   {{"vertexMain", RealRHI::ShaderStage::Vertex},
											   {"fragmentMain", RealRHI::ShaderStage::Fragment}}},
				   graphicsShader) == Result::Success,
			"Compile graphics shader");
		PipelineDesc pipelineDesc{};
		pipelineDesc.shader = graphicsShader.Raw();
		pipelineDesc.vertexLayout = {
			.bindings = {{.stride = sizeof(float) * 2}}, .attributes = {{.format = RealRHI::VertexFormat::Float2}}
		};
		pipelineDesc.rasterState.cullMode = CullMode::None;
		pipelineDesc.depthState.depthTestEnable = false;
		pipelineDesc.depthState.depthWriteEnable = false;
		pipelineDesc.renderTargetFormats.colorFormats = {TextureFormat::RGBA8_UNorm};
		Expect(device.CreateGraphicsPipeline(pipelineDesc, graphicsPipeline) == Result::Success,
			"Create graphics pipeline");
		const bool transferToCompute = DifferentFamilies(device, QueueType::Transfer, QueueType::Compute);
		const bool computeToGraphics = DifferentFamilies(device, QueueType::Compute, QueueType::Graphics);

		// Record the acquire before the release is submitted: state is resolved at submission, not recording.
		auto compute = MakeList(device, QueueType::Compute);
		compute->UseResource(values.Raw());
		BufferBarrierFor(*compute,
			values.Raw(),
			0,
			sizeof(input),
			QueueType::Transfer,
			QueueType::Compute,
			transferWrite,
			computeReadWrite);
		compute->BindPipeline(computePipeline.Raw());
		const uint32_t parameters[] = {values->GetDescriptorIndex(), static_cast<uint32_t>(input.size())};
		compute->SetPushConstants(parameters, sizeof(parameters));
		compute->DispatchCompute(1, 1, 1);
		if (computeToGraphics) {
			BufferBarrierFor(*compute,
				values.Raw(),
				0,
				sizeof(input),
				QueueType::Compute,
				QueueType::Graphics,
				computeReadWrite,
				fragmentRead);
		}
		End(*compute);
		if (transferToCompute) {
			ExpectRejected(device, QueueType::Compute, *compute, "matching release");
		}

		auto oldList = MakeList(device, QueueType::Transfer);
		End(*oldList);
		const auto oldToken = Submit(device, QueueType::Transfer, *oldList);
		Expect(oldToken.IsValid(), "Submit token preceding release");
		auto transfer = MakeList(device, QueueType::Transfer);
		transfer->CopyBuffer(staging.Raw(), values.Raw(), {.size = sizeof(input)});
		if (transferToCompute) {
			BufferBarrierFor(*transfer,
				values.Raw(),
				0,
				sizeof(input),
				QueueType::Transfer,
				QueueType::Compute,
				transferWrite,
				computeReadWrite);
		}
		End(*transfer);

		const auto submitFunction = vkQueueSubmit2;
		vkQueueSubmit2 = FailSubmit;
		ExpectRejected(device, QueueType::Transfer, *transfer, "Failed to submit Vulkan");
		vkQueueSubmit2 = submitFunction;
		const auto release = Submit(device, QueueType::Transfer, *transfer);
		Expect(release.IsValid() && release.value == oldToken.value + 1,
			"Failed native submission publishes neither state nor token");
		if (transferToCompute) {
			ExpectRejected(device, QueueType::Compute, *compute, "waitFor");
			ExpectRejected(device, QueueType::Compute, *compute, "waitFor", {&oldToken, 1});
			auto wrongRange = MakeList(device, QueueType::Compute);
			BufferBarrierFor(*wrongRange,
				values.Raw(),
				0,
				sizeof(input) / 2,
				QueueType::Transfer,
				QueueType::Compute,
				transferWrite,
				computeReadWrite);
			End(*wrongRange);
			ExpectRejected(device, QueueType::Compute, *wrongRange, "offset and size", {&release, 1});
			auto sourceUse = MakeList(device, QueueType::Transfer);
			sourceUse->CopyBuffer(values.Raw(), result.Raw(), {.size = sizeof(input)});
			End(*sourceUse);
			ExpectRejected(device, QueueType::Transfer, *sourceUse, "was released");
		}
		if (transferToCompute && computeToGraphics) {
			auto useAfterRelease = MakeList(device, QueueType::Compute);
			useAfterRelease->BindPipeline(computePipeline.Raw());
			useAfterRelease->UseResource(values.Raw());
			useAfterRelease->SetPushConstants(parameters, sizeof(parameters));
			BufferBarrierFor(*useAfterRelease,
				values.Raw(),
				0,
				sizeof(input),
				QueueType::Transfer,
				QueueType::Compute,
				transferWrite,
				computeReadWrite);
			useAfterRelease->DispatchCompute(1, 1, 1);
			BufferBarrierFor(*useAfterRelease,
				values.Raw(),
				0,
				sizeof(input),
				QueueType::Compute,
				QueueType::Graphics,
				computeReadWrite,
				fragmentRead);
			useAfterRelease->DispatchCompute(1, 1, 1);
			End(*useAfterRelease);
			ExpectRejected(device, QueueType::Compute, *useAfterRelease, "was released", {&release, 1});
		}
		auto laterList = MakeList(device, QueueType::Transfer);
		End(*laterList);
		const auto later = Submit(device, QueueType::Transfer, *laterList);
		Expect(later.IsValid() && later.value > release.value, "Create a later source-queue dependency");
		const SubmissionToken dependencies[] = {oldToken, release, later, release};
		const auto computed = Submit(device, QueueType::Compute, *compute, dependencies);
		Expect(computed.IsValid(), "Acquire, compute and release on compute queue");

		Ref<Window> window;
		Ref<Swapchain> swapchain;
		Expect(device.CreateWindow({.Title = "RealRHI ownership test", .Width = 64, .Height = 64}, window) ==
				Result::Success,
			"Create presentation window");
		Expect(device.CreateSwapchain({.window = window.Raw()}, swapchain) == Result::Success,
			"Create presentation swapchain");
		const auto frame = swapchain->BeginFrame();
		Expect(frame.backBufferView != nullptr, "Acquire presentation frame");
		auto graphics = MakeList(device, QueueType::Graphics);
		BufferBarrierFor(*graphics,
			values.Raw(),
			0,
			sizeof(input),
			QueueType::Compute,
			QueueType::Graphics,
			computeReadWrite,
			fragmentRead);
		TextureBarrierFor(*graphics,
			target.Raw(),
			TextureLayout::Undefined,
			TextureLayout::ColorAttachment,
			{},
			{PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite});
		RenderingInfo rendering{};
		rendering.colorAttachments.push_back({.target = target->GetTextureView()});
		rendering.renderArea = {.width = 4, .height = 4};
		graphics->BeginRendering(rendering);
		graphics->BindPipeline(graphicsPipeline.Raw());
		graphics->BindVertexBuffer(vertices.Raw());
		graphics->UseResource(values.Raw());
		graphics->SetPushConstants(parameters, sizeof(parameters));
		graphics->SetViewport({.width = 4, .height = 4});
		graphics->SetScissor({.width = 4, .height = 4});
		graphics->Draw(3);
		graphics->EndRendering();
		TextureBarrierFor(*graphics,
			target.Raw(),
			TextureLayout::ColorAttachment,
			TextureLayout::TransferSrc,
			{PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
			transferRead);
		graphics->CopyTextureToBuffer(target.Raw(), pixels.Raw(), {.width = 4, .height = 4});
		graphics->CopyBuffer(values.Raw(), result.Raw(), {.size = sizeof(input)});
		RenderingInfo frameRendering{};
		frameRendering.colorAttachments.push_back({.target = frame.backBufferView});
		frameRendering.renderArea = {.width = frame.width, .height = frame.height};
		graphics->BeginRendering(frameRendering);
		graphics->EndRendering();
		End(*graphics);
		if (computeToGraphics) {
			ExpectRejected(device, QueueType::Graphics, *graphics, "waitFor");
		}
		SubmissionToken rendered;
		Expect(device.Submit(graphics.Raw(), swapchain.Raw(), frame, &rendered, {&computed, 1}) == Result::Success &&
				rendered.IsValid(),
			"Frame submission combines the compute dependency with swapchain synchronization");
		swapchain->Present(frame);
		device.GetCommandQueue(QueueType::Graphics)->Wait(rendered);
		std::array<uint32_t, 64> actual{};
		Expect(result->ReadData(actual.data(), sizeof(actual)) == Result::Success, "Read computed values");
		for (size_t i = 0; i < actual.size(); ++i) {
			Expect(actual[i] == input[i] + 3, "Transfer-compute-graphics data matches");
		}
		std::array<uint8_t, 64> actualPixels{};
		Expect(pixels->ReadData(actualPixels.data(), actualPixels.size()) == Result::Success, "Read rendered pixels");
		for (size_t i = 0; i < actualPixels.size(); ++i) {
			Expect(actualPixels[i] == input[i % 4] + 3, "Every rendered pixel reflects computed buffer contents");
		}
		device.WaitIdle();
	}

	void TestTextureChain(Device& device) {
		std::array<uint8_t, 64> input{};
		for (size_t i = 0; i < input.size(); ++i) {
			input[i] = static_cast<uint8_t>(i * 3);
		}
		auto staging = MakeBuffer(device, input.size(), BufferUsage::TransferSrc, MemoryUsage::CPUToGPU, input.data());
		auto texture =
			MakeTexture(device, TextureUsage::Storage | TextureUsage::TransferDst | TextureUsage::TransferSrc);
		auto result = MakeBuffer(device, input.size(), BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		auto pipeline = MakeCompute(device, "ownership_texture");
		const bool transferToCompute = DifferentFamilies(device, QueueType::Transfer, QueueType::Compute);
		const bool computeToGraphics = DifferentFamilies(device, QueueType::Compute, QueueType::Graphics);
		auto compute = MakeList(device, QueueType::Compute);
		TextureBarrierFor(*compute,
			texture.Raw(),
			TextureLayout::TransferDst,
			TextureLayout::General,
			transferWrite,
			computeReadWrite,
			QueueType::Transfer,
			QueueType::Compute);
		compute->BindPipeline(pipeline.Raw());
		compute->UseResource(texture.Raw());
		const auto descriptor = texture->GetStorageImageDescriptorIndex();
		compute->SetPushConstants(&descriptor, sizeof(descriptor));
		compute->DispatchCompute(1, 1, 1);
		if (computeToGraphics) {
			TextureBarrierFor(*compute,
				texture.Raw(),
				TextureLayout::General,
				TextureLayout::TransferSrc,
				computeReadWrite,
				transferRead,
				QueueType::Compute,
				QueueType::Graphics);
		}
		End(*compute);
		if (transferToCompute) {
			ExpectRejected(device, QueueType::Compute, *compute, "matching release");
		}

		auto transfer = MakeList(device, QueueType::Transfer);
		TextureBarrierFor(
			*transfer, texture.Raw(), TextureLayout::Undefined, TextureLayout::TransferDst, {}, transferWrite);
		transfer->CopyBufferToTexture(staging.Raw(), texture.Raw(), {.width = 4, .height = 4});
		if (transferToCompute) {
			TextureBarrierFor(*transfer,
				texture.Raw(),
				TextureLayout::TransferDst,
				TextureLayout::General,
				transferWrite,
				computeReadWrite,
				QueueType::Transfer,
				QueueType::Compute);
		}
		End(*transfer);
		const auto submitFunction = vkQueueSubmit2;
		vkQueueSubmit2 = FailSubmit;
		ExpectRejected(device, QueueType::Transfer, *transfer, "Failed to submit Vulkan");
		vkQueueSubmit2 = submitFunction;
		const auto release = Submit(device, QueueType::Transfer, *transfer);
		Expect(release.IsValid(), "Upload and release texture");
		if (transferToCompute) {
			ExpectRejected(device, QueueType::Compute, *compute, "waitFor");
			auto mismatch = MakeList(device, QueueType::Compute);
			TextureBarrierFor(*mismatch,
				texture.Raw(),
				TextureLayout::General,
				TextureLayout::General,
				{},
				computeReadWrite,
				QueueType::Transfer,
				QueueType::Compute);
			End(*mismatch);
			ExpectRejected(device, QueueType::Compute, *mismatch, "old/new layouts", {&release, 1});
			auto undefined = MakeList(device, QueueType::Compute);
			TextureBarrierFor(*undefined,
				texture.Raw(),
				TextureLayout::Undefined,
				TextureLayout::General,
				{},
				computeReadWrite,
				QueueType::Transfer,
				QueueType::Compute);
			End(*undefined);
			ExpectRejected(device, QueueType::Compute, *undefined, "old/new layouts", {&release, 1});
			auto discard = MakeList(device, QueueType::Transfer);
			TextureBarrierFor(
				*discard, texture.Raw(), TextureLayout::Undefined, TextureLayout::TransferDst, {}, transferWrite);
			End(*discard);
			ExpectRejected(device, QueueType::Transfer, *discard, "was released");
		}
		if (transferToCompute && computeToGraphics) {
			auto useAfterRelease = MakeList(device, QueueType::Compute);
			useAfterRelease->BindPipeline(pipeline.Raw());
			useAfterRelease->UseResource(texture.Raw());
			useAfterRelease->SetPushConstants(&descriptor, sizeof(descriptor));
			TextureBarrierFor(*useAfterRelease,
				texture.Raw(),
				TextureLayout::TransferDst,
				TextureLayout::General,
				transferWrite,
				computeReadWrite,
				QueueType::Transfer,
				QueueType::Compute);
			useAfterRelease->DispatchCompute(1, 1, 1);
			TextureBarrierFor(*useAfterRelease,
				texture.Raw(),
				TextureLayout::General,
				TextureLayout::General,
				computeReadWrite,
				transferRead,
				QueueType::Compute,
				QueueType::Graphics);
			useAfterRelease->DispatchCompute(1, 1, 1);
			End(*useAfterRelease);
			ExpectRejected(device, QueueType::Compute, *useAfterRelease, "was released", {&release, 1});
		}
		const auto computed = Submit(device, QueueType::Compute, *compute, {&release, 1});
		Expect(computed.IsValid(), "Acquire and transform storage texture");
		auto graphics = MakeList(device, QueueType::Graphics);
		TextureBarrierFor(*graphics,
			texture.Raw(),
			TextureLayout::General,
			TextureLayout::TransferSrc,
			computeReadWrite,
			transferRead,
			QueueType::Compute,
			QueueType::Graphics);
		graphics->CopyTextureToBuffer(texture.Raw(), result.Raw(), {.width = 4, .height = 4});
		End(*graphics);
		if (computeToGraphics) {
			ExpectRejected(device, QueueType::Graphics, *graphics, "waitFor");
		}
		const auto copied = Submit(device, QueueType::Graphics, *graphics, {&computed, 1});
		Expect(copied.IsValid(), "Graphics acquires and reads transformed texture");
		device.GetCommandQueue(QueueType::Graphics)->Wait(copied);
		std::array<uint8_t, 64> actual{};
		Expect(result->ReadData(actual.data(), actual.size()) == Result::Success, "Read texture pixels");
		for (size_t i = 0; i < actual.size(); ++i) {
			Expect(actual[i] == 255 - input[i], "Texture bytes survive ownership transfers and compute");
		}
		device.WaitIdle();
	}

	void TestRangesAndBatch(Device& device) {
		if (!DifferentFamilies(device, QueueType::Transfer, QueueType::Compute)) {
			std::cout << "Range ownership negatives require distinct transfer/compute families; shared-family data "
						 "paths ran above.\n";
			return;
		}
		std::array<uint32_t, 16> input{};
		for (size_t i = 0; i < input.size(); ++i) {
			input[i] = static_cast<uint32_t>(i * 19);
		}
		auto staging = MakeBuffer(device, sizeof(input), BufferUsage::TransferSrc, MemoryUsage::CPUToGPU, input.data());
		auto buffer = MakeBuffer(device, sizeof(input), BufferUsage::TransferSrc | BufferUsage::TransferDst);
		auto output = MakeBuffer(device, sizeof(input), BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		auto transfer = MakeList(device, QueueType::Transfer);
		transfer->CopyBuffer(staging.Raw(), buffer.Raw(), {.size = sizeof(input)});
		BufferBarrierFor(*transfer,
			buffer.Raw(),
			0,
			sizeof(input) / 2,
			QueueType::Transfer,
			QueueType::Compute,
			transferWrite,
			transferRead);
		End(*transfer);
		const auto released = Submit(device, QueueType::Transfer, *transfer);
		Expect(released.IsValid(), "Release only the first half of a buffer");
		auto foreign = MakeList(device, QueueType::Compute);
		foreign->CopyBuffer(buffer.Raw(),
			output.Raw(),
			{.sourceOffset = sizeof(input) / 2, .destinationOffset = sizeof(input) / 2, .size = sizeof(input) / 2});
		End(*foreign);
		ExpectRejected(device, QueueType::Compute, *foreign, "belongs to another family", {&released, 1});
		auto compute = MakeList(device, QueueType::Compute);
		BufferBarrierFor(*compute,
			buffer.Raw(),
			0,
			sizeof(input) / 2,
			QueueType::Transfer,
			QueueType::Compute,
			transferWrite,
			transferRead);
		compute->CopyBuffer(buffer.Raw(), output.Raw(), {.size = sizeof(input) / 2});
		End(*compute);
		const auto acquired = Submit(device, QueueType::Compute, *compute, {&released, 1});
		Expect(acquired.IsValid(), "Acquire and use first buffer half");
		auto secondHalf = MakeList(device, QueueType::Transfer);
		BufferBarrierFor(*secondHalf,
			buffer.Raw(),
			sizeof(input) / 2,
			sizeof(input) / 2,
			QueueType::Transfer,
			QueueType::Transfer,
			transferWrite,
			transferRead);
		secondHalf->CopyBuffer(buffer.Raw(),
			output.Raw(),
			{.sourceOffset = sizeof(input) / 2, .destinationOffset = sizeof(input) / 2, .size = sizeof(input) / 2});
		End(*secondHalf);
		const auto second = Submit(device, QueueType::Transfer, *secondHalf);
		Expect(second.IsValid(), "Source family still owns the disjoint second half");
		device.GetCommandQueue(QueueType::Compute)->Wait(acquired);
		device.GetCommandQueue(QueueType::Transfer)->Wait(second);
		ExpectRejected(device, QueueType::Compute, *compute, "matching release", {&released, 1});
		std::array<uint32_t, 16> actual{};
		Expect(output->ReadData(actual.data(), sizeof(actual)) == Result::Success && actual == input,
			"Independent byte ranges preserve all data");

		auto batchBuffer = MakeBuffer(device, sizeof(input), BufferUsage::TransferSrc | BufferUsage::TransferDst);
		auto batchOutput = MakeBuffer(device, sizeof(input), BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		auto first = MakeList(device, QueueType::Transfer);
		first->CopyBuffer(staging.Raw(), batchBuffer.Raw(), {.size = sizeof(input)});
		BufferBarrierFor(*first,
			batchBuffer.Raw(),
			0,
			sizeof(input),
			QueueType::Transfer,
			QueueType::Compute,
			transferWrite,
			transferRead);
		End(*first);
		auto invalid = MakeList(device, QueueType::Transfer);
		invalid->CopyBuffer(batchBuffer.Raw(), batchOutput.Raw(), {.size = sizeof(input)});
		End(*invalid);
		CommandList* batch[] = {first.Raw(), invalid.Raw()};
		const auto before = g_ProgrammerErrors;
		Expect(!device.GetCommandQueue(QueueType::Transfer)->Submit(batch).IsValid(),
			"Reject use after release across a batch");
		Expect(g_ProgrammerErrors == before + 1 && g_LastError.find("was released") != std::string::npos,
			"Batch rejection identifies use after release");
		++g_ExpectedErrors;
		Expect(first->GetState() == CommandListState::Executable && invalid->GetState() == CommandListState::Executable,
			"Batch rejection preserves every list");
		const auto batchRelease = Submit(device, QueueType::Transfer, *first);
		Expect(batchRelease.IsValid(), "Retry first list proves rejected batch published no ownership");
		auto read = MakeList(device, QueueType::Compute);
		BufferBarrierFor(*read,
			batchBuffer.Raw(),
			0,
			sizeof(input),
			QueueType::Transfer,
			QueueType::Compute,
			transferWrite,
			transferRead);
		read->CopyBuffer(batchBuffer.Raw(), batchOutput.Raw(), {.size = sizeof(input)});
		End(*read);
		const auto copied = Submit(device, QueueType::Compute, *read, {&batchRelease, 1});
		Expect(copied.IsValid(), "Acquire after rejected batch and corrected release");
		device.GetCommandQueue(QueueType::Compute)->Wait(copied);
		Expect(batchOutput->ReadData(actual.data(), sizeof(actual)) == Result::Success && actual == input,
			"Corrected batch transfer preserves data");
		device.WaitIdle();
	}

	void TestTextureSubresources(Device& device) {
		if (!DifferentFamilies(device, QueueType::Transfer, QueueType::Compute)) {
			return;
		}
		std::array<uint8_t, 64> input{};
		for (size_t i = 0; i < input.size(); ++i) {
			input[i] = static_cast<uint8_t>(i + 7);
		}
		auto staging = MakeBuffer(device, input.size(), BufferUsage::TransferSrc, MemoryUsage::CPUToGPU, input.data());
		auto texture = MakeTexture(device, TextureUsage::TransferDst | TextureUsage::TransferSrc, 2, 2);
		auto output = MakeBuffer(device, 96, BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		auto transfer = MakeList(device, QueueType::Transfer);
		TextureBarrierFor(*transfer,
			texture.Raw(),
			TextureLayout::Undefined,
			TextureLayout::TransferDst,
			{},
			transferWrite,
			{},
			{},
			{0, 2, 0, 2});
		transfer->CopyBufferToTexture(
			staging.Raw(), texture.Raw(), {.width = 2, .height = 2, .mipLevel = 1, .layerCount = 2});
		transfer->CopyBufferToTexture(staging.Raw(), texture.Raw(), {.width = 4, .height = 4});
		TextureBarrierFor(*transfer,
			texture.Raw(),
			TextureLayout::TransferDst,
			TextureLayout::TransferSrc,
			transferWrite,
			transferRead,
			QueueType::Transfer,
			QueueType::Compute,
			{1, 1, 0, 2});
		End(*transfer);
		const auto released = Submit(device, QueueType::Transfer, *transfer);
		Expect(released.IsValid(), "Release two layers of only mip one");
		auto subset = MakeList(device, QueueType::Compute);
		TextureBarrierFor(*subset,
			texture.Raw(),
			TextureLayout::TransferDst,
			TextureLayout::TransferSrc,
			transferWrite,
			transferRead,
			QueueType::Transfer,
			QueueType::Compute,
			{1, 1, 1, 1});
		End(*subset);
		ExpectRejected(device, QueueType::Compute, *subset, "mip/layer range", {&released, 1});
		auto foreign = MakeList(device, QueueType::Compute);
		TextureBarrierFor(
			*foreign, texture.Raw(), TextureLayout::Undefined, TextureLayout::TransferSrc, {}, transferRead);
		End(*foreign);
		ExpectRejected(device, QueueType::Compute, *foreign, "belongs to another family", {&released, 1});
		auto compute = MakeList(device, QueueType::Compute);
		TextureBarrierFor(*compute,
			texture.Raw(),
			TextureLayout::TransferDst,
			TextureLayout::TransferSrc,
			transferWrite,
			transferRead,
			QueueType::Transfer,
			QueueType::Compute,
			{1, 1, 0, 2});
		compute->CopyTextureToBuffer(
			texture.Raw(), output.Raw(), {.width = 2, .height = 2, .mipLevel = 1, .layerCount = 2});
		End(*compute);
		const auto acquired = Submit(device, QueueType::Compute, *compute, {&released, 1});
		Expect(acquired.IsValid(), "Acquire exact mip/layer range");
		auto mipZero = MakeList(device, QueueType::Transfer);
		TextureBarrierFor(*mipZero,
			texture.Raw(),
			TextureLayout::TransferDst,
			TextureLayout::TransferSrc,
			transferWrite,
			transferRead);
		mipZero->CopyTextureToBuffer(texture.Raw(), output.Raw(), {.width = 4, .height = 4, .bufferOffset = 32});
		End(*mipZero);
		const auto copied = Submit(device, QueueType::Transfer, *mipZero);
		Expect(copied.IsValid(), "Unreleased mip remains on source family");
		device.GetCommandQueue(QueueType::Compute)->Wait(acquired);
		device.GetCommandQueue(QueueType::Transfer)->Wait(copied);
		std::array<uint8_t, 96> actual{};
		Expect(output->ReadData(actual.data(), actual.size()) == Result::Success, "Read all texture subresource bytes");
		for (size_t i = 0; i < actual.size(); ++i) {
			size_t inputIndex = i;
			if (i >= 32) {
				inputIndex -= 32;
			}
			Expect(actual[i] == input[inputIndex], "Independent texture subresources preserve their contents");
		}
		device.WaitIdle();
	}

	void TestRenderAttachmentOwnership(Device& device) {
		if (!DifferentFamilies(device, QueueType::Transfer, QueueType::Graphics)) {
			return;
		}
		auto texture = MakeTexture(device, TextureUsage::RenderTarget);
		const ResourceState colorWrite{PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite};
		auto transfer = MakeList(device, QueueType::Transfer);
		TextureBarrierFor(*transfer,
			texture.Raw(),
			TextureLayout::Undefined,
			TextureLayout::ColorAttachment,
			{},
			colorWrite,
			QueueType::Transfer,
			QueueType::Graphics);
		End(*transfer);
		const auto release = Submit(device, QueueType::Transfer, *transfer);
		Expect(release.IsValid(), "Release render-target texture from transfer family");
		RenderingInfo rendering{};
		rendering.colorAttachments.push_back({.target = texture->GetTextureView()});
		rendering.renderArea = {.width = 4, .height = 4};
		auto missing = MakeList(device, QueueType::Graphics);
		missing->BeginRendering(rendering);
		missing->EndRendering();
		End(*missing);
		ExpectRejected(device, QueueType::Graphics, *missing, "was released", {&release, 1});
		auto graphics = MakeList(device, QueueType::Graphics);
		TextureBarrierFor(*graphics,
			texture.Raw(),
			TextureLayout::Undefined,
			TextureLayout::ColorAttachment,
			{},
			colorWrite,
			QueueType::Transfer,
			QueueType::Graphics);
		graphics->BeginRendering(rendering);
		graphics->EndRendering();
		End(*graphics);
		const auto cleared = Submit(device, QueueType::Graphics, *graphics, {&release, 1});
		Expect(cleared.IsValid(), "Acquired render attachment can be cleared");
		device.GetCommandQueue(QueueType::Graphics)->Wait(cleared);
	}

	void TestRecordingValidation(Device& device) {
		auto buffer = MakeBuffer(device, 64, BufferUsage::TransferSrc | BufferUsage::TransferDst);
		auto list = MakeList(device, QueueType::Graphics);
		BarrierDesc barriers;
		barriers.buffers.push_back({.buffer = buffer.Raw(), .size = 64, .sourceQueue = QueueType::Graphics});
		ExpectRecordingError(
			[&] {
				list->Barrier(barriers);
			},
			"both sourceQueue");
		barriers.buffers[0].sourceQueue = static_cast<QueueType>(999);
		barriers.buffers[0].destinationQueue = QueueType::Graphics;
		ExpectRecordingError(
			[&] {
				list->Barrier(barriers);
			},
			"available");
		if (DifferentFamilies(device, QueueType::Graphics, QueueType::Transfer) &&
			DifferentFamilies(device, QueueType::Graphics, QueueType::Compute)) {
			barriers.buffers[0].sourceQueue = QueueType::Transfer;
			barriers.buffers[0].destinationQueue = QueueType::Compute;
			ExpectRecordingError(
				[&] {
					list->Barrier(barriers);
				},
				"source family");
		}
		barriers.buffers[0].sourceQueue.reset();
		barriers.buffers[0].destinationQueue.reset();
		barriers.buffers.push_back(barriers.buffers[0]);
		ExpectRecordingError(
			[&] {
				list->Barrier(barriers);
			},
			"Overlapping buffer");
		End(*list);
		const auto token = Submit(device, QueueType::Graphics, *list);
		Expect(token.IsValid(), "Recording errors skip invalid commands without poisoning the list");
		device.GetCommandQueue(QueueType::Graphics)->Wait(token);
	}
}

int main() {
	for (bool graphicsOnly : {false, true}) {
		DeviceDesc desc{};
		desc.enableValidationLayers = true;
		desc.enableDebug = true;
		desc.debugCallback = CaptureDebugMessage;
		desc.shaderDirectory = REALRHI_TEST_SHADER_DIRECTORY;
		std::unique_ptr<Device> device;
		if (graphicsOnly) {
			auto graphicsDevice = std::make_unique<GraphicsQueueTestDevice>();
			Expect(graphicsDevice->Init(desc) == Result::Success, "Initialize graphics-queue test device");
			device = std::move(graphicsDevice);
		}
		else {
			Expect(Device::Create(desc, device) == Result::Success, "Create ownership test device");
		}
		std::cout << "Queue families (graphics override=" << graphicsOnly
				  << "): graphics=" << device->GetCommandQueue(QueueType::Graphics)->GetQueueId().familyIndex
				  << ", compute=" << device->GetCommandQueue(QueueType::Compute)->GetQueueId().familyIndex
				  << ", transfer=" << device->GetCommandQueue(QueueType::Transfer)->GetQueueId().familyIndex << '\n';
		if (graphicsOnly) {
			Expect(device->GetCommandQueue(QueueType::Graphics) == device->GetCommandQueue(QueueType::Compute) &&
					device->GetCommandQueue(QueueType::Graphics) == device->GetCommandQueue(QueueType::Transfer),
				"Test subclass maps every role to the graphics queue");
		}
		TestBufferChain(*device);
		TestTextureChain(*device);
		TestRangesAndBatch(*device);
		TestTextureSubresources(*device);
		TestRenderAttachmentOwnership(*device);
		TestRecordingValidation(*device);
		Expect(g_ProgrammerErrors == g_ExpectedErrors, "No unexpected programmer errors");
	}
	std::cout << "QueueOwnershipTests passed\n";
}
