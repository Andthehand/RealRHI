#include "Device.h"
#include "Vulkan/VulkanBuffer.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanResourceStateTransaction.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string_view>
#include <vector>

namespace {
	using namespace RealRHI;
	using Clock = std::chrono::steady_clock;
	constexpr uint32_t s_Samples = 7;
	constexpr uint64_t s_RegionBytes = 16;

	void Check(bool condition, const char* message) {
		if (!condition) {
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void CaptureDebugMessage(const DebugMessage& message) {
		DefaultDebugCallback(message);
		Check(message.severity != DebugSeverity::Error, "Benchmark encountered an RHI error");
	}

	void Report(std::string_view workload, uint32_t resources, uint32_t operations, std::vector<double> samples) {
		std::sort(samples.begin(), samples.end());
		std::cout << workload << ',' << resources << ',' << operations << ',' << samples[samples.size() / 2] << ','
			<< samples.front() << ',' << samples.back() << '\n';
	}

	struct RetainedObject final : RefCounted { };

	void MeasureRetentionPattern(Device& device, uint32_t operations, std::string_view workload) {
		uint32_t resourceCount = operations;
		if (workload == "retain_duplicate") {
			resourceCount = 1;
		}
		else if (workload == "retain_cycle_16") {
			resourceCount = std::min(operations, 16u);
		}

		std::vector<Ref<RetainedObject>> resources;
		resources.reserve(resourceCount);
		for (uint32_t i = 0; i < resourceCount; ++i) {
			resources.push_back(Ref<RetainedObject>::Create());
		}

		Ref<CommandAllocator> allocator;
		Ref<CommandList> commands;
		Check(device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success &&
				device.CreateCommandList(allocator.Raw(), commands) == Result::Success,
			"Create retention-pattern benchmark command list");

		const uint32_t rounds = std::max(1u, 65536u / operations);
		std::vector<double> samples;
		for (uint32_t sample = 0; sample <= s_Samples; ++sample) {
			double milliseconds = 0;
			for (uint32_t round = 0; round < rounds; ++round) {
				Check(commands->Begin() == Result::Success, "Begin retention-pattern benchmark recording");
				const auto start = Clock::now();
				for (uint32_t operation = 0; operation < operations; ++operation) {
					commands->RetainResource(resources[operation % resourceCount].Raw());
				}
				milliseconds += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
				Check(commands->End() == Result::Success && commands->Reset() == Result::Success,
					"Recycle retention-pattern benchmark recording");
			}
			if (sample != 0) {
				samples.push_back(milliseconds / rounds);
			}
		}

		Report(workload, resourceCount, operations, std::move(samples));
	}

	void MeasureReverseRanges(VulkanDevice& device, uint32_t count) {
		Ref<Buffer> buffer;
		Check(device.CreateBuffer({.size = count * s_RegionBytes,
								 .usage = BufferUsage::TransferSrc | BufferUsage::TransferDst}, buffer) == Result::Success,
			"Create reverse-range benchmark buffer");

		auto* native = static_cast<VulkanBuffer*>(buffer.Raw());
		std::vector<VulkanResourceOperation> operations;
		operations.reserve(count);
		for (uint32_t i = 0; i < count; ++i) {
			operations.push_back({.name = "Reverse range benchmark",
				.buffer = native,
				.offset = (count - i - 1) * s_RegionBytes,
				.size = s_RegionBytes});
		}

		const SubmissionToken proposed{.queue = device.GetGraphicsCommandQueue()->GetQueueId(), .value = 1};
		const uint32_t rounds = std::max(1u, 1024u / count);
		std::vector<double> samples;
		for (uint32_t sample = 0; sample <= s_Samples; ++sample) {
			const auto start = Clock::now();
			for (uint32_t round = 0; round < rounds; ++round) {
				std::scoped_lock lock(device.GetStateTrackingMutex());
				VulkanResourceStateTransaction transaction(&device, proposed, {});
				Check(transaction.Validate(operations), "Validate reverse-range benchmark operations");
			}
			const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / rounds;
			if (sample != 0) {
				samples.push_back(milliseconds);
			}
		}

		Report("reverse_ranges", 1, count, std::move(samples));
	}

	void MeasureInterleavedBuffers(VulkanDevice& device, uint32_t count) {
		const uint32_t bufferCount = std::min(count, 64u);
		const uint32_t regionsPerBuffer = (count + bufferCount - 1) / bufferCount;
		std::vector<Ref<Buffer>> buffers;
		buffers.reserve(bufferCount);
		for (uint32_t i = 0; i < bufferCount; ++i) {
			Ref<Buffer> buffer;
			Check(device.CreateBuffer({.size = regionsPerBuffer * s_RegionBytes,
									 .usage = BufferUsage::TransferSrc | BufferUsage::TransferDst}, buffer) == Result::Success,
				"Create interleaved-buffer benchmark buffer");
			buffers.push_back(std::move(buffer));
		}

		std::vector<VulkanResourceOperation> operations;
		operations.reserve(count);
		for (uint32_t i = 0; i < count; ++i) {
			const uint32_t bufferIndex = i % bufferCount;
			operations.push_back({.name = "Interleaved buffer benchmark",
				.buffer = static_cast<VulkanBuffer*>(buffers[bufferIndex].Raw()),
				.offset = (i / bufferCount) * s_RegionBytes,
				.size = s_RegionBytes});
		}

		const SubmissionToken proposed{.queue = device.GetGraphicsCommandQueue()->GetQueueId(), .value = 1};
		const uint32_t rounds = std::max(1u, 1024u / count);
		std::vector<double> samples;
		for (uint32_t sample = 0; sample <= s_Samples; ++sample) {
			const auto start = Clock::now();
			for (uint32_t round = 0; round < rounds; ++round) {
				std::scoped_lock lock(device.GetStateTrackingMutex());
				VulkanResourceStateTransaction transaction(&device, proposed, {});
				Check(transaction.Validate(operations), "Validate interleaved-buffer benchmark operations");
			}
			const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / rounds;
			if (sample != 0) {
				samples.push_back(milliseconds);
			}
		}

		Report("interleaved_buffers", bufferCount, count, std::move(samples));
	}
}

int main() {
	std::unique_ptr<Device> device;
	Check(Device::Create({.applicationName = "Resource tracking pattern benchmarks",
							.shaderDirectory = ".",
							.enableDebug = false,
							.enableProfiling = false,
							.enableValidationLayers = false,
							.debugCallback = CaptureDebugMessage}, device) == Result::Success,
		"Create benchmark device");

	std::cerr << "Device: " << device->GetCapabilities().name << "; one warmup, " << s_Samples
		<< " measured samples; native allocations, command-list recycling, and GPU execution excluded.\n";
	std::cout << std::fixed << std::setprecision(6)
		<< "workload,resources,operations,median_ms,min_ms,max_ms\n";

	for (std::string_view workload : {"retain_unique", "retain_duplicate", "retain_cycle_16"}) {
		for (uint32_t operations : {1u, 8u, 32u, 128u, 1024u, 8192u, 65536u}) {
			MeasureRetentionPattern(*device, operations, workload);
		}
	}

	auto& native = *static_cast<VulkanDevice*>(device.get());
	for (uint32_t count : {1u, 8u, 32u, 128u, 1024u, 8192u}) {
		MeasureReverseRanges(native, count);
		MeasureInterleavedBuffers(native, count);
	}

	device->WaitIdle();
}
