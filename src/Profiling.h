#pragma once
#include "Queue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RealRHI {
	struct TimingStatistics {
		uint64_t sampleCount = 0;
		double totalMilliseconds = 0;
		double maximumMilliseconds = 0;
	};

	struct ResourceStatistics {
		uint64_t liveCount = 0;
		uint64_t peakCount = 0;
		uint64_t liveBytes = 0;
		uint64_t peakBytes = 0;
	};

	struct DescriptorStatistics {
		uint64_t bufferCount = 0;
		uint64_t imageCount = 0;
		// Unique live sampler descriptor slots, including those shared by several sampler objects.
		uint64_t samplerCount = 0;
		uint64_t resourceBytes = 0;
		// Cursor reservation includes freed slots retained for reuse; it can exceed live usage.
		uint64_t resourceReservedBytes = 0;
		uint64_t peakResourceBytes = 0;
		uint64_t resourceCapacityBytes = 0;
		uint64_t samplerCapacity = 0;
		uint64_t peakSamplerCount = 0;
	};

	struct GpuTimestamp {
		std::string name;
		// Relative to the first marker in this recording, after timestampPeriod conversion.
		double milliseconds = 0;
	};

	struct GpuTimingStatistics {
		std::string name;
		QueueId queue;
		TimingStatistics timing;
	};

	struct DeviceStatistics {
		// Owned native allocations, including internal staging and descriptor heap buffers.
		// External swapchain images are excluded. Bytes are VMA allocation sizes.
		ResourceStatistics buffers;
		ResourceStatistics textures;
		ResourceStatistics pipelines;
		DescriptorStatistics descriptors;

		TimingStatistics uploadStaging;
		TimingStatistics submissions;
		// Nested CPU portions of submissions; these are not GPU execution times.
		// Validation measures resource-log replay after locking, excluding state commit.
		TimingStatistics resourceStateValidation;
		TimingStatistics nativeSubmissions;
		TimingStatistics graphicsPipelineCreation;
		TimingStatistics computePipelineCreation;
		uint64_t stagedUploadBytes = 0;
		uint64_t submittedUploadBytes = 0;
		uint64_t uploadBatchCount = 0;

		// Explicit WriteTimestamp markers contribute adjacent intervals. Paired GPU timings
		// contribute their complete begin/end duration, independently of intervening markers.
		std::vector<GpuTimingStatistics> gpuTimings;
	};
}
