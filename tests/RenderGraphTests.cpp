#include "Device.h"
#include "RenderGraph/RenderGraph.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
	using namespace RealRHI;
	uint32_t s_ExpectedErrors = 0;
	uint32_t s_Errors = 0;
	bool s_AllocationErrorExpected = false;
	constexpr ResourceState s_TransferRead{PipelineStage::Transfer, ResourceAccess::TransferRead};
	constexpr ResourceState s_TransferWrite{PipelineStage::Transfer, ResourceAccess::TransferWrite};
	constexpr ResourceState s_ComputeRead{PipelineStage::ComputeShader, ResourceAccess::ShaderRead};
	constexpr ResourceState s_ComputeWrite{PipelineStage::ComputeShader, ResourceAccess::ShaderWrite};

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
						(s_AllocationErrorExpected && std::string_view(message.message).starts_with("CreateTexture:"))),
				"Only intentional graph recording errors are reported");
		}
		DefaultDebugCallback(message);
	}

	void EmptyPass(CommandList&) { }

	void ExpectInvalid(RenderGraph& graph, const char* diagnostic) {
		std::string error;
		Expect(!graph.Compile(error) && error.find(diagnostic) != std::string::npos,
			"Invalid graph explains the violated precondition");
		Expect(graph.GetExecutionPlan().empty() && graph.GetFinalBarriers().textures.empty() &&
				graph.GetFinalBarriers().buffers.empty(),
			"Failed compilation publishes no partial execution plan");
	}

	void TestPlanning(Buffer* buffer, Texture* texture) {
		RenderGraph graph(Capture);
		const auto data = graph.ImportBuffer(buffer);
		const auto first = graph.AddPass({.name = "Unrelated first"}, EmptyPass);
		graph.AddPass({.name = "Write",
						  .resources = {{data, s_ComputeWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			EmptyPass);
		graph.AddPass({.name = "Compute reader", .resources = {{data, s_ComputeRead}}}, EmptyPass);
		const ResourceState fragmentRead{PipelineStage::FragmentShader, ResourceAccess::ShaderRead};
		const auto last = graph.AddPass({.name = "Fragment reader", .resources = {{data, fragmentRead}}}, EmptyPass);
		graph.AddDependency(first, last);
		graph.SetFinalState(data, s_TransferWrite);
		std::string error;
		Expect(graph.Compile(error), "Compile a resource chain with reordered independent work");
		const auto& plan = graph.GetExecutionPlan();
		Expect(plan.size() == 4 && plan[0].name == "Write" && plan[1].name == "Compute reader" &&
				plan[2].name == "Fragment reader" && plan[3].name == "Unrelated first",
			"Explicit dependencies reorder independent passes while preserving resource versions");
		for (size_t i : {size_t(1), size_t(2)}) {
			Expect(plan[i].barriers.buffers.size() == 1 &&
					(static_cast<uint64_t>(plan[i].barriers.buffers[0].before.access) &
						static_cast<uint64_t>(ResourceAccess::ShaderWrite)) != 0,
				"Each reader stage receives visibility from the producer");
		}
		const auto& final = graph.GetFinalBarriers().buffers;
		Expect(final.size() == 1 &&
				(static_cast<uint64_t>(final[0].before.stages) &
					static_cast<uint64_t>(PipelineStage::ComputeShader | PipelineStage::FragmentShader)) ==
					static_cast<uint64_t>(PipelineStage::ComputeShader | PipelineStage::FragmentShader),
			"A later write waits for all preceding reader stages");

		graph.Reset();
		const auto left = graph.ImportBuffer(buffer, {}, 0, 16);
		const auto right = graph.ImportBuffer(buffer, {}, 16, 16);
		const auto before = graph.AddPass(
			{.name = "Left",
				.resources = {{left, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			EmptyPass);
		const auto after = graph.AddPass(
			{.name = "Right",
				.resources = {{right, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			EmptyPass);
		graph.AddDependency(before, after);
		Expect(graph.Compile(error) && graph.GetExecutionPlan()[0].name == "Right",
			"Disjoint buffer ranges do not impose false dependencies");
		Expect(graph.GetExecutionPlan()[0].barriers.buffers[0].offset == 16 &&
				graph.GetExecutionPlan()[0].barriers.buffers[0].size == 16,
			"Buffer barriers preserve imported ranges");

		graph.Reset();
		const auto mip0 = graph.ImportTexture(texture, TextureLayout::Undefined, {}, {0, 1, 0, 1});
		const auto mip1 = graph.ImportTexture(texture, TextureLayout::Undefined, {}, {1, 1, 0, 1});
		const auto pass0 = graph.AddPass(
			{.name = "Mip zero",
				.resources = {{mip0, s_TransferWrite, TextureLayout::TransferDst, RenderGraphAccess::Write}}},
			EmptyPass);
		const auto pass1 = graph.AddPass(
			{.name = "Mip one",
				.resources = {{mip1, s_TransferWrite, TextureLayout::TransferDst, RenderGraphAccess::Write}}},
			EmptyPass);
		graph.AddDependency(pass0, pass1);
		graph.SetFinalState(mip0, s_TransferRead, TextureLayout::TransferSrc);
		Expect(graph.Compile(error) && graph.GetExecutionPlan()[0].name == "Mip one",
			"Separate texture mip ranges can execute independently");
		Expect(graph.GetExecutionPlan()[0].barriers.textures[0].subresources.baseMipLevel == 1 &&
				graph.GetFinalBarriers().textures[0].newLayout == TextureLayout::TransferSrc,
			"Imported texture ranges and explicit export layouts reach the compiled plan");

		graph.Reset();
		const auto image = graph.ImportTexture(texture, TextureLayout::ShaderRead, s_ComputeRead);
		const auto sample = graph.AddPass(
			{.name = "Sample", .resources = {{image, s_ComputeRead, TextureLayout::ShaderRead}}}, EmptyPass);
		const auto copy = graph.AddPass(
			{.name = "Copy", .resources = {{image, s_TransferRead, TextureLayout::TransferSrc}}}, EmptyPass);
		graph.AddDependency(sample, copy);
		ExpectInvalid(graph, "cycle");
	}

	void TestDiagnosticsAndCulling(Device& device, Buffer* buffer) {
		RenderGraph graph(Capture);
		Expect(!graph.GetDiagnostics().compiled, "Uncompiled graph exposes no stale diagnostics");
		const auto data = graph.CreateBuffer({.size = 16, .usage = BufferUsage::Storage, .debugName = "Working data"});
		const auto discarded = graph.CreateTexture(
			{.width = UINT32_MAX,
				.height = 4,
				.format = TextureFormat::RGBA8_UNorm,
				.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget,
				.debugName = "Unused oversized target"});
		uint32_t callbacks = 0;
		graph.AddPass({.name = "Unused branch",
						  .resources =
							  {{discarded,
								  {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
								  TextureLayout::ColorAttachment,
								  RenderGraphAccess::Write}}},
			[&](CommandList&) {
				++callbacks;
			});
		const auto write = graph.AddPass(
			{.name = "Write \"data\"\npass",
				.resources = {{data, s_ComputeWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			EmptyPass);
		const auto reader = graph.AddPass({.name = "First read", .resources = {{data, s_ComputeRead}}}, EmptyPass);
		const auto repeated = graph.AddPass(
			{.name = "Repeated read", .resources = {{data, s_ComputeRead}}, .dependsOn = {reader}}, EmptyPass);
		const ResourceState fragment{PipelineStage::FragmentShader, ResourceAccess::ShaderRead};
		const auto newStage =
			graph.AddPass({.name = "New stage", .resources = {{data, fragment}}, .dependsOn = {repeated}}, EmptyPass);
		graph.AddPass(
			{.name = "Repeated new stage", .resources = {{data, fragment}}, .dependsOn = {newStage}}, EmptyPass);
		graph.SetFinalState(data, s_TransferRead);
		const auto output = graph.ImportBuffer(buffer);
		const auto external = graph.AddPass(
			{.name = "Imported write",
				.resources = {{output, s_ComputeWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			EmptyPass);
		const auto prerequisite = graph.AddPass({.name = "Required side work"}, EmptyPass);
		const auto effect = graph.AddPass(
			{.name = "External side effect", .dependsOn = {prerequisite}, .hasSideEffects = true}, [&](CommandList&) {
				++callbacks;
			});
		graph.AddPass({.name = "Unreferenced callback"}, [&](CommandList&) {
			callbacks += 100;
		});
		graph.SetOptions({.cullUnusedPasses = true, .enableGpuTimings = true});
		std::string error;
		Expect(graph.Compile(error), "Compile optional culling with exports, imports, and external effects");
		const auto info = graph.GetDiagnostics();
		Expect(info.compiled && !info.multiQueue && info.passes.size() == 10 && graph.GetExecutionPlan().size() == 8,
			"Diagnostics distinguish all declared passes from scheduled passes");
		Expect(info.passes[0].executionIndex == UINT32_MAX && info.passes[9].executionIndex == UINT32_MAX &&
				info.passes[external.index].executionIndex != UINT32_MAX &&
				info.passes[effect.index].prerequisites[0].index == prerequisite.index,
			"Culling keeps observable roots and explicit prerequisites");
		Expect(info.resources[data.index].name == "Working data" && info.resources[data.index].firstUse == 0 &&
				info.resources[data.index].lastUse == 4 && info.resources[data.index].exported &&
				info.resources[discarded.index].firstUse == UINT32_MAX && info.resources[output.index].imported,
			"Resource diagnostics describe names, surviving lifetimes, unused branches, and imports");
		const auto& plan = graph.GetExecutionPlan();
		Expect(plan[1].barriers.buffers.size() == 1 && plan[2].barriers.buffers.empty() &&
				plan[3].barriers.buffers.size() == 1 && plan[4].barriers.buffers.empty(),
			"Repeated readers reuse established visibility while new reader stages still synchronize");
		const auto description = graph.GetDebugDescription();
		const auto dot = graph.GetDependencyGraphDot();
		Expect(description.find("2 culled") != std::string::npos &&
				description.find("Working data") != std::string::npos &&
				description.find("ShaderWrite") != std::string::npos &&
				description.find("prerequisites:") != std::string::npos &&
				dot.find(R"(Write \"data\"\npass)") != std::string::npos && dot.find("p1 -> p2") != std::string::npos,
			"Human-readable and DOT reports expose barriers and escaped dependency names");
		Ref<CommandAllocator> allocator;
		Ref<CommandList> commands;
		Expect(device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success &&
				device.CreateCommandList(allocator.Raw(), commands) == Result::Success &&
				commands->Begin() == Result::Success,
			"Begin culled graph execution");
		Expect(graph.Execute(device, *commands) == Result::Success && callbacks == 1,
			"Culled callbacks and oversized dead allocations are skipped");
		Expect(commands->End() == Result::Success, "End culled graph execution");
		auto completion = device.GetGraphicsCommandQueue()->Submit(*commands);
		Expect(completion.IsValid(), "Submit optimized graph");
		device.GetGraphicsCommandQueue()->Wait(completion);
		if (commands->SupportsTimestamps()) {
			bool measured = false;
			for (const auto& timing : device.GetStatistics().gpuTimings) {
				if (timing.name == "RenderGraph: Imported write") {
					measured = timing.timing.sampleCount == 1 && timing.queue == commands->GetQueueId();
				}
			}
			Expect(measured, "Completed graph passes publish named GPU timing statistics");
		}
		Expect(graph.GetDiagnostics().passes[write.index].nativeQueue == commands->GetQueueId(),
			"Execute diagnostics identify the actual caller queue");
		graph.AddDependency(write, effect);
		Expect(!graph.GetDiagnostics().compiled && graph.GetDiagnostics().passes.empty(),
			"Mutation invalidates diagnostic snapshots");
		graph.Reset();
		graph.AddPass({.name = "Still optional"}, EmptyPass);
		Expect(graph.Compile(error) && graph.GetExecutionPlan().empty(), "Reset preserves culling options");
		graph.SetOptions({});
		Expect(graph.Compile(error) && graph.GetExecutionPlan().size() == 1,
			"Culling can be disabled without losing passes");
		graph.SetOptions({.cullUnusedPasses = true});
		const auto other = graph.AddPass({.name = "Cycle"}, EmptyPass);
		graph.AddDependency(other, other);
		ExpectInvalid(graph, "cycle");
		Expect(!graph.GetDiagnostics().compiled, "Even culled branches cannot hide dependency cycles");
		graph.Reset();
		graph.SetOptions({.enableGpuTimings = true});
		callbacks = 0;
		for (uint32_t i = 0; i < 70; ++i) {
			graph.AddPass({.name = "Capacity pass " + std::to_string(i)}, [&](CommandList&) {
				++callbacks;
			});
		}
		Expect(commands->Begin() == Result::Success && graph.Execute(device, *commands) == Result::Success &&
				callbacks == 70 && commands->End() == Result::Success,
			"Profiling capacity never prevents graph callbacks from executing");
		completion = device.GetGraphicsCommandQueue()->Submit(*commands);
		Expect(completion.IsValid(), "Submit graph larger than timestamp capacity");
		device.GetGraphicsCommandQueue()->Wait(completion);
		if (commands->SupportsTimestamps()) {
			std::vector<GpuTimestamp> timestamps;
			Expect(commands->GetTimestampResults(timestamps) == Result::Success && timestamps.size() == 128,
				"Large graph profiles complete pairs without overflowing the timestamp pool");
		}
	}

	void TestInvalidGraphs(Buffer* buffer, Texture* texture) {
		RenderGraph graph(Capture);
		graph.ImportBuffer(nullptr);
		ExpectInvalid(graph, "non-null");
		graph.Reset();
		graph.ImportBuffer(buffer, {}, 31, UINT64_MAX);
		ExpectInvalid(graph, "buffer range");
		graph.Reset();
		graph.ImportBuffer(buffer, {}, 0, 17);
		graph.ImportBuffer(buffer, {}, 16, 16);
		ExpectInvalid(graph, "overlaps");
		graph.Reset();
		graph.ImportTexture(texture, TextureLayout::Undefined, {}, {0, 2, 0, 1});
		graph.ImportTexture(texture, TextureLayout::Undefined, {}, {1, 1, 0, 1});
		ExpectInvalid(graph, "overlaps");
		graph.Reset();
		auto data = graph.ImportBuffer(buffer);
		graph.AddPass({.name = "Duplicate", .resources = {{data, s_ComputeRead}, {data, s_ComputeRead}}}, EmptyPass);
		ExpectInvalid(graph, "once");
		graph.Reset();
		graph.AddPass({.name = "Stale", .resources = {{data, s_ComputeRead}}}, EmptyPass);
		ExpectInvalid(graph, "handle");
		graph.Reset();
		RenderGraph other;
		data = other.ImportBuffer(buffer);
		graph.AddPass({.name = "Foreign", .resources = {{data, s_ComputeRead}}}, EmptyPass);
		ExpectInvalid(graph, "handle");
		graph.Reset();
		data = graph.ImportBuffer(buffer);
		graph.AddPass({.name = "Wrong mode", .resources = {{data, s_ComputeWrite}}}, EmptyPass);
		ExpectInvalid(graph, "access mode");
		graph.Reset();
		const auto image = graph.ImportTexture(texture, TextureLayout::Undefined);
		graph.AddPass(
			{.name = "Uninitialized", .resources = {{image, s_ComputeRead, TextureLayout::ShaderRead}}}, EmptyPass);
		ExpectInvalid(graph, "written before reading");
		graph.Reset();
		const auto unused = graph.ImportTexture(texture, TextureLayout::Undefined);
		graph.SetFinalState(unused, s_TransferRead, TextureLayout::TransferSrc);
		ExpectInvalid(graph, "producing pass");
		graph.Reset();
		graph.AddPass({.name = "No callback"}, {});
		ExpectInvalid(graph, "callback");
		graph.Reset();
		const auto a = graph.AddPass({.name = "A"}, EmptyPass);
		const auto b = graph.AddPass({.name = "B", .dependsOn = {a}}, EmptyPass);
		graph.AddDependency(a, b);
		ExpectInvalid(graph, "cycle");
		graph.Reset();
		graph.AddDependency(a, b);
		ExpectInvalid(graph, "invalid pass");
	}

	void TestCreatedValidation(Device& device) {
		Ref<CommandAllocator> allocator;
		Ref<CommandList> commands;
		Expect(device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success &&
				device.CreateCommandList(allocator.Raw(), commands) == Result::Success &&
				commands->Begin() == Result::Success,
			"Begin execution validation commands");
		RenderGraph graph(Capture);
		auto data = graph.CreateBuffer({.size = 16, .usage = BufferUsage::Storage});
		graph.AddPass({.name = "Uninitialized buffer", .resources = {{data, s_ComputeRead}}}, EmptyPass);
		ExpectInvalid(graph, "written before reading");
		graph.Reset();
		uint32_t initial = 1;
		graph.CreateBuffer(
			{.size = 4,
				.usage = BufferUsage::TransferSrc,
				.memoryUsage = MemoryUsage::CPUToGPU,
				.initialData = &initial});
		ExpectInvalid(graph, "initialData");
		graph.Reset();
		graph.CreateTexture(
			{.width = 0, .height = 8, .format = TextureFormat::RGBA8_UNorm, .usage = TextureUsage::RenderTarget});
		ExpectInvalid(graph, "nonzero dimensions");
		graph.Reset();
		const auto baseline = device.GetStatistics();
		data = graph.CreateBuffer({.size = 16, .usage = BufferUsage::TransferDst});
		graph.CreateTexture(
			{.width = device.GetCapabilities().limits.maxTextureDimension2D + 1,
				.height = 1,
				.format = TextureFormat::RGBA8_UNorm,
				.usage = TextureUsage::RenderTarget});
		uint32_t callbacks = 0;
		graph.AddPass({.name = "No partial execution",
						  .resources = {{data, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			[&](CommandList&) {
				++callbacks;
			});
		++s_ExpectedErrors;
		s_AllocationErrorExpected = true;
		Expect(graph.Execute(device, *commands) == Result::InvalidParameter,
			"Device allocation validation propagates to the caller");
		s_AllocationErrorExpected = false;
		Expect(device.GetStatistics().buffers.liveCount == baseline.buffers.liveCount &&
				device.GetStatistics().textures.liveCount == baseline.textures.liveCount && callbacks == 0,
			"A failed allocation batch releases prior allocations and records no callbacks");
		++s_ExpectedErrors;
		Expect(graph.GetBuffer(data) == nullptr, "Allocation failure publishes no partial resources");
		graph.Reset();
		data = graph.CreateBuffer({.size = 16, .usage = BufferUsage::TransferDst});
		graph.AddPass({.name = "Recovered",
						  .resources = {{data, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			EmptyPass);
		Expect(graph.Execute(device, *commands) == Result::Success,
			"Graph can execute after a previous failure and reset");
		++s_ExpectedErrors;
		Expect(graph.GetTexture(data) == nullptr, "Resource resolution validates buffer versus texture kind");
		graph.Reset();
		Expect(commands->End() == Result::Success && commands->Reset() == Result::Success,
			"Release recorded resources without submitting");
		Expect(device.GetStatistics().buffers.liveCount == baseline.buffers.liveCount,
			"Reset releases allocations after the recording is reset");
	}

	void TestExecutionsInFlight(Device& device) {
		const auto baseline = device.GetStatistics();
		const std::array<uint32_t, 4> initial{3, 5, 7, 11};
		Ref<Buffer> upload;
		Expect(device.CreateBuffer({.size = sizeof(initial),
									   .usage = BufferUsage::TransferSrc,
									   .memoryUsage = MemoryUsage::CPUToGPU,
									   .initialData = initial.data()},
				   upload) == Result::Success,
			"Create in-flight graph upload");
		RenderGraph graph(Capture);
		const auto source = graph.ImportBuffer(upload.Raw(), s_TransferRead);
		const auto data =
			graph.CreateBuffer({.size = sizeof(initial), .usage = BufferUsage::TransferDst | BufferUsage::TransferSrc});
		const auto output = graph.CreateBuffer(
			{.size = sizeof(initial), .usage = BufferUsage::TransferDst, .memoryUsage = MemoryUsage::GPUToCPU});
		graph.AddPass({.name = "Initialize",
						  .resources =
							  {{source, s_TransferRead},
								  {data, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			[&](CommandList& list) {
				list.CopyBuffer(graph.GetBuffer(source), graph.GetBuffer(data), {.size = sizeof(initial)});
			});
		graph.AddPass({.name = "Readback",
						  .resources =
							  {{data, s_TransferRead},
								  {output, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			[&](CommandList& list) {
				list.CopyBuffer(graph.GetBuffer(data), graph.GetBuffer(output), {.size = sizeof(initial)});
			});
		std::array<Ref<CommandAllocator>, 2> allocators;
		std::array<Ref<CommandList>, 2> commands;
		std::array<Ref<Buffer>, 2> readbacks;
		std::array<SubmissionToken, 2> tokens;
		Buffer* previous = nullptr;
		for (size_t i = 0; i < commands.size(); ++i) {
			Expect(device.CreateCommandAllocator(QueueType::Graphics, allocators[i]) == Result::Success &&
					device.CreateCommandList(allocators[i].Raw(), commands[i]) == Result::Success &&
					commands[i]->Begin() == Result::Success,
				"Begin independent graph execution");
			Expect(graph.Execute(device, *commands[i]) == Result::Success, "Execute with a fresh allocation batch");
			Expect(graph.GetBuffer(data) != previous, "An in-flight allocation is never reused by another execution");
			previous = graph.GetBuffer(data);
			readbacks[i] = Ref<Buffer>(graph.GetBuffer(output));
			Expect(commands[i]->End() == Result::Success, "End independent graph execution");
			tokens[i] = device.GetGraphicsCommandQueue()->Submit(*commands[i]);
			Expect(tokens[i].IsValid(), "Submit graph execution without waiting for the preceding one");
		}
		graph.Reset();
		upload.Reset();
		for (size_t i = 0; i < commands.size(); ++i) {
			device.GetGraphicsCommandQueue()->Wait(tokens[i]);
			std::array<uint32_t, 4> values{};
			Expect(readbacks[i]->ReadData(values.data(), sizeof(values)) == Result::Success && values == initial,
				"Both in-flight executions retain their own complete output after graph reset");
			Expect(commands[i]->Reset() == Result::Success, "Release completed execution resources");
			readbacks[i].Reset();
		}
		Expect(device.GetStatistics().buffers.liveCount == baseline.buffers.liveCount,
			"Graph-owned allocations are released after completed lists and retained outputs are reset");
	}

	void TestTexturePasses(Device& device) {
		Ref<Shader> shader;
		Ref<Pipeline> pipeline;
		Expect(device.CreateShader({.moduleName = "render_graph"}, shader) == Result::Success &&
				device.CreateGraphicsPipeline(
					{.shader = shader.Raw(),
						.rasterState = {.cullMode = CullMode::None},
						.depthState = {.depthTestEnable = false, .depthWriteEnable = false},
						.renderTargetFormats = {.colorFormats = {TextureFormat::RGBA8_UNorm}}},
					pipeline) == Result::Success,
			"Create render graph texture pipeline");
		Ref<Buffer> readback;
		Expect(device.CreateBuffer(
				   {.size = 8 * 8 * 8, .usage = BufferUsage::TransferDst, .memoryUsage = MemoryUsage::GPUToCPU},
				   readback) == Result::Success,
			"Create graph texture readback");
		Ref<CommandAllocator> allocator;
		Ref<CommandList> commands;
		Expect(device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success &&
				device.CreateCommandList(allocator.Raw(), commands) == Result::Success &&
				commands->Begin() == Result::Success,
			"Begin graph texture commands");
		const ResourceState attachment{PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite};
		const ResourceState sampled{PipelineStage::FragmentShader, ResourceAccess::ShaderRead};
		auto draw = [&](CommandList& list, Texture* target, Texture* source, std::array<float, 4> color) {
			list.BeginRendering(
				{.colorAttachments = {{.target = target->GetTextureView()}}, .renderArea = {.width = 8, .height = 8}});
			list.SetViewport({.width = 8, .height = 8});
			list.SetScissor({.width = 8, .height = 8});
			list.BindPipeline(pipeline.Raw());
			struct Parameters {
				std::array<float, 4> color;
				uint32_t textureIndex = 0;
				uint32_t sampleTexture = 0;
			} parameters{color};
			if (source) {
				list.UseResource(source);
				parameters.textureIndex = source->GetSampledImageDescriptorIndex();
				parameters.sampleTexture = 1;
			}
			list.SetPushConstants(&parameters, sizeof(parameters));
			list.Draw(3);
			list.EndRendering();
		};
		{
			RenderGraph graph(Capture);
			std::string debugName = "Graph-owned image";
			const TextureDesc textureDesc{
				.width = 8,
				.height = 8,
				.format = TextureFormat::RGBA8_UNorm,
				.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::TransferSrc,
				.debugName = debugName.c_str()
			};
			const auto first = graph.CreateTexture(textureDesc);
			const auto second = graph.CreateTexture(textureDesc);
			debugName.assign(1024, 'x');
			const auto output = graph.ImportBuffer(readback.Raw());
			graph.AddPass(
				{.name = "Green source",
					.resources = {{first, attachment, TextureLayout::ColorAttachment, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					draw(list, graph.GetTexture(first), nullptr, {0, 1, 0, 1});
				});
			graph.AddPass({.name = "Sample source",
							  .resources =
								  {{first, sampled, TextureLayout::ShaderRead},
									  {second, attachment, TextureLayout::ColorAttachment, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					draw(list, graph.GetTexture(second), graph.GetTexture(first), {});
				});
			graph.AddPass(
				{.name = "Overwrite sampled texture",
					.resources = {{first, attachment, TextureLayout::ColorAttachment, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					draw(list, graph.GetTexture(first), nullptr, {0, 0, 1, 1});
				});
			graph.AddPass({.name = "Read textures",
							  .resources =
								  {{first, s_TransferRead, TextureLayout::TransferSrc},
									  {second, s_TransferRead, TextureLayout::TransferSrc},
									  {output, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					list.CopyTextureToBuffer(graph.GetTexture(first), readback.Raw(), {.width = 8, .height = 8});
					list.CopyTextureToBuffer(
						graph.GetTexture(second), readback.Raw(), {.width = 8, .height = 8, .bufferOffset = 8 * 8 * 4});
				});
			const auto before = device.GetStatistics();
			std::string error;
			Expect(graph.Compile(error), "Compile graph-created textures before allocation");
			Expect(device.GetStatistics().textures.liveCount == before.textures.liveCount,
				"Compile allocates no textures");
			++s_ExpectedErrors;
			Expect(graph.GetTexture(first) == nullptr, "Created textures do not resolve before Execute");
			Expect(graph.Execute(device, *commands) == Result::Success, "Allocate and record graph-created textures");
			Expect(device.GetStatistics().textures.liveCount == before.textures.liveCount + 2,
				"Execute allocates the two declared textures");
			graph.Reset();
		}
		pipeline.Reset();
		shader.Reset();
		device.ClearShaderAndPipelineCaches();
		Expect(commands->End() == Result::Success, "End graph texture commands");
		const auto token = device.GetGraphicsCommandQueue()->Submit(*commands);
		Expect(token.IsValid(), "Pass-local resource declarations permit sampled-to-attachment reuse");
		device.GetGraphicsCommandQueue()->Wait(token);
		std::array<uint8_t, 8 * 8 * 8> pixels{};
		Expect(readback->ReadData(pixels.data(), pixels.size()) == Result::Success, "Read graph textures");
		for (size_t i = 0; i < 8 * 8 * 4; i += 4) {
			Expect(pixels[i] == 0 && pixels[i + 1] == 0 && pixels[i + 2] == 255 && pixels[i + 8 * 8 * 4] == 0 &&
					pixels[i + 8 * 8 * 4 + 1] == 255 && pixels[i + 8 * 8 * 4 + 2] == 0,
				"Sampled output stays green while the source is overwritten blue");
		}
	}

	void TestExecution(Device& device) {
		std::array<uint32_t, 64> initial;
		for (uint32_t i = 0; i < initial.size(); ++i) {
			initial[i] = i;
		}
		Ref<Buffer> upload;
		Ref<Buffer> snapshot;
		Ref<Buffer> readback;
		Expect(device.CreateBuffer({.size = sizeof(initial),
									   .usage = BufferUsage::TransferSrc,
									   .memoryUsage = MemoryUsage::CPUToGPU,
									   .initialData = initial.data()},
				   upload) == Result::Success,
			"Create graph upload");
		for (auto* target : {&snapshot, &readback}) {
			Expect(
				device.CreateBuffer(
					{.size = sizeof(initial), .usage = BufferUsage::TransferDst, .memoryUsage = MemoryUsage::GPUToCPU},
					*target) == Result::Success,
				"Create graph readback");
		}
		Ref<Shader> shader;
		Ref<Pipeline> pipeline;
		Expect(device.CreateShader({.moduleName = "ownership_compute"}, shader) == Result::Success &&
				device.CreateComputePipeline({.shader = shader.Raw()}, pipeline) == Result::Success,
			"Create graph compute pipeline");
		Ref<CommandAllocator> allocator;
		Ref<CommandList> commands;
		Expect(device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success &&
				device.CreateCommandList(allocator.Raw(), commands) == Result::Success,
			"Create graph command list");

		uint32_t skipped = 0;
		RenderGraph invalid(Capture);
		const auto bad = invalid.ImportBuffer(nullptr);
		invalid.AddPass({.name = "Invalid", .resources = {{bad, s_ComputeRead}}}, [&](CommandList&) {
			++skipped;
		});
		++s_ExpectedErrors;
		Expect(
			invalid.Execute(device, *commands) == Result::InvalidParameter, "Invalid graph execution returns failure");
		Expect(commands->Begin() == Result::Success, "Begin graph recording");
		++s_ExpectedErrors;
		Expect(
			invalid.Execute(device, *commands) == Result::InvalidParameter, "Invalid graph execution returns failure");
		Expect(skipped == 0, "Invalid graphs record no partial callbacks");

		std::vector<std::string> order;
		{
			RenderGraph graph(Capture);
			const auto source = graph.ImportBuffer(upload.Raw());
			const auto working = graph.CreateBuffer(
				{.size = sizeof(initial),
					.usage = BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc});
			const auto original = graph.ImportBuffer(snapshot.Raw());
			const auto result = graph.ImportBuffer(readback.Raw());
			graph.AddPass({.name = "Upload",
							  .resources =
								  {{source, s_TransferRead},
									  {working, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					order.push_back("Upload");
					list.CopyBuffer(upload.Raw(), graph.GetBuffer(working), {.size = sizeof(initial)});
				});
			graph.AddPass({.name = "Snapshot",
							  .resources =
								  {{working, s_TransferRead},
									  {original, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					order.push_back("Snapshot");
					list.CopyBuffer(graph.GetBuffer(working), snapshot.Raw(), {.size = sizeof(initial)});
				});
			graph.AddPass({.name = "Repeated snapshot",
							  .resources =
								  {{working, s_TransferRead},
									  {original, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					order.push_back("Repeated snapshot");
					list.CopyBuffer(graph.GetBuffer(working), snapshot.Raw(), {.size = sizeof(initial)});
				});
			for (uint32_t i = 0; i < 2; ++i) {
				graph.AddPass({.name = "Compute",
								  .resources =
									  {{working,
										  {PipelineStage::ComputeShader,
											  ResourceAccess::ShaderRead | ResourceAccess::ShaderWrite},
										  TextureLayout::Undefined,
										  RenderGraphAccess::ReadWrite}}},
					[&](CommandList& list) {
						order.push_back("Compute");
						list.BindPipeline(pipeline.Raw());
						list.UseResource(graph.GetBuffer(working));
						const uint32_t parameters[] = {graph.GetBuffer(working)->GetDescriptorIndex(), 64};
						list.SetPushConstants(parameters, sizeof(parameters));
						list.DispatchCompute(1, 1, 1);
					});
			}
			graph.AddPass({.name = "Readback",
							  .resources =
								  {{working, s_TransferRead},
									  {result, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					order.push_back("Readback");
					list.CopyBuffer(graph.GetBuffer(working), readback.Raw(), {.size = sizeof(initial)});
				});
			Expect(graph.Execute(device, *commands) == Result::Success, "Allocate and record graph working buffer");
			Expect(graph.GetExecutionPlan()[2].barriers.buffers.size() == 1 &&
					graph.GetExecutionPlan()[2].barriers.buffers[0].buffer == snapshot.Raw(),
				"A repeated copy synchronizes its destination without a redundant source read barrier");
			Expect(order ==
					std::vector<std::string>{
						"Upload", "Snapshot", "Repeated snapshot", "Compute", "Compute", "Readback"
					},
				"Read/write hazards preserve callback ordering");
		}
		upload.Reset();
		pipeline.Reset();
		shader.Reset();
		device.ClearShaderAndPipelineCaches();
		Expect(commands->End() == Result::Success, "End recording after an invalid and a valid graph");
		const auto completion = device.GetGraphicsCommandQueue()->Submit(*commands);
		Expect(completion.IsValid(), "Graph destruction and invalid recording do not poison submission");
		device.GetGraphicsCommandQueue()->Wait(completion);
		std::array<uint32_t, 64> values{};
		Expect(snapshot->ReadData(values.data(), sizeof(values)) == Result::Success && values == initial,
			"Write-after-read dependency preserves the snapshot taken before compute");
		Expect(readback->ReadData(values.data(), sizeof(values)) == Result::Success, "Read graph compute output");
		for (uint32_t i = 0; i < values.size(); ++i) {
			Expect(values[i] == i + 6,
				"Transfer-to-compute, compute-to-compute, and compute-to-transfer barriers preserve data");
		}
	}
}

int main() {
	std::unique_ptr<RealRHI::Device> device;
	Expect(RealRHI::Device::Create({.applicationName = "RenderGraphTests",
									   .shaderDirectory = REALRHI_TEST_SHADER_DIRECTORY,
									   .enableDebug = true,
									   .enableValidationLayers = true,
									   .debugCallback = Capture,
									   .enableProfiling = true},
			   device) == RealRHI::Result::Success,
		"Create render graph test device");
	{
		RealRHI::Ref<RealRHI::Buffer> buffer;
		RealRHI::Ref<RealRHI::Texture> texture;
		Expect(device->CreateBuffer({.size = 32, .usage = RealRHI::BufferUsage::Storage}, buffer) ==
				RealRHI::Result::Success,
			"Create planning buffer");
		Expect(
			device->CreateTexture({.width = 8,
									  .height = 8,
									  .mipLevels = 2,
									  .format = RealRHI::TextureFormat::RGBA8_UNorm,
									  .usage = RealRHI::TextureUsage::TransferSrc | RealRHI::TextureUsage::TransferDst |
										  RealRHI::TextureUsage::ShaderResource},
				texture) == RealRHI::Result::Success,
			"Create planning texture");
		TestPlanning(buffer.Raw(), texture.Raw());
		TestDiagnosticsAndCulling(*device, buffer.Raw());
		TestInvalidGraphs(buffer.Raw(), texture.Raw());
		TestExecution(*device);
		TestTexturePasses(*device);
		TestCreatedValidation(*device);
		TestExecutionsInFlight(*device);
	}
	device->WaitIdle();
	device.reset();
	Expect(s_Errors == s_ExpectedErrors, "Only expected graph errors occur, including cleanup");
	std::cout << "RenderGraphTests passed\n";
}
