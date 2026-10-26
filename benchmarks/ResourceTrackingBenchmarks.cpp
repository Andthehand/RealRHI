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

	// Small ordinary RefCounted objects isolate RetainResource's CPU work from
	// descriptor allocation, shader declarations, and native resource creation.
	struct RetainedObject final : RefCounted { };

	void MeasureRetention(Device& device, uint32_t count) {
		std::vector<Ref<RetainedObject>> resources;
		resources.reserve(count);
		for (uint32_t i = 0; i < count; ++i) {
			resources.push_back(Ref<RetainedObject>::Create());
		}
		Ref<CommandAllocator> allocator;
		Ref<CommandList> commands;
		Check(device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success &&
				device.CreateCommandList(allocator.Raw(), commands) == Result::Success,
			"Create retention benchmark command list");
		const uint32_t rounds = std::max(1u, 1024u / count);
		std::vector<double> samples;
		for (uint32_t sample = 0; sample <= s_Samples; ++sample) {
			double milliseconds = 0;
			for (uint32_t round = 0; round < rounds; ++round) {
				Check(commands->Begin() == Result::Success, "Begin retention benchmark recording");
				const auto start = Clock::now();
				for (uint32_t sweep = 0; sweep < 8; ++sweep) {
					for (auto& resource : resources) {
						commands->RetainResource(resource.Raw());
					}
				}
				milliseconds += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
				Check(commands->End() == Result::Success && commands->Reset() == Result::Success,
					"Recycle retention benchmark recording");
			}
			if (sample != 0) {
				samples.push_back(milliseconds / rounds);
			}
		}
		Check(resources.front()->GetRefCount() == 1 && resources.back()->GetRefCount() == 1,
			"Retention benchmark releases every recorded reference");
		Report("retain_eight_sweeps", count, count * 8, std::move(samples));
	}

	void MeasureRanges(VulkanDevice& device, uint32_t count, std::string_view workload) {
		Ref<Buffer> buffer;
		Check(device.CreateBuffer({.size = count * s_RegionBytes,
									 .usage = BufferUsage::TransferSrc | BufferUsage::TransferDst}, buffer) == Result::Success,
			"Create range benchmark buffer");
		auto* native = static_cast<VulkanBuffer*>(buffer.Raw());
		std::vector<VulkanResourceOperation> operations;
		operations.reserve(count);
		for (uint32_t i = 0; i < count; ++i) {
			uint64_t offset = i * s_RegionBytes;
			uint64_t size = s_RegionBytes;
			if (workload == "whole_buffer") {
				offset = 0;
				size = buffer->GetSize();
			}
			else if (workload == "scattered_ranges") {
				// All tested counts are powers of two; the odd multiplier visits every region.
				offset = ((uint64_t{i} * 4051) % count) * s_RegionBytes;
			}
			operations.push_back({.name = "Benchmark buffer use", .buffer = native, .offset = offset, .size = size});
		}
		const SubmissionToken proposed{.queue = device.GetGraphicsCommandQueue()->GetQueueId(), .value = 1};
		const uint32_t rounds = std::max(1u, 1024u / count);
		std::vector<double> samples;
		for (uint32_t sample = 0; sample <= s_Samples; ++sample) {
			const auto start = Clock::now();
			for (uint32_t round = 0; round < rounds; ++round) {
				std::scoped_lock lock(device.GetStateTrackingMutex());
				VulkanResourceStateTransaction transaction(&device, proposed, {});
				Check(transaction.Validate(operations), "Validate range benchmark operations");
				// Discard the prediction: no native work was submitted, so committing
				// would publish an ownership state that the GPU never executed.
			}
			const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / rounds;
			if (sample != 0) {
				samples.push_back(milliseconds);
			}
		}
		Report(workload, 1, count, std::move(samples));
	}
}

int main() {
	std::unique_ptr<Device> device;
	Check(Device::Create({.applicationName = "Resource tracking CPU benchmarks",
							.shaderDirectory = ".",
							.enableDebug = false,
							.enableProfiling = false,
							.enableValidationLayers = false,
							.debugCallback = CaptureDebugMessage}, device) == Result::Success,
		"Create benchmark device");
	std::cerr << "Device: " << device->GetCapabilities().name << "; one warmup, " << s_Samples
		<< " measured samples; native allocations, begin/end/reset, and GPU execution excluded from retention timings; "
		<< "range timings include prediction copy/split/destruction without commit or native submit.\n";
	std::cout << std::fixed << std::setprecision(6)
		<< "workload,resources,operations,median_ms,min_ms,max_ms\n";
	for (uint32_t count : {1u, 8u, 32u, 128u, 1024u, 8192u}) {
		MeasureRetention(*device, count);
	}
	auto& native = *static_cast<VulkanDevice*>(device.get());
	for (std::string_view workload : {"whole_buffer", "sequential_ranges", "scattered_ranges"}) {
		for (uint32_t count : {1u, 8u, 32u, 128u, 1024u, 8192u}) {
			MeasureRanges(native, count, workload);
		}
	}
	device->WaitIdle();
}
