#include "Device.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/TransientResourcePool.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
	using namespace RealRHI;
	uint32_t s_Errors = 0;
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
		}
		DefaultDebugCallback(message);
	}

	void CreateCommands(Device& device, QueueType queue, Ref<CommandAllocator>& allocator, Ref<CommandList>& commands) {
		Expect(device.CreateCommandAllocator(queue, allocator) == Result::Success &&
				device.CreateCommandList(allocator.Raw(), commands) == Result::Success,
			"Create pool recording commands");
		Expect(commands->GetQueueId() == device.GetCommandQueue(queue)->GetQueueId(),
			"Command list exposes its recording queue");
	}

	void TestGpuReuse(Device& device) {
		TransientResourcePool pool(device);
		std::array<uint32_t, 4> initial;
		initial.fill(0xff00ff00);
		Ref<Buffer> upload;
		Expect(device.CreateBuffer({.size = sizeof(initial),
									   .usage = BufferUsage::TransferSrc,
									   .memoryUsage = MemoryUsage::CPUToGPU,
									   .initialData = initial.data()},
				   upload) == Result::Success,
			"Create pool upload");
		std::array<Ref<Buffer>, 2> outputs;
		std::array<Ref<CommandAllocator>, 2> allocators;
		std::array<Ref<CommandList>, 2> commands;
		std::array<SubmissionToken, 2> tokens;
		for (size_t i = 0; i < commands.size(); ++i) {
			Expect(device.CreateBuffer({.size = sizeof(initial) * 2,
										   .usage = BufferUsage::TransferDst,
										   .memoryUsage = MemoryUsage::GPUToCPU},
					   outputs[i]) == Result::Success,
				"Create pool readback");
			CreateCommands(device, QueueType::Graphics, allocators[i], commands[i]);
		}
		const auto baseline = device.GetStatistics();
		auto checkOutput = [&](size_t slot) {
			device.GetGraphicsCommandQueue()->Wait(tokens[slot]);
			std::array<uint32_t, 8> values{};
			Expect(
				outputs[slot]->ReadData(values.data(), sizeof(values)) == Result::Success, "Read pooled GPU outputs");
			for (size_t i = 0; i < initial.size(); ++i) {
				Expect(values[i] == initial[i] && values[i + 4] == initial[i],
					"Reused textures and buffers contain the current execution's data");
			}
		};
		Texture* originalTexture = nullptr;
		Buffer* originalBuffer = nullptr;
		for (size_t frame = 0; frame < 4; ++frame) {
			const size_t slot = frame % 2;
			if (frame == 2) {
				for (size_t i = 0; i < 2; ++i) {
					checkOutput(i);
				}
				Expect(pool.GetStatistics().idleTextureCount == 0 && pool.GetStatistics().idleBufferCount == 0,
					"Completed executable recordings still reserve their allocations for possible replay");
				for (auto& list : commands) {
					Expect(list->Reset() == Result::Success, "Release completed recordings before recycling");
				}
			}
			if (frame >= 2) {
				initial.fill(0xffff0000 + static_cast<uint32_t>(frame));
				upload->WriteData(initial.data(), sizeof(initial));
			}
			Expect(commands[slot]->Begin() == Result::Success, "Begin pooled execution");
			{
				RenderGraph graph(Capture);
				const auto source = graph.ImportBuffer(upload.Raw(), s_Read);
				const auto texture = graph.CreateTexture(
					{.width = 2,
						.height = 2,
						.format = TextureFormat::RGBA8_UNorm,
						.usage = TextureUsage::ShaderResource | TextureUsage::TransferDst | TextureUsage::TransferSrc});
				const auto buffer = graph.CreateBuffer(
					{.size = sizeof(initial), .usage = BufferUsage::TransferDst | BufferUsage::TransferSrc});
				const auto output = graph.ImportBuffer(outputs[slot].Raw(), s_Write);
				graph.AddPass({.name = "Initialize",
								  .resources =
									  {{source, s_Read},
										  {texture, s_Write, TextureLayout::TransferDst, RenderGraphAccess::Write},
										  {buffer, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
					[&](CommandList& list) {
						list.CopyBufferToTexture(upload.Raw(), graph.GetTexture(texture), {.width = 2, .height = 2});
						list.CopyBuffer(upload.Raw(), graph.GetBuffer(buffer), {.size = sizeof(initial)});
					});
				graph.AddPass({.name = "Readback",
								  .resources =
									  {{texture, s_Read, TextureLayout::TransferSrc},
										  {buffer, s_Read},
										  {output, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
					[&](CommandList& list) {
						list.CopyTextureToBuffer(
							graph.GetTexture(texture), outputs[slot].Raw(), {.width = 2, .height = 2});
						list.CopyBuffer(graph.GetBuffer(buffer),
							outputs[slot].Raw(),
							{.destinationOffset = sizeof(initial), .size = sizeof(initial)});
					});
				Expect(graph.Execute(device, *commands[slot], &pool) == Result::Success,
					"Execute with the transient pool");
				if (frame == 0) {
					originalTexture = graph.GetTexture(texture);
					originalBuffer = graph.GetBuffer(buffer);
				}
				else if (frame == 1) {
					Expect(graph.GetTexture(texture) != originalTexture && graph.GetBuffer(buffer) != originalBuffer,
						"An in-flight execution never shares allocations with the next execution");
				}
				else if (frame == 2) {
					Expect(graph.GetTexture(texture) == originalTexture && graph.GetBuffer(buffer) == originalBuffer,
						"Reset completed recordings make the original allocations reusable");
				}
			}
			Expect(commands[slot]->End() == Result::Success, "End pooled execution");
			tokens[slot] = device.GetGraphicsCommandQueue()->Submit(*commands[slot]);
			Expect(tokens[slot].IsValid(), "Submit pooled execution");
			if (frame == 1) {
				pool.Trim();
				Expect(pool.GetStatistics().textureCount == 2 && pool.GetStatistics().bufferCount == 2,
					"Trimming cannot release allocations retained by submitted recordings");
			}
			if (frame >= 2) {
				checkOutput(slot);
				Expect(commands[slot]->Reset() == Result::Success, "Release reused execution");
			}
		}
		const auto statistics = pool.GetStatistics();
		Expect(statistics.textureAllocations == 2 && statistics.bufferAllocations == 2 &&
				statistics.textureReuses == 2 && statistics.bufferReuses == 2 && statistics.idleTextureCount == 2 &&
				statistics.idleBufferCount == 2,
			"Warm executions reuse native allocations rather than growing the pool");
		pool.Trim();
		Expect(pool.GetStatistics().textureCount == 0 && pool.GetStatistics().bufferCount == 0,
			"Trim releases idle entries");
		Expect(device.GetStatistics().textures.liveCount == baseline.textures.liveCount &&
				device.GetStatistics().buffers.liveCount == baseline.buffers.liveCount,
			"Trimming returns native resources to baseline");
	}

	void TestRetainedOutputs(Device& device) {
		Ref<CommandAllocator> allocator;
		Ref<CommandList> commands;
		CreateCommands(device, QueueType::Graphics, allocator, commands);
		for (uint32_t mode = 0; mode < 3; ++mode) {
			TransientResourcePool pool(device);
			Ref<Texture> exportedTexture;
			Ref<TextureView> exportedView;
			Texture* original = nullptr;
			for (uint32_t execution = 0; execution < 2; ++execution) {
				Expect(commands->Begin() == Result::Success, "Begin exported resource recording");
				{
					RenderGraph graph(Capture);
					const auto texture = graph.CreateTexture(
						{.width = 8,
							.height = 8,
							.format = TextureFormat::RGBA8_UNorm,
							.usage = TextureUsage::ShaderResource | TextureUsage::TransferDst});
					graph.AddPass(
						{.name = "Reserve texture",
							.resources = {{texture, s_Write, TextureLayout::TransferDst, RenderGraphAccess::Write}}},
						[](CommandList&) { });
					Expect(graph.Execute(device, *commands, &pool) == Result::Success, "Allocate exported resource");
					if (execution == 0) {
						original = graph.GetTexture(texture);
						if (mode == 0) {
							exportedTexture = Ref<Texture>(original);
						}
						else if (mode == 1) {
							Expect(
								device.CreateTextureView(
									original, {.usage = TextureUsage::ShaderResource}, exportedView) == Result::Success,
								"Create an exported custom view");
						}
						else {
							exportedView = Ref<TextureView>(original->GetTextureView());
						}
					}
					else {
						Expect(graph.GetTexture(texture) != original,
							"Exported textures and both view kinds prevent reuse");
					}
				}
				Expect(commands->End() == Result::Success && commands->Reset() == Result::Success,
					"Release unsubmitted recording references");
				if (execution == 0) {
					Expect(pool.GetStatistics().idleTextureCount == 0, "Exported references keep an allocation busy");
				}
			}
			pool.Trim();
			Expect(pool.GetStatistics().textureCount == 1, "Trim preserves an externally retained output");
			exportedView.Reset();
			exportedTexture.Reset();
			pool.Trim();
			Expect(
				pool.GetStatistics().textureCount == 0, "Releasing exported references makes the last entry trimmable");
		}
	}

	void TestMatchingAndQueues(Device& device) {
		TransientResourcePool pool(device);
		Ref<CommandAllocator> graphicsAllocator;
		Ref<CommandList> graphics;
		Ref<CommandAllocator> computeAllocator;
		Ref<CommandList> compute;
		CreateCommands(device, QueueType::Graphics, graphicsAllocator, graphics);
		CreateCommands(device, QueueType::Compute, computeAllocator, compute);
		auto textureAllocation = [&](const TextureDesc& desc, CommandList& commands) {
			Expect(commands.Begin() == Result::Success, "Begin descriptor matching recording");
			Texture* result = nullptr;
			{
				RenderGraph graph(Capture);
				const auto handle = graph.CreateTexture(desc);
				graph.AddPass({.name = "Texture",
								  .resources = {{handle, s_Write, TextureLayout::General, RenderGraphAccess::Write}}},
					[](CommandList&) { });
				Expect(graph.Execute(device, commands, &pool) == Result::Success, "Acquire matching texture");
				result = graph.GetTexture(handle);
			}
			Expect(commands.End() == Result::Success && commands.Reset() == Result::Success,
				"Release matching texture recording");
			return result;
		};
		auto bufferAllocation = [&](const BufferDesc& desc, CommandList& commands) {
			Expect(commands.Begin() == Result::Success, "Begin buffer matching recording");
			Buffer* result = nullptr;
			{
				RenderGraph graph(Capture);
				const auto handle = graph.CreateBuffer(desc);
				graph.AddPass({.name = "Buffer",
								  .resources = {{handle, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write}}},
					[](CommandList&) { });
				Expect(graph.Execute(device, commands, &pool) == Result::Success, "Acquire matching buffer");
				result = graph.GetBuffer(handle);
			}
			Expect(commands.End() == Result::Success && commands.Reset() == Result::Success,
				"Release matching buffer recording");
			return result;
		};
		const TextureDesc textureDesc{
			.width = 8,
			.height = 8,
			.format = TextureFormat::RGBA8_UNorm,
			.usage = TextureUsage::ShaderResource | TextureUsage::TransferDst
		};
		auto* texture = textureAllocation(textureDesc, *graphics);
		auto changedTexture = textureDesc;
		changedTexture.debugName = "Different debug name";
		Expect(textureAllocation(changedTexture, *graphics) == texture, "Debug names do not affect compatibility");
		std::array<TextureDesc, 7> textureVariants;
		textureVariants.fill(textureDesc);
		textureVariants[0].width = 16;
		textureVariants[1].height = 16;
		textureVariants[2].depth = 2;
		textureVariants[3].mipLevels = 2;
		textureVariants[4].arrayLayers = 2;
		textureVariants[5].format = TextureFormat::BGRA8_UNorm;
		textureVariants[6].usage = textureDesc.usage | TextureUsage::TransferSrc;
		for (const auto& variant : textureVariants) {
			Expect(textureAllocation(variant, *graphics) != texture,
				"Every image compatibility field participates in matching");
		}
		const BufferDesc bufferDesc{.size = 64, .usage = BufferUsage::TransferDst};
		auto* buffer = bufferAllocation(bufferDesc, *graphics);
		auto changedBuffer = bufferDesc;
		changedBuffer.debugName = "Different buffer name";
		Expect(bufferAllocation(changedBuffer, *graphics) == buffer, "Buffer names do not affect compatibility");
		std::array<BufferDesc, 3> bufferVariants;
		bufferVariants.fill(bufferDesc);
		bufferVariants[0].size = 128;
		bufferVariants[1].usage = BufferUsage::TransferDst | BufferUsage::TransferSrc;
		bufferVariants[2].memoryUsage = MemoryUsage::CPUToGPU;
		for (const auto& variant : bufferVariants) {
			Expect(bufferAllocation(variant, *graphics) != buffer,
				"Size, usage, and memory class participate in buffer matching");
		}
		const bool sameQueue = graphics->GetQueueId() == compute->GetQueueId();
		Expect((textureAllocation(textureDesc, *compute) == texture) == sameQueue &&
				(bufferAllocation(bufferDesc, *compute) == buffer) == sameQueue,
			"Pools reuse across queue roles only when they resolve to the same native queue");
	}
}

int main() {
	std::unique_ptr<RealRHI::Device> device;
	Expect(RealRHI::Device::Create({.applicationName = "TransientResourcePoolTests",
									   .enableDebug = true,
									   .enableValidationLayers = true,
									   .debugCallback = Capture},
			   device) == RealRHI::Result::Success,
		"Create pool test device");
	TestGpuReuse(*device);
	TestRetainedOutputs(*device);
	TestMatchingAndQueues(*device);
	device->WaitIdle();
	device.reset();
	Expect(s_Errors == 0, "Pooling and cleanup produce no validation errors");
	std::cout << "TransientResourcePoolTests passed\n";
}
