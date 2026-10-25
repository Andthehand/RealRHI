#pragma once
#include "UploadContext.h"
#include "VulkanBuffer.h"
#include "VulkanDevice.h"

#include <vector>

namespace RealRHI {
	class VulkanUploadContext final : public UploadContext {
	private:
		struct StagingPage {
			Ref<Buffer> buffer;
			uint64_t bytesUsed = 0;
			bool inUse = false;
		};

		struct BatchResources {
			Ref<CommandAllocator> allocator;
			Ref<CommandList> commandList;
			std::vector<size_t> stagingPageIndices;
			SubmissionToken completion{};
		};

		struct StagingAllocation {
			VulkanBuffer* buffer = nullptr;
			uint64_t offset = 0;
		};

		struct TextureUploadInfo {
			BufferTextureCopyRegion copyRegion;
			uint64_t byteSize = 0;
			uint64_t stagingAlignment = 0;
		};

	public:
		VulkanUploadContext(const VulkanDevice* device, const UploadContextDesc& desc);

		Result Init();

		Result BeginBatch() override;
		Result UploadBuffer(const BufferUploadDesc& desc, const void* data, uint64_t size) override;
		Result UploadTexture(const TextureUploadDesc& desc, const void* data, uint64_t size) override;

		Result Submit(UploadSubmission& outSubmission, std::span<const SubmissionToken> waitFor = {}) override;
		Result CancelBatch() override;

		Result ProcessCompleted() override;

	private:
		bool IsRecording() const;

		Result ValidateBufferUpload(const BufferUploadDesc& desc, const void* data, uint64_t size) const;
		Result PrepareTextureUpload(const TextureUploadDesc& desc,
			const void* data,
			uint64_t size,
			TextureUploadInfo& out) const;

		Result StageData(const void* data, uint64_t size, uint64_t alignment, StagingAllocation& out);
		Result AllocateStaging(uint64_t size, uint64_t alignment, StagingAllocation& out);

		void ReleaseStagingPages(BatchResources& batch);
		void ClearActiveBatch();

		bool RequiresQueueFamilyTransfer(QueueType destination) const;

	private:
		const VulkanDevice* m_Device;

		UploadContextDesc m_Desc;
		CommandQueue* m_Queue = nullptr;

		std::vector<StagingPage> m_StagingPages;
		std::vector<BatchResources> m_Batches;

		size_t m_ActiveBatchIndex = SIZE_MAX;
		bool m_FinalBarriersRecorded = false;
		uint64_t m_BatchBytes = 0;

		BarrierDesc m_FinalBarriers;
		BarrierDesc m_AcquireBarriers;
	};
}
