#pragma once
#include "BarrierDesc.h"
#include "Queue.h"
#include "Result.h"

#include <cstdint>
#include <optional>
#include <span>

namespace RealRHI {
	struct UploadContextDesc {
		QueueType queueType = QueueType::Transfer;
		uint64_t stagingPageSize = 4 * 1024 * 1024;
	};

	struct BufferUploadDesc {
		Buffer* destination = nullptr;
		uint64_t destinationOffset = 0;
		ResourceState before{PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite};
		// Set when acquiring a range previously released by another queue.
		std::optional<QueueType> sourceQueue;
		ResourceState finalState{PipelineStage::AllCommands, ResourceAccess::MemoryRead};
		QueueType finalQueue = QueueType::Graphics;
	};

	struct TextureUploadDesc {
		Texture* destination = nullptr;
		uint32_t mipLevel = 0;
		uint32_t baseArrayLayer = 0;
		uint32_t layerCount = 1;
		TextureLayout oldLayout = TextureLayout::Undefined;
		ResourceState before{PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite};
		// A source release must use oldLayout -> TransferDst for this exact range.
		std::optional<QueueType> sourceQueue;
		TextureLayout finalLayout = TextureLayout::ShaderRead;
		ResourceState finalState{PipelineStage::AllCommands, ResourceAccess::MemoryRead};
		QueueType finalQueue = QueueType::Graphics;
	};

	struct UploadSubmission {
		SubmissionToken completion{};
		// Non-owning resource pointers. Record these on the consumer before use,
		// and submit it with completion in waitFor. Empty when no family transfer is needed.
		// For multiple consumers, group these by destinationQueue before recording.
		BarrierDesc acquireBarriers;
	};

	// Device must outlive the context. Serialize calls to each context.
	class UploadContext {
	public:
		virtual ~UploadContext() = default;

		virtual Result BeginBatch() = 0;
		// Input bytes are copied before returning. Ranges in one batch must not overlap.
		virtual Result UploadBuffer(const BufferUploadDesc& desc, const void* data, uint64_t size) = 0;
		// Uploads tightly packed color data for a complete mip and selected array layers.
		virtual Result UploadTexture(const TextureUploadDesc& desc, const void* data, uint64_t size) = 0;
		// Does not wait on the CPU. Failed submissions can be retried or cancelled.
		virtual Result Submit(UploadSubmission& outSubmission, std::span<const SubmissionToken> waitFor = {}) = 0;
		virtual Result CancelBatch() = 0;
		// BeginBatch also polls completed batches. This never waits for the GPU.
		virtual Result ProcessCompleted() = 0;
	};
}
