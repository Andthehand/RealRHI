#include "Device.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/TransientResourcePool.h"

#include <volk.h>

#include <SDL3/SDL.h>

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
	std::vector<uint32_t> s_BinaryWaits;
	std::vector<uint32_t> s_BinarySignals;
	PFN_vkQueuePresentKHR s_Present = nullptr;
	uint32_t s_Presents = 0;
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
		uint32_t binaryWaits = 0;
		uint32_t binarySignals = 0;
		for (uint32_t i = 0; i < infos[0].waitSemaphoreInfoCount; ++i) {
			binaryWaits += infos[0].pWaitSemaphoreInfos[i].value == 0;
		}
		for (uint32_t i = 0; i < infos[0].signalSemaphoreInfoCount; ++i) {
			binarySignals += infos[0].pSignalSemaphoreInfos[i].value == 0;
		}
		s_BinaryWaits.push_back(binaryWaits);
		s_BinarySignals.push_back(binarySignals);
		if (s_SubmitCalls == s_FailCall) {
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		return s_Submit(queue, count, infos, fence);
	}

	VKAPI_ATTR VkResult VKAPI_CALL CapturePresent(VkQueue queue, const VkPresentInfoKHR* info) {
		++s_Presents;
		return s_Present(queue, info);
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
		Expect(
			!completion.completions.empty() && completion.completions.size() == graph.GetDiagnostics().batches.size(),
			"Graph returns one completion token per submitted batch");
		const auto diagnostics = graph.GetDiagnostics();
		Expect(diagnostics.compiled && diagnostics.multiQueue && diagnostics.passes.size() == 4 &&
				diagnostics.passes[0].nativeQueue == device.GetCommandQueue(QueueType::Compute)->GetQueueId(),
			"Submitted graph diagnostics resolve actual native queue assignments");
		if (device.GetCommandQueue(QueueType::Compute)->GetQueueId().familyIndex != uploaded.queue.familyIndex) {
			Expect(!diagnostics.initialBarriers.empty() && !diagnostics.initialBarriers[0].consumers.empty(),
				"Diagnostics expose initial ownership releases and their consumer passes");
		}

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
		const auto compute = device.GetCommandQueue(QueueType::Compute)->GetQueueId();
		const auto transfer = device.GetCommandQueue(QueueType::Transfer)->GetQueueId();
		const auto graphics = device.GetGraphicsCommandQueue()->GetQueueId();
		const bool allAliased = compute == transfer && compute == graphics;
		if (allAliased) {
			Expect(s_WaitCounts == std::vector<uint32_t>{0} && completion.completions.size() == 1,
				"Aliasing roles batch on one native queue without self-waits");
		}
		else {
			size_t expectedBatches = 3;
			if (compute == transfer) {
				expectedBatches = 2;
			}
			Expect(s_WaitCounts.size() == expectedBatches && s_WaitCounts[0] == 0,
				"Independent queues do not gain waits through batching");
			if (expectedBatches == 3) {
				Expect(s_WaitCounts[1] == 0, "The second independent native queue also has no wait");
			}
			uint32_t expectedWaits = 0;
			if (compute != graphics) {
				++expectedWaits;
			}
			if (transfer != graphics && transfer != compute) {
				++expectedWaits;
			}
			Expect(s_WaitCounts.back() == expectedWaits,
				"Join waits only on foreign producer timelines; its own queue is already ordered");
		}
		completion.Wait(device);
		s_SubmitCalls = 0;
		s_WaitCounts.clear();
		s_FailCall = 2;
		if (allAliased) {
			s_FailCall = 1;
		}
		const auto prefixCount = s_FailCall - 1;
		s_ExpectedErrors += 2;
		Expect(graph.Submit(device, completion) == Result::Failed && completion.completions.size() == prefixCount,
			"A driver submission failure preserves tokens for the successfully queued prefix");
		vkQueueSubmit2 = s_Submit;
		s_FailCall = 0;
		completion.Wait(device);
		Expect(completion.IsComplete(device), "The submitted prefix can be waited after failure");
		Expect(graph.Submit(device, completion) == Result::Success,
			"A graph can be submitted again after a recoverable submission failure");
		completion.Wait(device);
	}

	void TestBatching(Device& device) {
		RenderGraph graph(Capture);
		graph.SetOptions({.enableGpuTimings = true});
		std::vector<CommandList*> recordings;
		bool timestamps = false;
		uint32_t callbacks = 0;
		for (uint32_t i = 0; i < 5; ++i) {
			graph.AddPass({.name = "Batched pass " + std::to_string(i)}, [&](CommandList& list) {
				recordings.push_back(&list);
				timestamps = list.SupportsTimestamps();
				++callbacks;
				list.WriteTimestamp("Callback marker");
			});
		}
		s_Submit = vkQueueSubmit2;
		vkQueueSubmit2 = CaptureSubmit;
		s_SubmitCalls = 0;
		s_FailCall = 0;
		s_WaitCounts.clear();
		RenderGraphSubmission completion;
		Expect(graph.Submit(device, completion) == Result::Success && callbacks == 5 && s_SubmitCalls == 1 &&
				completion.completions.size() == 1,
			"Five consecutive same-queue passes use one native submission");
		vkQueueSubmit2 = s_Submit;
		for (auto* recording : recordings) {
			Expect(recording == recordings[0], "Batched callbacks share a command list");
		}
		const auto diagnostics = graph.GetDiagnostics();
		Expect(diagnostics.batches.size() == 1 && diagnostics.batches[0].steps.size() == 5 &&
				diagnostics.batches[0].prerequisites.empty() &&
				graph.GetDebugDescription().find("Batch 0") != std::string::npos &&
				graph.GetDependencyGraphDot().find("batch 0") != std::string::npos,
			"Reports expose the native submission batch and its passes");
		graph.Reset();
		completion.Wait(device);
		if (timestamps) {
			for (uint32_t i = 0; i < 5; ++i) {
				bool found = false;
				for (const auto& timing : device.GetStatistics().gpuTimings) {
					if (timing.name == "RenderGraph: Batched pass " + std::to_string(i)) {
						found = timing.timing.sampleCount == 1;
					}
				}
				Expect(found, "Batching preserves separate per-pass timings despite callback markers");
			}
		}

		// Use an actually distinct native queue; role aliases are exercised above and
		// in the join test. Hardware with one queue cannot exercise foreign waits.
		const auto graphics = device.GetGraphicsCommandQueue()->GetQueueId();
		std::optional<QueueType> other;
		for (auto role : {QueueType::Compute, QueueType::Transfer}) {
			if (device.GetCommandQueue(role)->GetQueueId() != graphics) {
				other = role;
				break;
			}
		}
		if (!other) {
			return;
		}

		graph.Reset();
		const auto producer = graph.AddPass({.name = "Foreign producer", .queue = *other}, [](CommandList&) { });
		graph.AddPass({.name = "Earlier independent graphics"}, [](CommandList&) { });
		graph.AddPass({.name = "Graphics needs foreign work", .dependsOn = {producer}}, [](CommandList&) { });
		Expect(graph.Submit(device, completion) == Result::Success, "Submit a later foreign wait");
		const auto waiting = graph.GetDiagnostics();
		Expect(waiting.batches.size() == 3 && waiting.batches[1].prerequisites.empty() &&
				waiting.batches[2].prerequisites == std::vector<uint32_t>{0},
			"A later pass's wait is not moved before independent graphics work");
		completion.Wait(device);

		graph.Reset();
		const auto early = graph.AddPass({.name = "Early graphics producer"}, [](CommandList&) { });
		graph.AddPass({.name = "Later graphics work"}, [](CommandList&) { });
		graph.AddPass({.name = "Foreign consumer", .dependsOn = {early}, .queue = *other}, [](CommandList&) { });
		Expect(graph.Submit(device, completion) == Result::Success, "Submit early cross-queue signal");
		const auto signaling = graph.GetDiagnostics();
		Expect(signaling.batches.size() == 3 && signaling.batches[2].prerequisites == std::vector<uint32_t>{0},
			"A producer signals its foreign consumer without waiting for later same-queue work");
		completion.Wait(device);

		graph.Reset();
		const auto shared =
			graph.AddPass({.name = "Shared foreign prerequisite", .queue = *other}, [](CommandList&) { });
		graph.AddPass({.name = "First graphics consumer", .dependsOn = {shared}}, [](CommandList&) { });
		graph.AddPass({.name = "Second graphics consumer", .dependsOn = {shared}}, [](CommandList&) { });
		Expect(graph.Submit(device, completion) == Result::Success, "Submit consumers sharing the same wait");
		const auto sharedWait = graph.GetDiagnostics();
		Expect(sharedWait.batches.size() == 2 && sharedWait.batches[1].steps.size() == 2 &&
				sharedWait.batches[1].prerequisites == std::vector<uint32_t>{0},
			"Consumers with an established wait share one batch without duplicate dependency tokens");
		completion.Wait(device);

		graph.Reset();
		const auto first = graph.AddPass({.name = "First joined producer"}, [](CommandList&) { });
		const auto second = graph.AddPass({.name = "Second joined producer"}, [](CommandList&) { });
		graph.AddPass(
			{.name = "Consumer needs both", .dependsOn = {first, second}, .queue = *other}, [](CommandList&) { });
		Expect(graph.Submit(device, completion) == Result::Success, "Submit producers with a common consumer");
		const auto joined = graph.GetDiagnostics();
		Expect(joined.batches.size() == 2 && joined.batches[0].steps.size() == 2 &&
				joined.batches[1].prerequisites == std::vector<uint32_t>{0},
			"Producers can share a batch when their foreign consumer requires both to complete");
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

	void TestSwapchain(Device& device) {
		Ref<Window> window;
		Ref<Swapchain> swapchain;
		Expect(device.CreateWindow({.Title = "Graph swapchain synchronization", .Width = 64, .Height = 64}, window) ==
					Result::Success &&
				device.CreateSwapchain({.window = window.Raw()}, swapchain) == Result::Success,
			"Create graph swapchain");
		const std::array<uint32_t, 4> initial{41, 43, 47, 53};
		Ref<Buffer> upload;
		Expect(device.CreateBuffer({.size = sizeof(initial),
									   .usage = BufferUsage::TransferSrc,
									   .memoryUsage = MemoryUsage::CPUToGPU,
									   .initialData = initial.data()},
				   upload) == Result::Success,
			"Create graph frame input");
		auto readback = Readback(device, sizeof(initial));
		s_Submit = vkQueueSubmit2;
		s_Present = vkQueuePresentKHR;
		vkQueueSubmit2 = CaptureSubmit;
		vkQueuePresentKHR = CapturePresent;
		FrameContext stale;
		for (uint32_t iteration = 0; iteration < 8; ++iteration) {
			if (iteration == 4) {
				int count = 0;
				auto** windows = SDL_GetWindows(&count);
				for (int i = 0; i < count; ++i) {
					if (std::string_view(SDL_GetWindowTitle(windows[i])) == "Graph swapchain synchronization") {
						Expect(SDL_SetWindowSize(windows[i], 80, 72) && SDL_SyncWindow(windows[i]),
							"Resize graph presentation window");
					}
				}
				SDL_free(windows);
				SDL_PumpEvents();
			}
			const auto frame = swapchain->BeginFrame();
			Expect(swapchain->IsFrameReady(frame) && frame.acquisitionId != 0, "Acquire a fresh graph frame");
			if (iteration > 0) {
				Expect(frame.acquisitionId != stale.acquisitionId && !swapchain->IsFrameReady(stale),
					"Old acquisition IDs never become valid again");
			}
			if (iteration >= 4) {
				Expect(frame.width == 80 && frame.height == 72, "Graph frame follows resized swapchain extent");
			}
			if (iteration == 0) {
				RenderGraph unused(Capture);
				unused.ImportBackBuffer(*swapchain, frame);
				RenderGraphSubmission empty;
				++s_ExpectedErrors;
				Expect(unused.Submit(device, empty) == Result::InvalidParameter && empty.completions.empty() &&
						swapchain->IsFrameReady(frame),
					"An unwritten backbuffer fails before consuming image availability");
			}
			RenderGraph graph(Capture);
			graph.SetOptions({.cullUnusedPasses = true, .enableGpuTimings = true});
			const auto image = graph.ImportBackBuffer(*swapchain, frame);
			const auto source = graph.ImportBuffer(upload.Raw(), s_Read);
			const auto output =
				graph.ImportBuffer(readback.Raw(), {PipelineStage::AllCommands, ResourceAccess::MemoryWrite});
			const auto scratch = graph.CreateBuffer({.size = sizeof(initial), .usage = BufferUsage::TransferDst});
			const auto before = graph.AddPass(
				{.name = "Work before acquisition",
					.resources =
						{{source, s_Read}, {scratch, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}},
					.queue = QueueType::Transfer,
					.hasSideEffects = true},
				[&](CommandList& list) {
					list.CopyBuffer(upload.Raw(), graph.GetBuffer(scratch), {.size = sizeof(initial)});
				});
			const auto draw =
				graph.AddPass({.name = "Clear acquired image",
								  .resources =
									  {{image,
										  {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
										  TextureLayout::ColorAttachment,
										  RenderGraphAccess::Write}}},
					[frame](CommandList& list) {
						list.BeginRendering(
							{.colorAttachments =
									{{.target = frame.backBufferView,
										.loadOp = LoadOp::Clear,
										.clearColor = {0.1f, 0.2f, 0.4f, 1.0f}}},
								.renderArea = {.width = frame.width, .height = frame.height}});
						list.EndRendering();
					});
			graph.AddPass(
				{.name = "Independent late transfer",
					.resources =
						{{source, s_Read}, {output, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}},
					.queue = QueueType::Transfer},
				[&](CommandList& list) {
					list.CopyBuffer(upload.Raw(), readback.Raw(), {.size = sizeof(initial)});
				});
			if (iteration % 2 != 0) {
				graph.AddPass(
					{.name = "Return from transfer family",
						.resources =
							{{image, {PipelineStage::AllCommands, ResourceAccess::MemoryRead}, TextureLayout::Present}},
						.queue = QueueType::Transfer},
					[](CommandList&) { });
			}
			s_SubmitCalls = 0;
			s_FailCall = 0;
			s_Presents = 0;
			s_BinaryWaits.clear();
			s_BinarySignals.clear();
			RenderGraphSubmission completion;
			Expect(graph.Submit(device, completion) == Result::Success && s_Presents == 1,
				"Graph submits and presents an acquired image automatically");
			const auto info = graph.GetDiagnostics();
			const auto acquire = info.passes[draw.index].batchIndex;
			Expect(info.batches.back().presentation && info.batches[acquire].waitForAcquire &&
					info.passes[before.index].batchIndex < acquire &&
					!info.batches[info.passes[before.index].batchIndex].waitForAcquire &&
					info.presentationBarriers.textures.size() == 1 &&
					info.presentationBarriers.textures[0].releaseToPresent,
				"Diagnostics expose an acquire boundary and a final presentation join");
			for (size_t i = 0; i < info.batches.size(); ++i) {
				Expect(s_BinaryWaits[i] == uint32_t(i == acquire) &&
						s_BinarySignals[i] == uint32_t(i + 1 == info.batches.size()),
					"Image availability is waited exactly once and render completion signaled exactly once");
			}
			if (device.GetCommandQueue(QueueType::Transfer)->GetQueueId() !=
				device.GetGraphicsCommandQueue()->GetQueueId()) {
				Expect(!info.batches.back().prerequisites.empty(),
					"Presentation joins independent work on foreign queues");
			}
			if (iteration == 0) {
				++s_ExpectedErrors;
				Expect(graph.Submit(device, completion) == Result::InvalidParameter && s_Presents == 1,
					"Already submitted frames fail before duplicate presentation");
			}
			stale = frame;
		}
		device.WaitIdle();
		std::array<uint32_t, 4> values{};
		Expect(readback->ReadData(values.data(), sizeof(values)) == Result::Success && values == initial,
			"Frame completion covers independent late transfer data");
		RenderGraph invalid(Capture);
		++s_ExpectedErrors;
		Expect(invalid.ImportBackBuffer(*swapchain, stale).index == UINT32_MAX, "Stale frame import is rejected");

		const auto frame = swapchain->BeginFrame();
		RenderGraph failing(Capture);
		const auto image = failing.ImportBackBuffer(*swapchain, frame);
		failing.AddPass({.name = "Frame before failed present signal",
							.resources =
								{{image,
									{PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
									TextureLayout::ColorAttachment,
									RenderGraphAccess::Write}}},
			[frame](CommandList& list) {
				list.BeginRendering(
					{.colorAttachments = {{.target = frame.backBufferView, .loadOp = LoadOp::Clear}},
						.renderArea = {.width = frame.width, .height = frame.height}});
				list.EndRendering();
			});
		s_SubmitCalls = 0;
		s_Presents = 0;
		s_FailCall = 2;
		s_ExpectedErrors += 2;
		RenderGraphSubmission prefix;
		Expect(failing.Submit(device, prefix) == Result::Failed && prefix.completions.size() == 1 && s_Presents == 0,
			"Failed final synchronization preserves queued tokens and never presents an unsignaled semaphore");
		s_FailCall = 0;
		vkQueueSubmit2 = s_Submit;
		vkQueuePresentKHR = s_Present;
		failing.Reset();
		prefix.Wait(device);
		Expect(
			!swapchain->IsFrameReady(frame), "A partially submitted frame cannot consume its acquire semaphore again");
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
			graph.SetOptions({.cullUnusedPasses = true, .enableGpuTimings = true});
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
			graph.AddPass(
				{.name = "Repeated graphics queue read",
					.resources =
						{{shared, s_Read}, {output, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					list.CopyBuffer(graph.GetBuffer(shared), readback.Raw(), {.size = 16});
				});
			RenderGraphSubmission completion;
			Expect(graph.Submit(device, completion, &pool) == Result::Success,
				"Combine queue scheduling with resource pooling");
			const auto diagnostics = graph.GetDiagnostics();
			const auto graphicsBatch = diagnostics.passes[1].batchIndex;
			Expect(graphicsBatch == diagnostics.passes[2].batchIndex &&
					diagnostics.batches[graphicsBatch].steps.size() >= 2 &&
					completion.completions.size() == diagnostics.batches.size(),
				"Consecutive graphics reads share one submission and keep pooled resources alive");
			Expect(graph.GetExecutionPlan()[2].barriers.buffers.size() == 1 &&
					graph.GetExecutionPlan()[2].barriers.buffers[0].buffer == readback.Raw(),
				"Same-queue readers omit redundant source barriers across separate command lists");
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
									   .debugCallback = Capture,
									   .enableProfiling = true},
			   device) == RealRHI::Result::Success,
		"Create multi-queue graph test device");
	TestImportsAndFanOut(*device);
	TestIndependentAndFailedSubmissions(*device);
	TestBatching(*device);
	TestQueueValidation(*device);
	TestPoolIntegration(*device);
	TestSwapchain(*device);
	device->WaitIdle();
	device.reset();
	Expect(s_Errors == s_ExpectedErrors, "Only expected scheduler errors occur, including cleanup");
	std::cout << "RenderGraphQueueTests passed\n";
}
