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
					std::string_view(message.message).starts_with("RenderGraph:"),
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
		Ref<Texture> a;
		Ref<Texture> b;
		for (auto* target : {&a, &b}) {
			Expect(device.CreateTexture({.width = 8,
											.height = 8,
											.format = TextureFormat::RGBA8_UNorm,
											.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource |
												TextureUsage::TransferSrc},
					   *target) == Result::Success,
				"Create graph texture");
		}
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
			const auto first = graph.ImportTexture(a.Raw(), TextureLayout::Undefined);
			const auto second = graph.ImportTexture(b.Raw(), TextureLayout::Undefined);
			const auto output = graph.ImportBuffer(readback.Raw());
			graph.AddPass(
				{.name = "Green source",
					.resources = {{first, attachment, TextureLayout::ColorAttachment, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					draw(list, a.Raw(), nullptr, {0, 1, 0, 1});
				});
			graph.AddPass({.name = "Sample source",
							  .resources =
								  {{first, sampled, TextureLayout::ShaderRead},
									  {second, attachment, TextureLayout::ColorAttachment, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					draw(list, b.Raw(), a.Raw(), {});
				});
			graph.AddPass(
				{.name = "Overwrite sampled texture",
					.resources = {{first, attachment, TextureLayout::ColorAttachment, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					draw(list, a.Raw(), nullptr, {0, 0, 1, 1});
				});
			graph.AddPass({.name = "Read textures",
							  .resources =
								  {{first, s_TransferRead, TextureLayout::TransferSrc},
									  {second, s_TransferRead, TextureLayout::TransferSrc},
									  {output, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					list.CopyTextureToBuffer(a.Raw(), readback.Raw(), {.width = 8, .height = 8});
					list.CopyTextureToBuffer(
						b.Raw(), readback.Raw(), {.width = 8, .height = 8, .bufferOffset = 8 * 8 * 4});
				});
			graph.Record(*commands);
		}
		a.Reset();
		b.Reset();
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
		Ref<Buffer> data;
		Ref<Buffer> snapshot;
		Ref<Buffer> readback;
		Expect(device.CreateBuffer({.size = sizeof(initial),
									   .usage = BufferUsage::TransferSrc,
									   .memoryUsage = MemoryUsage::CPUToGPU,
									   .initialData = initial.data()},
				   upload) == Result::Success,
			"Create graph upload");
		Expect(device.CreateBuffer(
				   {.size = sizeof(initial),
					   .usage = BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc},
				   data) == Result::Success,
			"Create graph data");
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
		invalid.Record(*commands);
		Expect(commands->Begin() == Result::Success, "Begin graph recording");
		++s_ExpectedErrors;
		invalid.Record(*commands);
		Expect(skipped == 0, "Invalid graphs record no partial callbacks");

		std::vector<std::string> order;
		{
			RenderGraph graph(Capture);
			const auto source = graph.ImportBuffer(upload.Raw());
			const auto working = graph.ImportBuffer(data.Raw());
			const auto original = graph.ImportBuffer(snapshot.Raw());
			const auto result = graph.ImportBuffer(readback.Raw());
			graph.AddPass({.name = "Upload",
							  .resources =
								  {{source, s_TransferRead},
									  {working, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					order.push_back("Upload");
					list.CopyBuffer(upload.Raw(), data.Raw(), {.size = sizeof(initial)});
				});
			graph.AddPass({.name = "Snapshot",
							  .resources =
								  {{working, s_TransferRead},
									  {original, s_TransferWrite, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				[&](CommandList& list) {
					order.push_back("Snapshot");
					list.CopyBuffer(data.Raw(), snapshot.Raw(), {.size = sizeof(initial)});
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
						list.UseResource(data.Raw());
						const uint32_t parameters[] = {data->GetDescriptorIndex(), 64};
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
					list.CopyBuffer(data.Raw(), readback.Raw(), {.size = sizeof(initial)});
				});
			graph.Record(*commands);
			Expect(order == std::vector<std::string>{"Upload", "Snapshot", "Compute", "Compute", "Readback"},
				"Read/write hazards preserve callback ordering");
		}
		upload.Reset();
		data.Reset();
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
									   .debugCallback = Capture},
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
		TestInvalidGraphs(buffer.Raw(), texture.Raw());
		TestExecution(*device);
		TestTexturePasses(*device);
	}
	device->WaitIdle();
	device.reset();
	Expect(s_Errors == s_ExpectedErrors, "Only expected graph errors occur, including cleanup");
	std::cout << "RenderGraphTests passed\n";
}
