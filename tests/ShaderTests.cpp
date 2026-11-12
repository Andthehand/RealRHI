#include "Slang/SlangShaderCompiler.h"

#include <cstdlib>
#include <iostream>

namespace {
	using namespace RealRHI;
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
			++errors;
		}
	}
}

int main() {
	using namespace RealRHI;
#ifdef REALRHI_IMGUI_SHADER_DIRECTORY
	{
		SlangShaderCompiler imguiCompiler(REALRHI_IMGUI_SHADER_DIRECTORY, true, Capture);
		CompiledShaderProgram imguiProgram;
		Expect(imguiCompiler.Compile({.moduleName = "realrhi_imgui"}, imguiProgram) == Result::Success,
			"Compile ImGui graphics program without a GPU");
		Expect(imguiProgram.reflection.vertexInputs.size() == 3, "ImGui reflects position, UV, and color inputs");
		Expect(imguiProgram.reflection.pushData.size == 24, "ImGui push constants match the renderer's C++ layout");
	}
#endif
	SlangShaderCompiler compiler(REALRHI_TEST_SHADER_DIRECTORY, true, Capture);
	CompiledShaderProgram program;
	ShaderDesc graphics{
		.moduleName = "reflection",
		.entryPoints = {{"vertexMain", ShaderStage::Vertex}, {"fragmentMain", ShaderStage::Fragment}}
	};
	Expect(compiler.Compile(graphics, program) == Result::Success, "Compile selected graphics entries without a GPU");
	const auto& inputs = program.reflection.vertexInputs;
	Expect(inputs.size() == 6, "Retain multiple parameters, nested fields, arrays, and exclude system inputs");
	for (size_t i = 0; i < inputs.size(); ++i) {
		Expect(inputs[i].location == i, "Preserve consecutive reflected input locations");
	}
	Expect(
		inputs[2].scalarType == ShaderScalarType::UInt32 && inputs[2].componentCount == 2, "Reflect integer vectors");
	const auto& packedLayout = program.reflection.vertexLayout;
	Expect(packedLayout.bindings.size() == 1 && packedLayout.bindings[0].binding == 0 &&
			packedLayout.bindings[0].stride == 60,
		"Infer a single tightly packed vertex stream");
	const uint32_t offsets[] = {0, 12, 20, 28, 36, 44};
	for (size_t i = 0; i < packedLayout.attributes.size(); ++i) {
		Expect(packedLayout.attributes[i].offset == offsets[i] &&
				packedLayout.attributes[i].location == inputs[i].location,
			"Pack attributes without C++ padding while retaining reflected locations");
	}
	Expect(packedLayout.attributes[2].format == VertexFormat::UInt2, "Infer integer vector storage");
	Expect(program.reflection.pushData.size == 48, "Preserve push-data layout padding");
	Expect(program.reflection.entryPoints.size() == 2 && program.exportedEntryPoints.size() == 2,
		"Only selected stages are emitted");
	ShaderDesc compute{.moduleName = "reflection", .entryPoints = {{"computeMain"}}};
	Expect(compiler.Compile(compute, program) == Result::Success, "Select compute from a multi-entry module");
	Expect(program.reflection.vertexInputs.empty(), "Compute has no vertex inputs");
	Expect(program.reflection.entryPoints[0].computeGroupSize == std::array<uint32_t, 3>{8, 4, 2},
		"Reflect compute group size");
	const auto savedCode = program.spirv;
	ShaderDesc missing{.moduleName = "reflection", .entryPoints = {{"missing", ShaderStage::Compute}}};
	Expect(compiler.Compile(missing, program) != Result::Success, "Missing entry fails");
	Expect(program.spirv == savedCode, "Failure does not publish partial program");
	graphics.entryPoints[0].stage = ShaderStage::Compute;
	Expect(compiler.Compile(graphics, program) != Result::Success, "Mixed graphics and compute selection fails");
	compute.entryPoints[0].stage = ShaderStage::Vertex;
	Expect(compiler.Compile(compute, program) != Result::Success, "Wrong stage fails");
	Expect(compiler.Compile({.moduleName = "reflection"}, program) != Result::Success,
		"Mixed-program module requires selection");
	Expect(compiler.Compile({.moduleName = "reflection_matrix", .entryPoints = {{"vertexMain", ShaderStage::Vertex}}},
			   program) == Result::Success,
		"Reflect a column-major matrix input");
	Expect(program.reflection.vertexInputs.size() == 2 && program.reflection.vertexInputs[0].componentCount == 3 &&
			program.reflection.vertexInputs[1].location == 1,
		"Expand matrix columns into consecutive vector locations");
	SlangShaderCompiler independent(REALRHI_OTHER_SHADER_DIRECTORY, false, Capture);
	Expect(independent.Compile(
			   {.moduleName = "shader",
				   .entryPoints = {{"vertexMain", ShaderStage::Vertex}, {"fragmentMain", ShaderStage::Fragment}}},
			   program) == Result::Success,
		"Compiler contexts use independent directories and options");
	Expect(program.reflection.vertexInputs.size() == 2, "Independent directory produces its own layout");
	Expect(compiler.Compile(
			   {.moduleName = "reflection_multiple_push", .entryPoints = {{"vertexMain", ShaderStage::Vertex}}},
			   program) != Result::Success,
		"Reject multiple push-data declarations");
	Expect(compiler.Compile({.moduleName = "reflection_invalid", .entryPoints = {{"vertexMain", ShaderStage::Vertex}}},
			   program) != Result::Success,
		"Compilation errors fail without publishing a shader");
	Expect(compiler.Compile({.moduleName = "ownership_compute"}, program) == Result::Success,
		"Discover compute name and stage");
	Expect(program.reflection.entryPoints.size() == 1 && program.reflection.entryPoints[0].name == "computeMain" &&
			program.reflection.entryPoints[0].stage == ShaderStage::Compute,
		"Preserve discovered compute metadata");
	Expect(compiler.Compile({.moduleName = "reflection_discovery"}, program) == Result::Success,
		"Discover arbitrary graphics entry-point names");
	Expect(program.reflection.entryPoints.size() == 2 && program.reflection.entryPoints[0].name == "customVertex" &&
			program.reflection.entryPoints[0].stage == ShaderStage::Vertex &&
			program.reflection.entryPoints[1].name == "customFragment" &&
			program.reflection.entryPoints[1].stage == ShaderStage::Fragment,
		"Reflect names and stages without caller declarations");
	Expect(errors == 6, "Expected failures produce diagnostics");
	ShaderDesc variant{.moduleName = "preprocessor"};
	Expect(compiler.Compile(variant, program) == Result::Success, "Compile without preprocessor definitions");
	const auto defaultId = program.cacheId;
	Expect(program.reflection.entryPoints[0].computeGroupSize == std::array<uint32_t, 3>{4, 1, 1},
		"Use source defaults when definitions are absent");
	variant.preprocessorDefinitions = {{"GROUP_SIZE", "8"}, {"ENABLE_FEATURE"}, {"EMPTY_REPLACEMENT", ""}};
	Expect(compiler.Compile(variant, program) == Result::Success, "Compile valued, default, and empty definitions");
	const auto variantId = program.cacheId;
	Expect(variantId != defaultId &&
			program.reflection.entryPoints[0].computeGroupSize == std::array<uint32_t, 3>{8, 2, 1},
		"Definitions affect compilation and distinguish cache entries");
	Expect(compiler.Compile(variant, program) == Result::Success && program.cacheId == variantId,
		"Identical definitions reuse cached compilation");
	variant.preprocessorDefinitions[0].value = "16";
	Expect(compiler.Compile(variant, program) == Result::Success && program.cacheId != variantId &&
			program.reflection.entryPoints[0].computeGroupSize == std::array<uint32_t, 3>{16, 2, 1},
		"Changing a definition value compiles a distinct variant");
	variant.preprocessorDefinitions.clear();
	Expect(compiler.Compile(variant, program) == Result::Success && program.cacheId == defaultId,
		"Definitions do not leak into the default variant");
	compiler.ClearCache();
	Expect(compiler.Compile(variant, program) == Result::Success &&
			program.reflection.entryPoints[0].computeGroupSize == std::array<uint32_t, 3>{4, 1, 1},
		"Definitions do not leak into a fresh session");
	const auto validId = program.cacheId;
	for (const auto& definitions : std::vector<std::vector<ShaderPreprocessorDefinition>>{
			 {{"", "1"}}, {{"NOT-AN-IDENTIFIER", "1"}}, {{"GROUP_SIZE", "8"}, {"GROUP_SIZE", "16"}},
			 {{"GROUP_SIZE", std::string("8\0ignored", 9)}}}) {
		variant.preprocessorDefinitions = definitions;
		Expect(compiler.Compile(variant, program) == Result::InvalidParameter && program.cacheId == validId,
			"Invalid definitions fail without publishing a partial program");
	}
	Expect(errors == 10, "Invalid definitions produce diagnostics");
	std::cout << "ShaderTests passed\n";
}
