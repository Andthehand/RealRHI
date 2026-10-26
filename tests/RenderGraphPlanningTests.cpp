#include "RenderGraph/RenderGraph.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

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

	void TestCullingAndReset() {
		RenderGraph graph;
		graph.SetOptions({.cullUnusedPasses = true});
		const auto output = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
		const auto discarded = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
		graph.AddPass(
			{.name = "Output", .resources = {{output, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
			EmptyPass);
		graph.AddPass(
			{.name = "Discarded",
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

	void TestInitializationAndLayouts() {
		RenderGraph graph;
		const auto texture = graph.CreateTexture({.width = 4,
			.height = 4,
			.format = TextureFormat::RGBA8_UNorm,
			.usage = TextureUsage::ShaderResource | TextureUsage::Storage});
		graph.AddPass({.name = "Uninitialized read", .resources = {{texture, s_Read, TextureLayout::ShaderRead}}},
			EmptyPass);
		ExpectInvalid(graph, "written before reading");

		graph.Reset();
		const auto initialized = graph.CreateTexture({.width = 4,
			.height = 4,
			.format = TextureFormat::RGBA8_UNorm,
			.usage = TextureUsage::ShaderResource | TextureUsage::Storage});
		graph.AddPass(
			{.name = "Initialize", .resources = {{initialized, s_Write, TextureLayout::General, RenderGraphAccess::Write}}},
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
	TestScheduling();
	TestCullingAndReset();
	TestInitializationAndLayouts();
	std::cout << "RenderGraphPlanningTests passed\n";
}
