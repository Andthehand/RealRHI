#pragma once
#include <cstdint>

namespace RealRHI {

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

	struct DeviceStatistics {
		// Owned native allocations, including internal staging and descriptor heap buffers.
		// External swapchain images are excluded. Bytes are VMA allocation sizes.
		ResourceStatistics buffers;
		ResourceStatistics textures;
		// Live compiled shader stages (a graphics program can own multiple stages).
		ResourceStatistics shaders;
		DescriptorStatistics descriptors;

		// Successful staging includes cancelled batches; submission counts only successful batches.
		uint64_t stagedUploadBytes = 0;
		uint64_t submittedUploadBytes = 0;
		uint64_t uploadBatchCount = 0;
	};
}
