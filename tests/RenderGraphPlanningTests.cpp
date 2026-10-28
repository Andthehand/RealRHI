#include "RenderGraph/RenderGraph.h"

#include <array>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {
	using namespace RealRHI;
	constexpr ResourceState s_Read{PipelineStage::ComputeShader, ResourceAccess::ShaderRead};
	constexpr ResourceState s_Write{PipelineStage::ComputeShader, ResourceAccess::ShaderWrite};

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void EmptyPass(CommandList&) { }

	class PlanningBuffer : public Buffer {
	public:
		uint64_t GetAllocationSize() const override { return 0; }
		uint64_t GetSize() const override { return UINT64_MAX; }
		uint32_t GetDescriptorIndex() const override { return UINT32_MAX; }
		Result WriteData(const void*, uint64_t, uint64_t) override { return Result::Failed; }
		Result ReadData(void*, uint64_t, uint64_t) const override { return Result::Failed; }
	};

	class PlanningTexture : public Texture {
	public:
		uint64_t GetAllocationSize() const override { return 0; }
		uint32_t GetSampleCount() const override { return 1; }
		TextureView* GetTextureView() override { return nullptr; }
		const TextureView* GetTextureView() const override { return nullptr; }
		Result UploadData(const void*, uint32_t) override { return Result::Failed; }
	};

	void ExpectInvalid(RenderGraph& graph, std::string_view diagnostic) {
		std::string error;
		Expect(!graph.Compile(error) && error.find(diagnostic) != std::string::npos,
			"Invalid graph reports its cause without a device");
		Expect(!graph.GetDiagnostics().compiled && graph.GetExecutionPlan().empty(),
			"Invalid compilation publishes no partial plan");
	}

	void TestScheduling() {
		RenderGraph graph;
		const auto data = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
		const auto independent = graph.AddPass({.name = "Independent"}, EmptyPass);
		const auto writer = graph.AddPass(
			{.name = "Write", .resources = {{data, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			EmptyPass);
		const auto firstReader = graph.AddPass({.name = "Read A", .resources = {{data, s_Read}}}, EmptyPass);
		const auto secondReader = graph.AddPass({.name = "Read B", .resources = {{data, s_Read}}}, EmptyPass);
		graph.AddPass(
			{.name = "Overwrite", .resources = {{data, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			EmptyPass);
		graph.AddDependency(independent, firstReader);
		// Repeated declarations must not add another incoming edge.
		graph.AddDependency(firstReader, writer);
		graph.AddDependency(firstReader, writer);

		std::string error;
		Expect(graph.Compile(error), "Compile created resources without allocating them");
		const auto& plan = graph.GetExecutionPlan();
		Expect(plan.size() == 5 && plan[0].name == "Write" && plan[1].name == "Read A" &&
				plan[2].name == "Independent" && plan[3].name == "Read B" && plan[4].name == "Overwrite",
			"Choose the earliest ready pass while respecting resource hazards");
		const auto diagnostics = graph.GetDiagnostics();
		Expect(diagnostics.passes[firstReader.index].prerequisites.size() == 1 &&
				diagnostics.passes[secondReader.index].prerequisites.size() == 1 &&
				diagnostics.passes.back().prerequisites.size() == 3,
			"Reads can run independently; overwriting waits for the writer and both readers");
		Expect(diagnostics.transientBytes == 0, "Planning uses no native allocations");
		Expect(graph.Compile(error) && graph.GetExecutionPlan().size() == 5,
			"Repeated compilation replaces the previous schedule");

		graph.AddDependency(writer, secondReader);
		ExpectInvalid(graph, "cycle");
	}

	void TestImportValidation() {
		auto buffer = Ref<PlanningBuffer>::Create();
		auto otherBuffer = Ref<PlanningBuffer>::Create();
		auto texture = Ref<PlanningTexture>::Create();
		auto otherTexture = Ref<PlanningTexture>::Create();
		RenderGraph graph;
		std::string error;
		// Import out of order; adjacent intervals and identical ranges on different objects are legal.
		for (uint64_t offset : std::array<uint64_t, 4>{64, 0, 32, UINT64_MAX - 32}) {
			graph.ImportBuffer(buffer.Raw(), {}, offset, 32);
		}
		graph.ImportBuffer(otherBuffer.Raw(), {}, 0, 96);
		Expect(graph.Compile(error) && graph.Compile(error), "Disjoint buffer imports validate across recompilation");
		for (const auto [offset, size] :
			std::array<std::pair<uint64_t, uint64_t>, 6>{{{0, 64}, {16, 64}, {48, 64}, {80, 64}, {40, 8}, {32, 32}}}) {
			graph.Reset();
			graph.ImportBuffer(buffer.Raw(), {}, 32, 32);
			graph.ImportBuffer(buffer.Raw(), {}, 96, 32);
			graph.ImportBuffer(buffer.Raw(), {}, offset, size);
			Expect(!graph.Compile(error) &&
					error == "Resource 2: overlaps another import; use the same handle or import disjoint ranges.",
				"Reject containing, contained, preceding and following buffer overlaps with the original diagnostic");
		}
		graph.Reset();
		graph.ImportBuffer(buffer.Raw(), {}, UINT64_MAX - 16, 32);
		ExpectInvalid(graph, "buffer range must be nonempty and fit");

		graph.Reset();
		graph.ImportTexture(texture.Raw(), TextureLayout::ShaderRead, s_Read, {2, 2, 0, 2});
		graph.ImportTexture(texture.Raw(), TextureLayout::ShaderRead, s_Read, {0, 2, 0, 2});
		graph.ImportTexture(texture.Raw(), TextureLayout::ShaderRead, s_Read, {0, 4, 2, 2});
		graph.ImportTexture(otherTexture.Raw(), TextureLayout::ShaderRead, s_Read, {0, 4, 0, 4});
		Expect(graph.Compile(error), "Mip or layer separation and distinct texture identity allow imports");
		graph.ImportTexture(texture.Raw(), TextureLayout::ShaderRead, s_Read, {1, 2, 1, 2});
		Expect(!graph.Compile(error) &&
				error == "Resource 4: overlaps another import; use the same handle or import disjoint ranges.",
			"Texture imports must overlap in both mip and layer dimensions to conflict");
		graph.Reset();
		graph.ImportTexture(texture.Raw(), TextureLayout::Undefined, {}, {UINT32_MAX, 2, 0, 1});
		ExpectInvalid(graph, "texture range and initial layout must be valid");
	}

	void TestManyResourceDeclarations() {
		RenderGraph graph;
		std::vector<RenderGraphUse> writes;
		std::vector<RenderGraphUse> reads;
		for (uint32_t i = 0; i < 4096; ++i) {
			const auto buffer = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
			writes.push_back({buffer, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write});
			reads.push_back({buffer, s_Read});
		}
		graph.AddPass({.name = "Initialize many resources", .resources = writes}, EmptyPass);
		graph.AddPass({.name = "First reader", .resources = reads}, EmptyPass);
		graph.AddPass({.name = "Second reader", .resources = reads}, EmptyPass);
		std::string error;
		Expect(graph.Compile(error) && graph.Compile(error) && graph.GetExecutionPlan().size() == 3,
			"Each pass can declare the same large resource set across repeated compilation");
		reads.push_back(reads.front());
		graph.AddPass({.name = "Duplicate at end", .resources = reads}, EmptyPass);
		Expect(!graph.Compile(error) &&
				error ==
					"Pass 'Duplicate at end': declare each resource once; use ReadWrite for an in-place operation.",
			"Reject a nonadjacent duplicate in a large declaration list with its original diagnostic");
		graph.Reset();
		auto invalid = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
		invalid.index = UINT32_MAX;
		graph.AddPass({.name = "Invalid index", .resources = {{invalid, s_Read}}}, EmptyPass);
		ExpectInvalid(graph, "resource handle belongs to another graph, was reset, or is invalid");
	}

	void TestCullingAndReset() {
		RenderGraph graph;
		graph.SetOptions({.cullUnusedPasses = true});
		const auto output = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
		const auto discarded = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
		graph.AddPass(
			{.name = "Output", .resources = {{output, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			EmptyPass);
		graph.AddPass({.name = "Discarded",
						  .resources = {{discarded, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			EmptyPass);
		const auto prerequisite = graph.AddPass({.name = "Prerequisite"}, EmptyPass);
		graph.AddPass({.name = "Side effect", .dependsOn = {prerequisite}, .hasSideEffects = true}, EmptyPass);
		graph.SetFinalState(output, s_Read);
		std::string error;
		Expect(graph.Compile(error), "Compile observable graph roots");
		const auto& plan = graph.GetExecutionPlan();
		Expect(plan.size() == 3 && plan[0].name == "Output" && plan[1].name == "Prerequisite" &&
				plan[2].name == "Side effect",
			"Keep exports and side effects with their prerequisites, culling unused work");

		graph.Reset();
		const auto replacement = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
		Expect(replacement.index == output.index && replacement.graph != output.graph,
			"Reset gives reused indices a new graph identity");
		graph.AddPass({.name = "Stale resource", .resources = {{output, s_Read}}}, EmptyPass);
		ExpectInvalid(graph, "resource handle");
		graph.Reset();
		graph.AddPass({.name = "Unused"}, EmptyPass);
		Expect(graph.Compile(error) && graph.GetExecutionPlan().empty(), "Reset preserves culling options");
	}

	void ExpectPrerequisites(const RenderGraphDiagnostics& diagnostics,
		RenderGraphPass pass,
		std::initializer_list<RenderGraphPass> expected) {
		const auto& prerequisites = diagnostics.passes[pass.index].prerequisites;
		Expect(prerequisites.size() == expected.size(), "Keep exactly the declared and implicit dependencies");
		size_t index = 0;
		for (auto prerequisite : expected) {
			Expect(prerequisites[index].index == prerequisite.index && prerequisites[index].graph == prerequisite.graph,
				"Keep prerequisite diagnostics in their original order");
			++index;
		}
	}

	void TestReadLayoutsAndDependencyOrder() {
		RenderGraph graph;
		const auto texture = graph.CreateTexture(
			{.width = 4,
				.height = 4,
				.format = TextureFormat::RGBA8_UNorm,
				.usage = TextureUsage::ShaderResource | TextureUsage::Storage});
		const auto buffer = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
		const auto writer =
			graph.AddPass({.name = "Initialize",
							  .resources =
								  {{texture, s_Write, TextureLayout::General, RenderGraphAccess::Write},
									  {buffer, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				EmptyPass);
		const auto generalA = graph.AddPass(
			{.name = "General A", .resources = {{texture, s_Read, TextureLayout::General}, {buffer, s_Read}}},
			EmptyPass);
		const auto generalB = graph.AddPass(
			{.name = "General B", .resources = {{texture, s_Read, TextureLayout::General}, {buffer, s_Read}}},
			EmptyPass);
		const auto sampledA = graph.AddPass(
			{.name = "Sample A", .resources = {{texture, s_Read, TextureLayout::ShaderRead}, {buffer, s_Read}}},
			EmptyPass);
		const auto sampledB =
			graph.AddPass({.name = "Sample B",
							  .resources = {{texture, s_Read, TextureLayout::ShaderRead}, {buffer, s_Read}},
							  .dependsOn = {writer, generalA, writer}},
				EmptyPass);
		constexpr ResourceState readWrite{
			PipelineStage::ComputeShader, ResourceAccess::ShaderRead | ResourceAccess::ShaderWrite
		};
		const auto overwrite =
			graph.AddPass({.name = "Overwrite",
							  .resources =
								  {{texture, readWrite, TextureLayout::General, RenderGraphAccess::ReadWrite},
									  {buffer, readWrite, TextureLayout::Undefined, RenderGraphAccess::ReadWrite}}},
				EmptyPass);
		const auto generalAfter =
			graph.AddPass({.name = "General after overwrite",
							  .resources = {{texture, s_Read, TextureLayout::General}, {buffer, s_Read}}},
				EmptyPass);
		const auto sampledAfter =
			graph.AddPass({.name = "Sample after overwrite",
							  .resources = {{texture, s_Read, TextureLayout::ShaderRead}, {buffer, s_Read}}},
				EmptyPass);
		graph.AddDependency(generalA, generalB);
		graph.AddDependency(sampledB, generalA);
		graph.AddDependency(sampledB, writer);
		std::string error;
		Expect(graph.Compile(error), "Reorder compatible readers while keeping texture layout hazards");
		const auto& plan = graph.GetExecutionPlan();
		Expect(plan.size() == 8 && plan[1].name == "General B" && plan[2].name == "General A",
			"Explicit dependencies can reverse reads sharing the same layout");
		const auto diagnostics = graph.GetDiagnostics();
		ExpectPrerequisites(diagnostics, writer, {});
		ExpectPrerequisites(diagnostics, generalA, {writer, generalB});
		ExpectPrerequisites(diagnostics, generalB, {writer});
		ExpectPrerequisites(diagnostics, sampledA, {writer, generalA, generalB});
		ExpectPrerequisites(diagnostics, sampledB, {writer, generalA, generalB});
		ExpectPrerequisites(diagnostics, overwrite, {writer, generalA, generalB, sampledA, sampledB});
		ExpectPrerequisites(diagnostics, generalAfter, {writer, sampledA, sampledB, overwrite});
		ExpectPrerequisites(diagnostics, sampledAfter, {writer, generalA, generalB, overwrite, generalAfter});

		graph.AddDependency(generalB, sampledA);
		ExpectInvalid(graph, "cycle");
	}

	void TestReadyPassOrder() {
		RenderGraph graph;
		constexpr uint32_t count = 1024;
		std::vector<RenderGraphPass> passes;
		for (uint32_t i = 0; i < count; ++i) {
			passes.push_back(graph.AddPass({.name = "Pass " + std::to_string(i)}, EmptyPass));
		}
		for (uint32_t i = 0; i < count; i += 2) {
			graph.AddDependency(passes[i], passes[i + 1]);
		}
		std::string error;
		Expect(graph.Compile(error), "Schedule a wide graph with explicit dependencies");
		const auto diagnostics = graph.GetDiagnostics();
		for (uint32_t i = 0; i < count; i += 2) {
			Expect(diagnostics.passes[i + 1].executionIndex == i && diagnostics.passes[i].executionIndex == i + 1,
				"Newly ready earlier passes run before later passes already waiting");
		}
	}

	void TestExplicitDependencyOrder() {
		RenderGraph graph;
		const auto first = graph.AddPass({.name = "First"}, EmptyPass);
		const auto second = graph.AddPass({.name = "Second"}, EmptyPass);
		const auto buffer = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
		const auto writer =
			graph.AddPass({.name = "Initialize",
							  .resources = {{buffer, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				EmptyPass);
		const auto reader = graph.AddPass(
			{.name = "Read", .resources = {{buffer, s_Read}}, .dependsOn = {second, first, second}}, EmptyPass);
		const auto last = graph.AddPass({.name = "Last prerequisite"}, EmptyPass);
		graph.AddDependency(reader, first);
		graph.AddDependency(reader, last);
		std::string error;
		Expect(graph.Compile(error), "Combine implicit hazards and explicit dependencies");
		const auto diagnostics = graph.GetDiagnostics();
		ExpectPrerequisites(diagnostics, reader, {writer, second, first, last});
		Expect(
			diagnostics.passes[last.index].executionIndex == 3 && diagnostics.passes[reader.index].executionIndex == 4,
			"Explicit prerequisites can point to later independent passes");
	}

	void TestSharedReaders() {
		RenderGraph graph;
		const auto buffer = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
		const auto writer =
			graph.AddPass({.name = "Initialize",
							  .resources = {{buffer, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
				EmptyPass);
		constexpr uint32_t count = 2048;
		std::vector<RenderGraphPass> readers;
		for (uint32_t i = 0; i < count; ++i) {
			readers.push_back(graph.AddPass({.name = "Read " + std::to_string(i),
												.resources = {{buffer, s_Read}},
												.hasSideEffects = i == count - 1},
				EmptyPass));
		}
		std::string error;
		Expect(graph.Compile(error), "Compile many compatible readers of one resource");
		const auto diagnostics = graph.GetDiagnostics();
		for (uint32_t i = 0; i < count; ++i) {
			ExpectPrerequisites(diagnostics, readers[i], {writer});
			Expect(diagnostics.passes[readers[i].index].executionIndex == i + 1,
				"Compatible readers keep insertion order without reader-to-reader edges");
		}
		graph.SetOptions({.cullUnusedPasses = true});
		Expect(graph.Compile(error) && graph.GetExecutionPlan().size() == 2 &&
				graph.GetExecutionPlan().back().name == "Read " + std::to_string(count - 1),
			"Culling one observable reader retains its writer without retaining unrelated readers");
	}

	void TestInitializationAndLayouts() {
		RenderGraph graph;
		const auto texture = graph.CreateTexture(
			{.width = 4,
				.height = 4,
				.format = TextureFormat::RGBA8_UNorm,
				.usage = TextureUsage::ShaderResource | TextureUsage::Storage});
		graph.AddPass(
			{.name = "Uninitialized read", .resources = {{texture, s_Read, TextureLayout::ShaderRead}}}, EmptyPass);
		ExpectInvalid(graph, "written before reading");

		graph.Reset();
		const auto initialized = graph.CreateTexture(
			{.width = 4,
				.height = 4,
				.format = TextureFormat::RGBA8_UNorm,
				.usage = TextureUsage::ShaderResource | TextureUsage::Storage});
		graph.AddPass({.name = "Initialize",
						  .resources = {{initialized, s_Write, TextureLayout::General, RenderGraphAccess::Write}}},
			EmptyPass);
		graph.AddPass({.name = "Sample", .resources = {{initialized, s_Read, TextureLayout::ShaderRead}}}, EmptyPass);
		std::string error;
		Expect(graph.Compile(error), "Plan texture initialization and layout transitions without a device");
		const auto& barriers = graph.GetExecutionPlan()[1].barriers.textures;
		Expect(barriers.size() == 1 && barriers[0].oldLayout == TextureLayout::General &&
				barriers[0].newLayout == TextureLayout::ShaderRead && barriers[0].texture == nullptr,
			"Planning records transitions with deferred resource pointers");
	}
}

int main() {
	TestImportValidation();
	TestManyResourceDeclarations();
	TestScheduling();
	TestReadLayoutsAndDependencyOrder();
	TestReadyPassOrder();
	TestExplicitDependencyOrder();
	TestSharedReaders();
	TestCullingAndReset();
	TestInitializationAndLayouts();
	std::cout << "RenderGraphPlanningTests passed\n";
}
