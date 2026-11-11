#include "Device.h"
#include "Slang/SlangShaderCompiler.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

namespace {
	using namespace RealRHI;
	const std::filesystem::path s_Directory = REALRHI_CACHE_TEST_DIRECTORY;
	uint32_t s_Errors = 0;
	uint32_t s_ExpectedErrors = 0;

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void Capture(const DebugMessage& message) {
		if (message.severity == DebugSeverity::Error) {
			++s_Errors;
		}
		DefaultDebugCallback(message);
	}

	void Write(const char* name, const char* contents) {
		std::ofstream file(s_Directory / name, std::ios::binary);
		file << contents;
		file.close();
		Expect(bool(file), "Write temporary cache shader");
	}

	ShaderDesc ComputeDesc() {
		return {.moduleName = "cache", .entryPoints = {{"computeMain"}}};
	}

	ShaderDesc GraphicsDesc() {
		return {.moduleName = "cache", .entryPoints = {{"vertexMain"}, {"fragmentMain"}}};
	}

	void TestCompiler(const char* source) {
		SlangShaderCompiler compiler(s_Directory, false, Capture);
		CompiledShaderProgram first;
		CompiledShaderProgram repeated;
		Expect(compiler.Compile(ComputeDesc(), first) == Result::Success, "Compile initial compute program");
		Expect(compiler.Compile(ComputeDesc(), repeated) == Result::Success && repeated.cacheId == first.cacheId &&
				repeated.spirv == first.spirv,
			"Repeated program reuses compilation");
		auto named = ComputeDesc();
		named.debugName = "Another name";
		Expect(compiler.Compile(named, repeated) == Result::Success && repeated.cacheId == first.cacheId,
			"Debug names do not alter compiled identity");

		auto asserted = ComputeDesc();
		asserted.entryPoints[0].stage = ShaderStage::Fragment;
		++s_ExpectedErrors;
		Expect(compiler.Compile(asserted, repeated) == Result::InvalidParameter && repeated.cacheId == first.cacheId,
			"Stage assertions cannot bypass validation through a cached program");
		Expect(compiler.Compile(GraphicsDesc(), repeated) == Result::Success && repeated.cacheId != first.cacheId,
			"Entry selection distinguishes graphics from compute");

		const auto timestamp = std::filesystem::last_write_time(s_Directory / "cache_value.slangh");
		Write("cache_value.slangh", "#define CACHE_VALUE 29\n");
		std::filesystem::last_write_time(s_Directory / "cache_value.slangh", timestamp);
		Expect(compiler.Compile(ComputeDesc(), repeated) == Result::Success && repeated.cacheId != first.cacheId &&
				repeated.spirv != first.spirv,
			"Transitive include changes invalidate even with unchanged size and timestamp");
		first = repeated;

		Write("cache_shared.slang", "public uint getValue() { return 31; }\n");
		Expect(compiler.Compile(ComputeDesc(), repeated) == Result::Success && repeated.cacheId != first.cacheId &&
				repeated.spirv != first.spirv,
			"Imported module edits invalidate compiled programs");
		first = repeated;
		Write("cache.slang", "this is deliberately invalid shader code\n");
		++s_ExpectedErrors;
		Expect(compiler.Compile(ComputeDesc(), repeated) != Result::Success && repeated.cacheId == first.cacheId,
			"Broken root source does not return stale success or overwrite output");
		Write("cache.slang", source);
		Expect(compiler.Compile(ComputeDesc(), repeated) == Result::Success && repeated.cacheId != first.cacheId,
			"Repairing source recovers in the same compiler without stale module state");
		first = repeated;
		compiler.ClearCache();
		Expect(compiler.Compile(ComputeDesc(), repeated) == Result::Success && repeated.cacheId != first.cacheId &&
				repeated.spirv == first.spirv,
			"Explicit compiler clear recompiles the same program");
	}

	void DispatchAndVerify(Device& device, Ref<Shader> pipeline, uint32_t expected) {
		Ref<Buffer> output;
		Ref<Buffer> readback;
		Expect(device.CreateBuffer({.size = 4, .usage = BufferUsage::Storage | BufferUsage::TransferSrc}, output) ==
				Result::Success,
			"Create cache output");
		Expect(device.CreateBuffer({.size = 4, .usage = BufferUsage::TransferDst, .memoryUsage = MemoryUsage::GPUToCPU},
				   readback) == Result::Success,
			"Create cache readback");
		Ref<CommandAllocator> allocator;
		Ref<CommandList> commands;
		Expect(device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success &&
				device.CreateCommandList(allocator.Raw(), commands) == Result::Success &&
				commands->Begin() == Result::Success,
			"Begin cached shader commands");
		commands->BindShaderProgram(pipeline.Raw());
		commands->UseResource(output.Raw());
		const uint32_t descriptor = output->GetDescriptorIndex();
		commands->SetPushConstants(&descriptor, sizeof(descriptor));
		commands->DispatchCompute(1, 1, 1);
		BarrierDesc barrier;
		barrier.buffers.push_back(
			{.buffer = output.Raw(),
				.before = {PipelineStage::ComputeShader, ResourceAccess::ShaderWrite},
				.after = {PipelineStage::Transfer, ResourceAccess::TransferRead},
				.size = 4});
		commands->Barrier(barrier);
		commands->CopyBuffer(output.Raw(), readback.Raw(), {.size = 4});
		Expect(commands->End() == Result::Success, "End cached shader commands");
		device.ClearShaderCache();
		pipeline.Reset();
		const auto token = device.GetGraphicsCommandQueue()->Submit(*commands);
		Expect(token.IsValid(), "Clearing caches preserves recorded shader");
		device.GetGraphicsCommandQueue()->Wait(token);
		uint32_t value = 0;
		Expect(readback->ReadData(&value, sizeof(value)) == Result::Success && value == expected,
			"Cached shader version produces the expected GPU result after cache clear");
	}

	void TestDevice() {
		std::unique_ptr<Device> device;
		Expect(Device::Create({.applicationName = "CacheTests",
								  .shaderDirectory = s_Directory,
								  .enableDebug = true,
								  .enableValidationLayers = true,
								  .debugCallback = Capture},
				   device) == Result::Success,
			"Create cache test device");
		Ref<Shader> shader;
		Ref<Shader> repeated;
		Expect(device->CreateShader(ComputeDesc(), shader) == Result::Success &&
				device->CreateShader(ComputeDesc(), repeated) == Result::Success && repeated == shader,
			"Device reuses shader program");
		std::array<Ref<Shader>, 4> shaders;
		std::array<std::thread, 4> workers;
		for (size_t i = 0; i < workers.size(); ++i) {
			workers[i] = std::thread([&, i] {
				Expect(device->CreateShader(ComputeDesc(), shaders[i]) == Result::Success,
					"Concurrent shader creation succeeds");
			});
		}
		for (auto& worker : workers) {
			worker.join();
		}
		for (auto& value : shaders) {
			Expect(value == shader, "Concurrent requests publish one shared shader");
		}

		Ref<Shader> first = shader;

		Write("cache_value.slangh", "#define CACHE_VALUE 29\n");
		Expect(device->CreateShader(ComputeDesc(), repeated) == Result::Success && repeated != shader,
			"Source edits select a new shader object");
		Ref<Shader> changed = repeated;
		DispatchAndVerify(*device, std::move(first), 17);
		DispatchAndVerify(*device, std::move(changed), 29);

		Ref<Shader> graphics;
		Expect(device->CreateShader(GraphicsDesc(), graphics) == Result::Success, "Compile graphics cache program");
		device->ClearShaderCache();
		Ref<Shader> afterClear;
		Expect(device->CreateShader(GraphicsDesc(), afterClear) == Result::Success && afterClear != graphics,
			"Explicit clear causes a fresh shader creation");
		device->WaitIdle();
		device->ClearShaderCache();
	}
}

int main() {
	std::filesystem::create_directories(s_Directory);
	const char* source = R"(
import cache_shared;
struct Parameters { uint output; };
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;
[require(spvDescriptorHeapEXT)]
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain() {
    RWStructuredBuffer<uint> output = RWStructuredBuffer<uint>.Handle(uint2(parameters.output, 0));
    output[0] = getValue();
}
[shader("vertex")]
float4 vertexMain(uint vertex : SV_VulkanVertexID) : SV_Position {
    float2 positions[3] = {float2(-1, -1), float2(1, -1), float2(0, 1)};
    return float4(positions[vertex], 0, 1);
}
[shader("fragment")]
float4 fragmentMain() : SV_Target0 { return float4(getValue() / 255.0, 0, 0, 1); }
)";
	const char* shared = "#include \"cache_value.slangh\"\npublic uint getValue() { return CACHE_VALUE; }\n";
	Write("cache.slang", source);
	Write("cache_shared.slang", shared);
	Write("cache_value.slangh", "#define CACHE_VALUE 17\n");
	TestCompiler(source);
	Write("cache.slang", source);
	Write("cache_shared.slang", shared);
	Write("cache_value.slangh", "#define CACHE_VALUE 17\n");
	TestDevice();
	Expect(s_Errors == s_ExpectedErrors, "Only expected failures produce diagnostics, including device cleanup");
	for (const char* name : {"cache.slang", "cache_shared.slang", "cache_value.slangh"}) {
		std::filesystem::remove(s_Directory / name);
	}
	std::filesystem::remove(s_Directory);
	std::cout << "CacheTests passed\n";
}
