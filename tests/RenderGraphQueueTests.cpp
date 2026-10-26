#include "Device.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/TransientResourcePool.h"

#include <volk.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
	using namespace RealRHI;
	uint32_t s_Errors = 0;
	uint32_t s_ExpectedErrors = 0;
	PFN_vkQueueSubmit2 s_Submit = nullptr;
	uint32_t s_SubmitCalls = 0;
	uint32_t s_FailCall = 0;
	std::vector<uint32_t> s_WaitCounts;
	constexpr ResourceState s_Read{PipelineStage::Transfer, ResourceAccess::TransferRead};
	constexpr ResourceState s_Write{PipelineStage::Transfer, ResourceAccess::TransferWrite};

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void Capture(const DebugMessage& message) {
		if (message.severity == DebugSeverity::Error) {
			++s_Errors;
			Expect(message.type == DebugMessageType::General &&
					(std::string_view(message.message).starts_with("RenderGraph:") ||
						std::string_view(message.message).starts_with("Failed to submit Vulkan")),
				"Only intentional scheduler errors are reported");
		}
		DefaultDebugCallback(message);
	}

	VKAPI_ATTR VkResult VKAPI_CALL CaptureSubmit(VkQueue queue,
		uint32_t count,
		const VkSubmitInfo2* infos,
		VkFence fence) {
		++s_SubmitCalls;
		Expect(count == 1, "Graph uses one queue submission per batch");
		s_WaitCounts.push_back(infos[0].waitSemaphoreInfoCount);
		if (s_SubmitCalls == s_FailCall) {
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		return s_Submit(queue, count, infos, fence);
	}

	Ref<Buffer> Readback(Device& device, uint64_t size) {
		Ref<Buffer> buffer;
		Expect(
			device.CreateBuffer({.size = size, .usage = BufferUsage::TransferDst, .memoryUsage = MemoryUsage::GPUToCPU},
				buffer) == Result::Success,
			"Create queue graph readback");
		return buffer;
	}

	void TestImportsAndFanOut(Device& device) {
		std::array<uint32_t, 64> initial;
		for (uint32_t i = 0; i < initial.size(); ++i) {
			initial[i] = i;
		}
		Ref<Buffer> upload;
		Ref<Buffer> data;
		Ref<Texture> texture;
		Expect(device.CreateBuffer({.size = sizeof(initial),
									   .usage = BufferUsage::TransferSrc,
									   .memoryUsage = MemoryUsage::CPUToGPU,
									   .initialData = initial.data()},
				   upload) == Result::Success &&
				device.CreateBuffer(
					{.size = sizeof(initial),
						.usage = BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc},
					data) == Result::Success,
			"Create external upload buffers");
		Expect(device.CreateTexture(
				   {.width = 4,
					   .height = 4,
					   .format = TextureFormat::RGBA8_UNorm,
					   .usage = TextureUsage::TransferDst | TextureUsage::TransferSrc | TextureUsage::ShaderResource},
				   texture) == Result::Success,
			"Create externally uploaded texture");
		Ref<CommandAllocator> allocator;
		Ref<CommandList> commands;
		Expect(device.CreateCommandAllocator(QueueType::Transfer, allocator) == Result::Success &&
				device.CreateCommandList(allocator.Raw(), commands) == Result::Success &&
				commands->Begin() == Result::Success,
			"Begin external transfer");
		BarrierDesc transition;
		transition.textures.push_back(
			{.texture = texture.Raw(),
				.after = s_Write,
				.oldLayout = TextureLayout::Undefined,
				.newLayout = TextureLayout::TransferDst});
		commands->Barrier(transition);
		commands->CopyBuffer(upload.Raw(), data.Raw(), {.size = sizeof(initial)});
		commands->CopyBufferToTexture(upload.Raw(), texture.Raw(), {.width = 4, .height = 4});
		Expect(commands->End() == Result::Success, "End external transfer");
		const auto uploaded = device.GetCommandQueue(QueueType::Transfer)->Submit(*commands);
		Expect(uploaded.IsValid(), "Submit external transfer without a CPU wait");
		commands.Reset();
		allocator.Reset();
		upload.Reset();

		Ref<Shader> shader;
		Ref<Pipeline> pipeline;
		Expect(device.CreateShader({.moduleName = "ownership_compute"}, shader) == Result::Success &&
				device.CreateComputePipeline({.shader = shader.Raw()}, pipeline) == Result::Success,
			"Create queue graph compute pipeline");
		std::array<Ref<Buffer>, 3> outputs;
		for (auto& output : outputs) {
			output = Readback(device, sizeof(initial));
		}
		auto images = Readback(device, 128);
		RenderGraph graph(Capture);
		const auto input = graph.ImportBuffer(data.Raw(), s_Write, 0, 0, QueueType::Transfer);
		const auto image =
			graph.ImportTexture(texture.Raw(), TextureLayout::TransferDst, s_Write, {}, QueueType::Transfer);
		const auto imageOutput = graph.ImportBuffer(images.Raw());
		std::array<RenderGraphResource, 3> outputHandles;
		for (size_t i = 0; i < outputs.size(); ++i) {
			outputHandles[i] = graph.ImportBuffer(outputs[i].Raw());
		}
		graph.AddPass(
			{.name = "Compute",
				.resources =
					{{input,
						 {PipelineStage::ComputeShader, ResourceAccess::ShaderRead | ResourceAccess::ShaderWrite},
						 TextureLayout::Undefined,
						 RenderGraphAccess::ReadWrite},
						{image, s_Read, TextureLayout::TransferSrc},
						{imageOutput, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}},
				.queue = QueueType::Compute},
			[&](CommandList& list) {
				list.BindPipeline(pipeline.Raw());
				list.UseResource(data.Raw());
				const uint32_t parameters[] = {data->GetDescriptorIndex(), 64};
				list.SetPushConstants(parameters, sizeof(parameters));
				list.DispatchCompute(1, 1, 1);
				list.CopyTextureToBuffer(texture.Raw(), images.Raw(), {.width = 4, .height = 4});
			});
		graph.AddPass({.name = "Graphics reader",
						  .resources =
							  {{input, s_Read},
								  {image, s_Read, TextureLayout::TransferSrc},
								  {outputHandles[0], s_Write, TextureLayout::Undefined, RenderGraphAccess::Write},
								  {imageOutput, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}},
						  .queue = QueueType::Graphics},
			[&](CommandList& list) {
				list.CopyBuffer(data.Raw(), outputs[0].Raw(), {.size = sizeof(initial)});
				list.CopyTextureToBuffer(texture.Raw(), images.Raw(), {.width = 4, .height = 4, .bufferOffset = 64});
			});
		graph.AddPass(
			{.name = "Compute reader",
				.resources =
					{{input, s_Read}, {outputHandles[1], s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}},
				.queue = QueueType::Compute},
			[&](CommandList& list) {
				list.CopyBuffer(data.Raw(), outputs[1].Raw(), {.size = sizeof(initial)});
			});
		graph.AddPass(
			{.name = "Transfer reader",
				.resources =
					{{input, s_Read}, {outputHandles[2], s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}},
				.queue = QueueType::Transfer},
			[&](CommandList& list) {
				list.CopyBuffer(data.Raw(), outputs[2].Raw(), {.size = sizeof(initial)});
			});
		graph.SetFinalState(image, {PipelineStage::AllCommands, ResourceAccess::MemoryRead}, TextureLayout::ShaderRead);
		RenderGraphSubmission completion;
		const std::array<SubmissionToken, 1> waits{uploaded};
		Expect(graph.Submit(device, completion, nullptr, waits) == Result::Success,
			"Schedule imports, compute, and read/read ownership handoffs");
		Expect(completion.completions.size() >= 4, "Graph returns completion tokens for all pass submissions");
		const auto& plan = graph.GetExecutionPlan();
		if (device.GetCommandQueue(QueueType::Compute)->GetQueueId().familyIndex !=
			device.GetGraphicsCommandQueue()->GetQueueId().familyIndex) {
			Expect(!plan[0].afterBarriers.buffers.empty() &&
					plan[0].afterBarriers.buffers[0].sourceQueue == QueueType::Compute &&
					plan[0].afterBarriers.buffers[0].destinationQueue == QueueType::Graphics,
				"Compiled plan exposes matching release/acquire endpoints");
		}
		graph.Reset();
		data.Reset();
		shader.Reset();
		pipeline.Reset();
		device.ClearShaderAndPipelineCaches();
		completion.Wait(device);
		Expect(completion.IsComplete(device), "Waiting retires all graph submissions");
		std::array<uint32_t, 64> values{};
		for (auto& output : outputs) {
			Expect(output->ReadData(values.data(), sizeof(values)) == Result::Success, "Read cross-queue buffer");
			for (uint32_t i = 0; i < initial.size(); ++i) {
				Expect(values[i] == initial[i] + 3,
					"Every reader observes the compute write across repeated ownership handoffs");
			}
		}
		std::array<uint32_t, 32> imageValues{};
		Expect(images->ReadData(imageValues.data(), sizeof(imageValues)) == Result::Success,
			"Read transferred image data");
		for (uint32_t i = 0; i < 16; ++i) {
			Expect(imageValues[i] == initial[i] && imageValues[i + 16] == initial[i],
				"Read/read image handoffs preserve every pixel");
		}

		// Consume an exported image with its declared final state and current owner.
		RenderGraph next(Capture);
		auto readback = Readback(device, 64);
		const auto exported = next.ImportTexture(texture.Raw(),
			TextureLayout::ShaderRead,
			{PipelineStage::AllCommands, ResourceAccess::MemoryRead},
			{},
			QueueType::Graphics);
		const auto destination = next.ImportBuffer(readback.Raw());
		next.AddPass({.name = "Consume export",
						 .resources =
							 {{exported, s_Read, TextureLayout::TransferSrc},
								 {destination, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}},
						 .queue = QueueType::Transfer},
			[&](CommandList& list) {
				list.CopyTextureToBuffer(texture.Raw(), readback.Raw(), {.width = 4, .height = 4});
			});
		RenderGraphSubmission consumed;
		Expect(next.Submit(device, consumed, nullptr, completion.completions) == Result::Success,
			"Another graph acquires an exported image");
		next.Reset();
		texture.Reset();
		consumed.Wait(device);
		Expect(readback->ReadData(imageValues.data(), 64) == Result::Success, "Read exported image");
		for (uint32_t i = 0; i < 16; ++i) {
			Expect(imageValues[i] == initial[i], "Export state transitions preserve image contents");
		}
	}

	void TestIndependentAndFailedSubmissions(Device& device) {
		RenderGraph graph(Capture);
		uint32_t recorded = 0;
		const auto a = graph.AddPass({.name = "Independent compute", .queue = QueueType::Compute}, [&](CommandList&) {
			++recorded;
		});
		const auto b = graph.AddPass({.name = "Independent transfer", .queue = QueueType::Transfer}, [&](CommandList&) {
			++recorded;
		});
		graph.AddPass({.name = "Join", .dependsOn = {a, b}}, [&](CommandList&) {
			++recorded;
		});
		s_Submit = vkQueueSubmit2;
		vkQueueSubmit2 = CaptureSubmit;
		s_SubmitCalls = 0;
		s_FailCall = 0;
		s_WaitCounts.clear();
		RenderGraphSubmission completion;
		Expect(graph.Submit(device, completion) == Result::Success && recorded == 3,
			"Record and submit independent queues");
		Expect(s_WaitCounts.size() == 3 && s_WaitCounts[0] == 0 && s_WaitCounts[1] == 0,
			"Independent queue submissions do not wait on each other");
		uint32_t expectedWaits = 2;
		if (device.GetCommandQueue(QueueType::Compute)->GetQueueId() ==
			device.GetCommandQueue(QueueType::Transfer)->GetQueueId()) {
			expectedWaits = 1;
		}
		Expect(s_WaitCounts[2] == expectedWaits,
			"Join waits on all distinct producer timelines, including aliasing queue roles");
		completion.Wait(device);
		s_SubmitCalls = 0;
		s_WaitCounts.clear();
		s_FailCall = 2;
		s_ExpectedErrors += 2;
		Expect(graph.Submit(device, completion) == Result::Failed && completion.completions.size() == 1,
			"A driver submission failure preserves tokens for the successfully queued prefix");
		vkQueueSubmit2 = s_Submit;
		s_FailCall = 0;
		completion.Wait(device);
		Expect(completion.IsComplete(device), "The submitted prefix can be waited after failure");
		Expect(graph.Submit(device, completion) == Result::Success,
			"A graph can be submitted again after a recoverable submission failure");
		completion.Wait(device);
	}

	void TestQueueValidation(Device& device) {
		RenderGraph graph(Capture);
		RenderGraphSubmission completion;
		uint32_t callbacks = 0;
		graph.AddPass({.name = "Unavailable queue", .queue = static_cast<QueueType>(99)}, [&](CommandList&) {
			++callbacks;
		});
		++s_ExpectedErrors;
		Expect(graph.Submit(device, completion) == Result::InvalidParameter && callbacks == 0 &&
				completion.completions.empty(),
			"Invalid queues fail before callbacks or submissions");
		graph.Reset();
		const auto data = graph.CreateBuffer({.size = 16, .usage = BufferUsage::Storage});
		graph.AddPass({.name = "Compute on transfer",
						  .resources =
							  {{data,
								  {PipelineStage::ComputeShader, ResourceAccess::ShaderWrite},
								  TextureLayout::Undefined,
								  RenderGraphAccess::Write}},
						  .queue = QueueType::Transfer},
			[&](CommandList&) {
				++callbacks;
			});
		++s_ExpectedErrors;
		Expect(graph.Submit(device, completion) == Result::InvalidParameter && callbacks == 0,
			"Queue-incompatible stages fail before allocation");
		graph.Reset();
		graph.AddPass({.name = "No work"}, [](CommandList&) { });
		const std::array<SubmissionToken, 1> invalid{{}};
		++s_ExpectedErrors;
		Expect(graph.Submit(device, completion, nullptr, invalid) == Result::InvalidParameter &&
				completion.completions.empty(),
			"Invalid external waits fail before scheduling");
	}

	void TestPoolIntegration(Device& device) {
		TransientResourcePool pool(device);
		const std::array<uint32_t, 8> initial{2, 3, 5, 7, 11, 13, 17, 19};
		Ref<Buffer> upload;
		Expect(device.CreateBuffer({.size = sizeof(initial),
									   .usage = BufferUsage::TransferSrc,
									   .memoryUsage = MemoryUsage::CPUToGPU,
									   .initialData = initial.data()},
				   upload) == Result::Success,
			"Create pooled graph upload");
		auto readback = Readback(device, 16);
		for (uint32_t frame = 0; frame < 3; ++frame) {
			RenderGraph graph(Capture);
			const auto source = graph.ImportBuffer(upload.Raw(), s_Read);
			const auto shared =
				graph.CreateBuffer({.size = 16, .usage = BufferUsage::TransferSrc | BufferUsage::TransferDst});
			const auto local = graph.CreateBuffer({.size = sizeof(initial), .usage = BufferUsage::TransferDst});
			const auto output = graph.ImportBuffer(readback.Raw(), s_Write);
			graph.AddPass({.name = "Compute queue copy",
							  .resources =
								  {{source, s_Read},
									  {shared, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write},
									  {local, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}},
							  .queue = QueueType::Compute},
				[&](CommandList& list) {
					list.CopyBuffer(upload.Raw(), graph.GetBuffer(shared), {.size = 16});
					list.CopyBuffer(upload.Raw(), graph.GetBuffer(local), {.size = sizeof(initial)});
				});
			graph.AddPass(
				{.name = "Graphics queue read",
					.resources =
						{{shared, s_Read}, {output, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					list.CopyBuffer(graph.GetBuffer(shared), readback.Raw(), {.size = 16});
				});
			RenderGraphSubmission completion;
			Expect(graph.Submit(device, completion, &pool) == Result::Success,
				"Combine queue scheduling with resource pooling");
			graph.Reset();
			completion.Wait(device);
			std::array<uint32_t, 4> values{};
			Expect(readback->ReadData(values.data(), sizeof(values)) == Result::Success, "Read pooled graph output");
			for (uint32_t i = 0; i < values.size(); ++i) {
				Expect(values[i] == initial[i],
					"Cross-queue direct allocations and queue-local pooled allocations remain valid");
			}
		}
		uint64_t eligible = 1;
		if (device.GetCommandQueue(QueueType::Compute)->GetQueueId() ==
			device.GetGraphicsCommandQueue()->GetQueueId()) {
			eligible = 2;
		}
		const auto statistics = pool.GetStatistics();
		Expect(statistics.bufferCount == eligible && statistics.bufferAllocations == eligible &&
				statistics.bufferReuses == eligible * 2,
			"Only allocations confined to one native queue are cached and safely reused");
	}
}

int main() {
	std::unique_ptr<RealRHI::Device> device;
	Expect(RealRHI::Device::Create({.applicationName = "RenderGraphQueueTests",
									   .shaderDirectory = REALRHI_TEST_SHADER_DIRECTORY,
									   .enableDebug = true,
									   .enableValidationLayers = true,
									   .debugCallback = Capture},
			   device) == RealRHI::Result::Success,
		"Create multi-queue graph test device");
	TestImportsAndFanOut(*device);
	TestIndependentAndFailedSubmissions(*device);
	TestQueueValidation(*device);
	TestPoolIntegration(*device);
	device->WaitIdle();
	device.reset();
	Expect(s_Errors == s_ExpectedErrors, "Only expected scheduler errors occur, including cleanup");
	std::cout << "RenderGraphQueueTests passed\n";
}
