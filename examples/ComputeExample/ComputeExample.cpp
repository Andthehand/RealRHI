#include "../ExampleSupport.h"
#include "Device.h"
#include "Queue.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>

int main() {
	RealRHI::DeviceDesc desc{};
	desc.applicationName = "RealRHI Compute Example";
	desc.shaderDirectory = std::filesystem::current_path() / "shaders";
	desc.enableValidationLayers = true;
	desc.enableDebug = true;
	desc.debugCallback = RealRHI::Examples::CaptureDebugMessage;

	std::unique_ptr<RealRHI::Device> device;
	if (RealRHI::Device::Create(desc, device) != RealRHI::Result::Success) {
		return 1;
	}

	RealRHI::Ref<RealRHI::Shader> shader;
	if (device->CreateShader({.moduleName = "compute"}, shader) != RealRHI::Result::Success) {
		std::cerr << "Compute shader compilation failed.\n";
		return 1;
	}

	RealRHI::Ref<RealRHI::Pipeline> pipeline;
	if (device->CreateComputePipeline({.shader = shader.Raw()}, pipeline) != RealRHI::Result::Success) {
		return 1;
	}

	constexpr uint32_t elementCount = 64;
	RealRHI::Ref<RealRHI::Buffer> output;
	RealRHI::BufferDesc outputDesc{
		.size = elementCount * sizeof(uint32_t),
		.usage = RealRHI::BufferUsage::Storage | RealRHI::BufferUsage::TransferSrc,
	};
	if (device->CreateBuffer(outputDesc, output) != RealRHI::Result::Success) {
		return 1;
	}

	RealRHI::Ref<RealRHI::Buffer> readback;
	RealRHI::BufferDesc readbackDesc{
		.size = outputDesc.size,
		.usage = RealRHI::BufferUsage::TransferDst,
		.memoryUsage = RealRHI::MemoryUsage::GPUToCPU,
	};
	if (device->CreateBuffer(readbackDesc, readback) != RealRHI::Result::Success) {
		return 1;
	}

	RealRHI::Ref<RealRHI::CommandAllocator> allocator;
	RealRHI::Ref<RealRHI::CommandList> list;
	if (device->CreateCommandAllocator(RealRHI::QueueType::Graphics, allocator) != RealRHI::Result::Success ||
		device->CreateCommandList(allocator.Raw(), list) != RealRHI::Result::Success ||
		list->Begin() != RealRHI::Result::Success) {
		return 1;
	}
	struct ComputeParameters {
		uint32_t outputIndex;
		uint32_t multiplier;
		uint32_t addend;
		uint32_t elementCount;
	};

	const ComputeParameters parameters{
		.outputIndex = output->GetDescriptorIndex(),
		.multiplier = 2,
		.addend = 1,
		.elementCount = elementCount,
	};

	list->BindPipeline(pipeline.Raw());
	list->UseResource(output.Raw());
	list->SetPushConstants(&parameters, sizeof(parameters));
	list->DispatchCompute(1, 1, 1);

	RealRHI::BarrierDesc barriers;
	barriers.buffers.push_back({
		.buffer = output.Raw(),
		.before = {RealRHI::PipelineStage::ComputeShader, RealRHI::ResourceAccess::ShaderWrite},
		.after = {RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferRead},
		.offset = 0,
		.size = outputDesc.size,
	});
	const RealRHI::BufferCopyRegion copy{.size = outputDesc.size};
	list->Barrier(barriers);
	list->CopyBuffer(output.Raw(), readback.Raw(), copy);
	if (list->End() != RealRHI::Result::Success) {
		return 1;
	}

	RealRHI::CommandQueue* queue = device->GetGraphicsCommandQueue();
	const RealRHI::SubmissionToken token = queue->Submit(*list);
	if (!token.IsValid()) {
		return 1;
	}
	queue->Wait(token);

	std::array<uint32_t, elementCount> values{};
	if (readback->ReadData(values.data(), sizeof(values)) != RealRHI::Result::Success) {
		return 1;
	}
	for (uint32_t i = 0; i < elementCount; ++i) {
		if (values[i] != 2 * i + 1) {
			std::cerr << "Compute output mismatch at index " << i << "\n";
			return 1;
		}
	}

	device->WaitIdle();
	// Check diagnostics after destroying resources and the device as well.
	list.Reset();
	allocator.Reset();
	readback.Reset();
	output.Reset();
	pipeline.Reset();
	shader.Reset();
	device.reset();
	if (RealRHI::Examples::s_ErrorCount != 0) {
		return 1;
	}
	std::cout << "ComputeExample passed\n";
	return 0;
}
