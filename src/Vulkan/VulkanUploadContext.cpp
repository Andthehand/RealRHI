#include "VulkanUploadContext.h"

#include "VulkanConversions.h"
#include "VulkanCpuTimer.h"
#include "VulkanTexture.h"

#include <algorithm>
#include <numeric>
#include <utility>

namespace RealRHI {
	VulkanUploadContext::VulkanUploadContext(const VulkanDevice* device, const UploadContextDesc& desc)
		: m_Device(device), m_Desc(desc) { }

	Result VulkanUploadContext::Init() {
		m_Queue = m_Device->GetCommandQueue(m_Desc.queueType);
		if (!m_Queue || m_Desc.stagingPageSize == 0 || m_Desc.stagingPageSize > SIZE_MAX) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateUploadContext: Provide an available queue and a nonzero staging page size within host address "
				"limits.");

			return Result::InvalidParameter;
		}

		return Result::Success;
	}

	Result VulkanUploadContext::BeginBatch() {
		if (m_ActiveBatchIndex != SIZE_MAX) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"BeginBatch: Submit or cancel the current upload batch first.");

			return Result::InvalidParameter;
		}

		const Result retirement = ProcessCompleted();
		if (retirement != Result::Success) {
			return retirement;
		}

		size_t index = 0;
		while (index < m_Batches.size() && m_Batches[index].completion.IsValid()) {
			++index;
		}

		if (index == m_Batches.size()) {
			BatchResources batch;
			Result result = m_Device->CreateCommandAllocator(m_Desc.queueType, batch.allocator);
			if (result != Result::Success) {
				return result;
			}

			result = m_Device->CreateCommandList(batch.allocator.Raw(), batch.commandList);
			if (result != Result::Success) {
				return result;
			}

			m_Batches.push_back(std::move(batch));
		}

		const Result result = m_Batches[index].commandList->Begin();
		if (result == Result::Success) {
			m_ActiveBatchIndex = index;
			m_FinalBarriersRecorded = false;
			m_Batches[index].commandList->WriteTimestamp("Upload");
		}

		return result;
	}

	Result VulkanUploadContext::UploadBuffer(const BufferUploadDesc& desc, const void* data, uint64_t size) {
		const VulkanCpuTimer timer(m_Device);
		Result result = ValidateBufferUpload(desc, data, size);
		if (result != Result::Success) {
			return result;
		}

		auto* destination = static_cast<VulkanBuffer*>(desc.destination);
		StagingAllocation allocation;
		result = StageData(data, size, 4, allocation);
		if (result != Result::Success) {
			return result;
		}

		BarrierDesc before;
		before.buffers.push_back({
			.buffer = destination,
			.before = desc.before,
			.after = {PipelineStage::Transfer, ResourceAccess::TransferWrite},
			.offset = desc.destinationOffset,
			.size = size,
			.sourceQueue = desc.sourceQueue,
		});
		if (desc.sourceQueue) {
			before.buffers.back().destinationQueue = m_Desc.queueType;
		}

		auto* commandList = m_Batches[m_ActiveBatchIndex].commandList.Raw();
		commandList->BeginDebugLabel("Upload buffer");
		commandList->Barrier(before);

		commandList->CopyBuffer(allocation.buffer,
			destination,
			{.sourceOffset = allocation.offset, .destinationOffset = desc.destinationOffset, .size = size});
		commandList->EndDebugLabel();

		const BufferBarrier final{
			.buffer = destination,
			.before = {PipelineStage::Transfer, ResourceAccess::TransferWrite},
			.after = desc.finalState,
			.offset = desc.destinationOffset,
			.size = size,
			.sourceQueue = m_Desc.queueType,
			.destinationQueue = desc.finalQueue,
		};

		m_FinalBarriers.buffers.push_back(final);

		if (RequiresQueueFamilyTransfer(desc.finalQueue)) {
			m_AcquireBarriers.buffers.push_back(final);
		}
		m_BatchBytes += size;
		timer.Finish(VulkanProfiledOperation::UploadStaging, size);

		return Result::Success;
	}

	Result VulkanUploadContext::UploadTexture(const TextureUploadDesc& desc, const void* data, uint64_t size) {
		const VulkanCpuTimer timer(m_Device);
		TextureUploadInfo upload;
		Result result = PrepareTextureUpload(desc, data, size, upload);
		if (result != Result::Success) {
			return result;
		}

		auto* destination = static_cast<VulkanTexture*>(desc.destination);
		StagingAllocation allocation;
		result = StageData(data, upload.byteSize, upload.stagingAlignment, allocation);
		if (result != Result::Success) {
			return result;
		}

		BarrierDesc before;
		before.textures.push_back({
			.texture = destination,
			.before = desc.before,
			.after = {PipelineStage::Transfer, ResourceAccess::TransferWrite},
			.oldLayout = desc.oldLayout,
			.newLayout = TextureLayout::TransferDst,
			.subresources = {desc.mipLevel, 1, desc.baseArrayLayer, desc.layerCount},
			.sourceQueue = desc.sourceQueue,
		});
		if (desc.sourceQueue) {
			before.textures.back().destinationQueue = m_Desc.queueType;
		}

		auto* commandList = m_Batches[m_ActiveBatchIndex].commandList.Raw();
		commandList->BeginDebugLabel("Upload texture");
		commandList->Barrier(before);

		upload.copyRegion.bufferOffset = allocation.offset;
		commandList->CopyBufferToTexture(allocation.buffer, destination, upload.copyRegion);
		commandList->EndDebugLabel();

		const TextureBarrier final{
			.texture = destination,
			.before = {PipelineStage::Transfer, ResourceAccess::TransferWrite},
			.after = desc.finalState,
			.oldLayout = TextureLayout::TransferDst,
			.newLayout = desc.finalLayout,
			.subresources = {desc.mipLevel, 1, desc.baseArrayLayer, desc.layerCount},
			.sourceQueue = m_Desc.queueType,
			.destinationQueue = desc.finalQueue,
		};

		m_FinalBarriers.textures.push_back(final);

		if (RequiresQueueFamilyTransfer(desc.finalQueue)) {
			m_AcquireBarriers.textures.push_back(final);
		}
		m_BatchBytes += upload.byteSize;
		timer.Finish(VulkanProfiledOperation::UploadStaging, upload.byteSize);

		return Result::Success;
	}

	Result VulkanUploadContext::Submit(UploadSubmission& outSubmission, std::span<const SubmissionToken> waitFor) {
		outSubmission = {};

		if (m_ActiveBatchIndex == SIZE_MAX || (m_FinalBarriers.buffers.empty() && m_FinalBarriers.textures.empty())) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Upload Submit: Begin a batch and add at least one upload before submitting.");

			return Result::InvalidParameter;
		}

		auto& batch = m_Batches[m_ActiveBatchIndex];

		// End or submission can fail. A retry must not record these barriers twice.
		if (!m_FinalBarriersRecorded) {
			batch.commandList->Barrier(m_FinalBarriers);
			batch.commandList->WriteTimestamp("Upload end");
			m_FinalBarriersRecorded = true;
		}

		if (batch.commandList->GetState() == CommandListState::Recording) {
			const Result result = batch.commandList->End();
			if (result != Result::Success) {
				return result;
			}
		}

		CommandList* lists[] = {batch.commandList.Raw()};
		const SubmissionToken token = m_Queue->Submit({.commandLists = lists, .waitFor = waitFor});
		if (!token.IsValid()) {
			return Result::Failed;
		}

		batch.completion = token;
		outSubmission.completion = token;
		outSubmission.acquireBarriers = std::move(m_AcquireBarriers);
		m_Device->RecordUploadBatch(m_BatchBytes);

		ClearActiveBatch();
		return Result::Success;
	}

	Result VulkanUploadContext::CancelBatch() {
		if (m_ActiveBatchIndex == SIZE_MAX) {
			return Result::Success;
		}

		auto& batch = m_Batches[m_ActiveBatchIndex];

		if (batch.commandList->GetState() == CommandListState::Recording) {
			const Result result = batch.commandList->End();
			if (result != Result::Success) {
				return result;
			}
		}

		const Result result = batch.commandList->Reset();
		if (result != Result::Success) {
			return result;
		}

		ReleaseStagingPages(batch);
		ClearActiveBatch();
		return Result::Success;
	}

	Result VulkanUploadContext::ProcessCompleted() {
		for (auto& batch : m_Batches) {
			if (!batch.completion.IsValid() || !m_Queue->IsComplete(batch.completion)) {
				continue;
			}

			// Reset must succeed before staging memory can be reused.
			const Result result = batch.commandList->Reset();
			if (result != Result::Success) {
				return result;
			}

			ReleaseStagingPages(batch);
			batch.completion = {};
		}

		return Result::Success;
	}

	bool VulkanUploadContext::IsRecording() const {
		return m_ActiveBatchIndex != SIZE_MAX && !m_FinalBarriersRecorded;
	}

	Result VulkanUploadContext::AllocateStaging(uint64_t size, uint64_t alignment, StagingAllocation& out) {
		auto& batch = m_Batches[m_ActiveBatchIndex];

		for (size_t index : batch.stagingPageIndices) {
			auto& page = m_StagingPages[index];
			const uint64_t padding = (alignment - page.bytesUsed % alignment) % alignment;
			const uint64_t remaining = page.buffer->GetSize() - page.bytesUsed;

			if (padding <= remaining && size <= remaining - padding) {
				out = {static_cast<VulkanBuffer*>(page.buffer.Raw()), page.bytesUsed + padding};
				page.bytesUsed += padding + size;

				return Result::Success;
			}
		}

		size_t index = 0;
		while (index < m_StagingPages.size() &&
			(m_StagingPages[index].inUse || m_StagingPages[index].buffer->GetSize() < size)) {
			++index;
		}

		if (index == m_StagingPages.size()) {
			StagingPage page;
			const BufferDesc desc{
				.size = std::max(m_Desc.stagingPageSize, size),
				.usage = BufferUsage::TransferSrc,
				.memoryUsage = MemoryUsage::CPUToGPU,
			};

			const Result result = m_Device->CreateBuffer(desc, page.buffer);
			if (result != Result::Success) {
				return result;
			}

			m_StagingPages.push_back(std::move(page));
		}

		auto& page = m_StagingPages[index];
		page.inUse = true;
		page.bytesUsed = size;
		batch.stagingPageIndices.push_back(index);

		out = {static_cast<VulkanBuffer*>(page.buffer.Raw()), 0};

		return Result::Success;
	}

	bool VulkanUploadContext::RequiresQueueFamilyTransfer(QueueType destination) const {
		return m_Device->GetCommandQueue(destination)->GetQueueId().familyIndex != m_Queue->GetQueueId().familyIndex;
	}

	void VulkanUploadContext::ReleaseStagingPages(BatchResources& batch) {
		for (size_t index : batch.stagingPageIndices) {
			m_StagingPages[index].bytesUsed = 0;
			m_StagingPages[index].inUse = false;
		}

		batch.stagingPageIndices.clear();
	}

	Result VulkanUploadContext::ValidateBufferUpload(const BufferUploadDesc& desc,
		const void* data,
		uint64_t size) const {
		auto* destination = static_cast<VulkanBuffer*>(desc.destination);

		if (!IsRecording()) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadBuffer: Requires a recording batch. Begin a batch before adding uploads.");

			return Result::InvalidParameter;
		}

		if (!destination || !data) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadBuffer: Provide a non-null destination and non-null data.");

			return Result::InvalidParameter;
		}

		if (!Any(destination->GetUsage() & BufferUsage::TransferDst)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadBuffer: Destination must have TransferDst usage.");

			return Result::InvalidParameter;
		}

		if (size == 0 || size > SIZE_MAX || desc.destinationOffset > destination->GetSize() ||
			size > destination->GetSize() - desc.destinationOffset) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadBuffer: Provide an in-bounds nonzero range within host address limits.");

			return Result::InvalidParameter;
		}

		if (!m_Device->GetCommandQueue(desc.finalQueue) ||
			(desc.sourceQueue && !m_Device->GetCommandQueue(*desc.sourceQueue))) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "UploadBuffer: Provide available queue endpoints.");

			return Result::InvalidParameter;
		}

		for (const auto& previous : m_FinalBarriers.buffers) {
			if (previous.buffer == destination && previous.offset < desc.destinationOffset + size &&
				desc.destinationOffset < previous.offset + previous.size) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"UploadBuffer: Destination ranges in one batch must not overlap.");

				return Result::InvalidParameter;
			}
		}

		return Result::Success;
	}

	Result VulkanUploadContext::PrepareTextureUpload(const TextureUploadDesc& desc,
		const void* data,
		uint64_t size,
		TextureUploadInfo& out) const {
		auto* destination = static_cast<VulkanTexture*>(desc.destination);

		if (!IsRecording()) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadTexture: Requires a recording batch. Begin a batch before adding uploads.");

			return Result::InvalidParameter;
		}

		if (!destination || !data) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadTexture: Provide a non-null destination and non-null data.");

			return Result::InvalidParameter;
		}

		if (size == 0 || size > SIZE_MAX) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadTexture: Provide a nonzero data size within host address limits.");

			return Result::InvalidParameter;
		}

		if (destination->IsExternal() || destination->GetSampleCount() != 1 ||
			!Any(destination->GetUsage() & TextureUsage::TransferDst)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadTexture: Destination must be a single-sampled, non-swapchain TransferDst texture.");

			return Result::InvalidParameter;
		}

		if (desc.mipLevel >= destination->GetMipLevelCount() || desc.layerCount == 0 ||
			desc.baseArrayLayer >= destination->GetArrayLayerCount() ||
			desc.layerCount > destination->GetArrayLayerCount() - desc.baseArrayLayer) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadTexture: Select valid mip/layers within the destination texture.");

			return Result::InvalidParameter;
		}

		if (!m_Device->GetCommandQueue(desc.finalQueue) ||
			(desc.sourceQueue && !m_Device->GetCommandQueue(*desc.sourceQueue))) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "UploadTexture: Provide available queue endpoints.");

			return Result::InvalidParameter;
		}

		const auto format = Utils::VkFormatToTextureFormat(destination->GetFormat());
		const uint64_t texelSize = Utils::TextureFormatByteSize(format);

		bool validLayout = false;
		switch (desc.finalLayout) {
			case TextureLayout::General:
			case TextureLayout::TransferDst:
				validLayout = true;
				break;
			case TextureLayout::ShaderRead:
				validLayout = Any(destination->GetUsage() & TextureUsage::ShaderResource);
				break;
			case TextureLayout::TransferSrc:
				validLayout = Any(destination->GetUsage() & TextureUsage::TransferSrc);
				break;
			case TextureLayout::ColorAttachment:
				validLayout = Any(destination->GetUsage() & TextureUsage::RenderTarget);
				break;
			default:
				break;
		}

		if (!validLayout || texelSize == 0 ||
			Utils::TextureFormatToVkImageAspect(format) != VK_IMAGE_ASPECT_COLOR_BIT) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadTexture: Only tightly packed color formats and a final layout compatible with texture usage are "
				"supported.");

			return Result::InvalidParameter;
		}

		const auto extent = destination->GetMipExtent(desc.mipLevel);
		const uint32_t width = extent.width;
		const uint32_t height = extent.height;
		const uint32_t depth = extent.depth;

		uint64_t required = texelSize;
		for (uint32_t factor : {width, height, depth, desc.layerCount}) {
			if (required > UINT64_MAX / factor) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"UploadTexture: Packed subresource byte count overflows uint64_t.");

				return Result::InvalidParameter;
			}
			required *= factor;
		}

		if (size < required) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadTexture: Input does not contain the complete tightly packed mip/layer data.");

			return Result::InvalidParameter;
		}

		for (const auto& previous : m_FinalBarriers.textures) {
			if (previous.texture == destination && previous.subresources.baseMipLevel == desc.mipLevel &&
				previous.subresources.baseArrayLayer < desc.baseArrayLayer + desc.layerCount &&
				desc.baseArrayLayer < previous.subresources.baseArrayLayer + previous.subresources.arrayLayerCount) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"UploadTexture: Destination mip/layer ranges in one batch must not overlap.");

				return Result::InvalidParameter;
			}
		}

		out.copyRegion = {
			.width = width,
			.height = height,
			.depth = depth,
			.mipLevel = desc.mipLevel,
			.baseArrayLayer = desc.baseArrayLayer,
			.layerCount = desc.layerCount,
		};
		out.byteSize = required;

		// Transfer queues require four-byte alignment; the format also requires whole texels.
		out.stagingAlignment = std::lcm(uint64_t{4}, texelSize);
		return Result::Success;
	}

	Result VulkanUploadContext::StageData(const void* data, uint64_t size, uint64_t alignment, StagingAllocation& out) {
		const Result result = AllocateStaging(size, alignment, out);
		if (result != Result::Success) {
			return result;
		}

		return out.buffer->CopyToMappedMemory(data, size, out.offset);
	}

	void VulkanUploadContext::ClearActiveBatch() {
		m_BatchBytes = 0;
		m_FinalBarriers = {};
		m_AcquireBarriers = {};
		m_ActiveBatchIndex = SIZE_MAX;
		m_FinalBarriersRecorded = false;
	}

}
