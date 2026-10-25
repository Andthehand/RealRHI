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

	void TestPipelineKeys(Device& device, Shader* shader) {
		PipelineDesc desc{
			.shader = shader,
			.rasterState = {.cullMode = CullMode::None},
			.depthState = {.depthTestEnable = false, .depthWriteEnable = false},
			.renderTargetFormats = {
				.colorFormats = {TextureFormat::RGBA8_UNorm}, .depthFormat = TextureFormat::D32_Float
			}
		};
		Ref<Pipeline> first;
		Ref<Pipeline> repeated;
		Expect(device.CreateGraphicsPipeline(desc, first) == Result::Success, "Create base graphics pipeline");
		desc.debugName = "Ignored cache name";
		desc.vertexLayout = shader->GetReflection().vertexLayout;
		desc.colorBlendStates = {desc.blendState};
		Expect(device.CreateGraphicsPipeline(desc, repeated) == Result::Success && repeated == first,
			"Equivalent effective vertex/blend state and different debug names reuse pipeline");

		const std::vector<std::function<void(PipelineDesc&)>> changes{
			[](auto& d) {
				d.rasterState.cullMode = CullMode::Back;
			},
			[](auto& d) {
				d.rasterState.frontCounterClockwise = true;
			},
			[](auto& d) {
				d.rasterState.depthBias.enable = true;
			},
			[](auto& d) {
				d.rasterState.depthBias.constantFactor = 1;
			},
			[](auto& d) {
				d.rasterState.depthBias.slopeFactor = 1;
			},
			[](auto& d) {
				d.depthState.depthTestEnable = true;
			},
			[](auto& d) {
				d.depthState.depthWriteEnable = true;
			},
			[](auto& d) {
				d.depthState.compareOp = CompareOp::Always;
			},
			[](auto& d) {
				d.topology = PrimitiveTopology::TriangleStrip;
			},
			[](auto& d) {
				d.colorBlendStates[0].enable = true;
			},
			[](auto& d) {
				d.colorBlendStates[0].sourceColorFactor = BlendFactor::One;
			},
			[](auto& d) {
				d.colorBlendStates[0].destinationColorFactor = BlendFactor::Zero;
			},
			[](auto& d) {
				d.colorBlendStates[0].colorOperation = BlendOperation::Max;
			},
			[](auto& d) {
				d.colorBlendStates[0].sourceAlphaFactor = BlendFactor::Zero;
			},
			[](auto& d) {
				d.colorBlendStates[0].destinationAlphaFactor = BlendFactor::One;
			},
			[](auto& d) {
				d.colorBlendStates[0].alphaOperation = BlendOperation::Min;
			},
			[](auto& d) {
				d.colorBlendStates[0].writeMask = ColorWriteMask::Red;
			},
			[](auto& d) {
				d.blendConstants[2] = 0.5f;
			},
			[](auto& d) {
				d.renderTargetFormats.colorFormats[0] = TextureFormat::BGRA8_UNorm;
			},
			[](auto& d) {
				d.renderTargetFormats.depthFormat = TextureFormat::Unknown;
			},
		};
		for (const auto& change : changes) {
			auto changed = desc;
			change(changed);
			Expect(device.CreateGraphicsPipeline(changed, repeated) == Result::Success && repeated != first,
				"Changed graphics state selects a different pipeline");
		}

		auto invalid = desc;
		invalid.colorBlendStates.resize(2);
		++s_ExpectedErrors;
		Expect(device.CreateGraphicsPipeline(invalid, repeated) == Result::InvalidParameter,
			"Mismatched blend array does not hit an otherwise equivalent pipeline");
		invalid = desc;
		invalid.topology = PrimitiveTopology::TriangleStrip;
		invalid.primitiveRestartEnable = true;
		Expect(device.CreateGraphicsPipeline(invalid, repeated) == Result::Success, "Cache primitive restart state");
		invalid.primitiveRestartEnable = false;
		Ref<Pipeline> withoutRestart;
		Expect(device.CreateGraphicsPipeline(invalid, withoutRestart) == Result::Success && repeated != withoutRestart,
			"Restart flag distinguishes pipelines");
	}

	void DispatchAndVerify(Device& device, Ref<Pipeline> pipeline, uint32_t expected) {
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
			"Begin cached pipeline commands");
		commands->BindPipeline(pipeline.Raw());
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
		Expect(commands->End() == Result::Success, "End cached pipeline commands");
		device.ClearShaderAndPipelineCaches();
		pipeline.Reset();
		const auto token = device.GetGraphicsCommandQueue()->Submit(*commands);
		Expect(token.IsValid(), "Clearing caches preserves recorded pipeline");
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
								  .debugCallback = Capture,
								  .enableProfiling = true},
				   device) == Result::Success,
			"Create cache test device");
		Ref<Shader> shader;
		Ref<Shader> repeated;
		Expect(device->CreateShader(ComputeDesc(), shader) == Result::Success &&
				device->CreateShader(ComputeDesc(), repeated) == Result::Success && repeated == shader,
			"Device reuses shader module");
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

		Ref<Pipeline> first;
		Ref<Pipeline> hit;
		Expect(device->CreateComputePipeline({.shader = shader.Raw()}, first) == Result::Success,
			"Create cached compute pipeline");
		const auto before = device->GetStatistics();
		Expect(device->CreateComputePipeline({.shader = shader.Raw(), .debugName = "Another name"}, hit) ==
					Result::Success &&
				hit == first,
			"Device reuses compute pipeline independently of debug name");
		const auto after = device->GetStatistics();
		Expect(after.computePipelineCreation.sampleCount == before.computePipelineCreation.sampleCount &&
				after.pipelines.liveCount == before.pipelines.liveCount,
			"Cache hits create no native pipeline or timing sample");

		Write("cache_value.slangh", "#define CACHE_VALUE 29\n");
		Expect(device->CreateShader(ComputeDesc(), repeated) == Result::Success && repeated != shader,
			"Source edits select a new shader object");
		Ref<Pipeline> changed;
		Expect(
			device->CreateComputePipeline({.shader = repeated.Raw()}, changed) == Result::Success && changed != first,
			"New shader version selects a new pipeline");
		hit.Reset();
		DispatchAndVerify(*device, std::move(first), 17);
		DispatchAndVerify(*device, std::move(changed), 29);

		Ref<Shader> graphics;
		Expect(device->CreateShader(GraphicsDesc(), graphics) == Result::Success, "Compile graphics cache program");
		TestPipelineKeys(*device, graphics.Raw());
		device->ClearShaderAndPipelineCaches();
		Expect(device->GetStatistics().pipelines.liveCount == 0, "Clearing unused caches releases native pipelines");
		Ref<Shader> afterClear;
		Expect(device->CreateShader(GraphicsDesc(), afterClear) == Result::Success && afterClear != graphics,
			"Explicit clear causes a fresh shader creation");
		device->WaitIdle();
		device->ClearShaderAndPipelineCaches();
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
