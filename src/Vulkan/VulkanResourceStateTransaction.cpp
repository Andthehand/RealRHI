#include "VulkanResourceStateTransaction.h"

#include "VulkanBuffer.h"
#include "VulkanCommandQueue.h"
#include "VulkanTexture.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <utility>

namespace RealRHI {
	VulkanResourceStateTransaction::VulkanResourceStateTransaction(const VulkanDevice* device,
		SubmissionToken submission,
		std::span<const VkSemaphoreSubmitInfo> waits)
		: m_Device(device), m_Submission(submission), m_Waits(waits) { }

	bool VulkanResourceStateTransaction::ValidateOwnership(VulkanQueueOwnership& ownership,
		const VulkanResourceOperation& operation) const {
		auto reject = [&](const char* cause) {
			std::ostringstream message;
			message << "Submit: " << operation.name << ": " << cause << "; family=" << m_Submission.queue.familyIndex
					<< ", owner=" << ownership.family << ", pending destination=" << ownership.pendingFamily;
			if (operation.buffer) {
				message << ", buffer=" << operation.buffer << ", offset=" << operation.offset
						<< ", size=" << operation.size;
			}
			else {
				message << ", texture=" << operation.texture << ", base mip=" << operation.subresources.baseMipLevel
						<< ", base layer=" << operation.subresources.baseArrayLayer;
			}
			if (ownership.release.IsValid()) {
				message << ", release token=" << ownership.release.queue.familyIndex << ":"
						<< ownership.release.queue.queueIndex << "/" << ownership.release.value;
			}
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, message.str().c_str());
			return false;
		};

		const bool transfer =
			operation.sourceFamily != UINT32_MAX && operation.sourceFamily != operation.destinationFamily;
		const bool acquire = transfer && m_Submission.queue.familyIndex == operation.destinationFamily;
		if (acquire) {
			if (ownership.family != operation.sourceFamily || ownership.pendingFamily != operation.destinationFamily ||
				!ownership.release.IsValid()) {
				return reject("acquire requires a matching release from the source family");
			}
			auto* sourceQueue = m_Device->GetCommandQueue(ownership.release.queue);
			bool waited = false;
			for (const auto& wait : m_Waits) {
				if (sourceQueue && wait.semaphore == sourceQueue->GetTimelineSemaphore() &&
					wait.value >= ownership.release.value &&
					(wait.stageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) != 0) {
					waited = true;
					break;
				}
			}
			if (!waited) {
				return reject("acquire must waitFor the release submission token (or a later token on that queue)");
			}
			ownership.family = operation.destinationFamily;
			ownership.pendingFamily = UINT32_MAX;
			ownership.release = {};
			return true;
		}

		if (ownership.pendingFamily != UINT32_MAX) {
			return reject("resource was released; record a matching acquire before using or releasing it again");
		}
		if (ownership.family != UINT32_MAX && ownership.family != m_Submission.queue.familyIndex) {
			return reject("resource belongs to another family; release there and acquire on this family");
		}
		ownership.family = m_Submission.queue.familyIndex;
		if (transfer) {
			ownership.pendingFamily = operation.destinationFamily;
			ownership.release = m_Submission;
		}
		return true;
	}

	size_t VulkanResourceStateTransaction::SplitBufferRange(std::vector<VulkanBufferRangeState>& ranges,
		uint64_t boundary) {
		const auto next = std::upper_bound(ranges.begin(), ranges.end(), boundary,
			[](uint64_t offset, const VulkanBufferRangeState& range) { return offset < range.offset; });
		const auto current = std::prev(next);
		if (current->offset == boundary) {
			return static_cast<size_t>(current - ranges.begin());
		}
		const size_t index = static_cast<size_t>(next - ranges.begin());
		if (boundary < current->offset + current->size) {
			auto tail = *current;
			tail.size = current->offset + current->size - boundary;
			tail.offset = boundary;
			current->size = boundary - current->offset;
			ranges.insert(next, tail);
		}
		return index;
	}

	void VulkanResourceStateTransaction::MergeBufferRanges(std::vector<VulkanBufferRangeState>& ranges,
		size_t first,
		size_t last) {
		// Only the changed ranges and their immediate neighbors can become mergeable.
		if (first > 0) {
			--first;
		}
		if (last < ranges.size()) {
			++last;
		}
		size_t count = first + 1;
		for (size_t i = first + 1; i < last; ++i) {
			if (ranges[count - 1].CanMergeWith(ranges[i])) {
				ranges[count - 1].size += ranges[i].size;
			}
			else {
				ranges[count++] = ranges[i];
			}
		}
		ranges.erase(ranges.begin() + count, ranges.begin() + last);
	}

	bool VulkanResourceStateTransaction::Validate(std::span<const VulkanResourceOperation> operations) {
		for (const auto& operation : operations) {
			// Swapchain layouts/ownership follow acquire/present synchronization, outside
			// the ordinary queue timeline ownership tracker (as with attachment uses).
			if (operation.texture && operation.texture->IsExternal()) {
				continue;
			}
			const bool transfer =
				operation.sourceFamily != UINT32_MAX && operation.sourceFamily != operation.destinationFamily;
			const bool acquire = transfer && m_Submission.queue.familyIndex == operation.destinationFamily;
			if (operation.buffer) {
				auto [entry, inserted] = m_Buffers.try_emplace(operation.buffer);
				if (inserted) {
					entry->second = operation.buffer->GetSubmittedRanges();
				}
				auto& ranges = entry->second;
				const uint64_t end = operation.offset + operation.size;
				const size_t first = SplitBufferRange(ranges, operation.offset);
				const size_t last = SplitBufferRange(ranges, end);
				for (size_t i = first; i < last; ++i) {
					auto& range = ranges[i];
					if (!ValidateOwnership(range.ownership, operation)) {
						return false;
					}
					if (acquire && (range.releaseOffset != operation.offset || range.releaseSize != operation.size)) {
						m_Device->SendDebugMessage(DebugSeverity::Error,
							DebugMessageType::General,
							"Submit: Buffer acquire offset and size must exactly match the release barrier.");
						return false;
					}
					if (transfer && !acquire) {
						range.releaseOffset = operation.offset;
						range.releaseSize = operation.size;
					}
				}
				MergeBufferRanges(ranges, first, last);
				continue;
			}

			const auto& subresources = operation.subresources;
			for (uint32_t mip = subresources.baseMipLevel; mip < subresources.baseMipLevel + subresources.mipLevelCount;
				++mip) {
				for (uint32_t layer = subresources.baseArrayLayer;
					layer < subresources.baseArrayLayer + subresources.arrayLayerCount;
					++layer) {
					const VulkanSubresourceKey key{operation.texture, mip, layer};
					auto [entry, inserted] = m_Textures.try_emplace(key);
					if (inserted) {
						entry->second = operation.texture->GetSubmittedState(mip, layer);
					}
					auto& state = entry->second;
					if (!ValidateOwnership(state.ownership, operation)) {
						return false;
					}
					if (acquire) {
						const auto& released = state.releaseRange;
						if (state.releaseOldLayout != operation.oldLayout ||
							state.releaseNewLayout != operation.newLayout ||
							released.baseMipLevel != subresources.baseMipLevel ||
							released.mipLevelCount != subresources.mipLevelCount ||
							released.baseArrayLayer != subresources.baseArrayLayer ||
							released.arrayLayerCount != subresources.arrayLayerCount) {
							m_Device->SendDebugMessage(DebugSeverity::Error,
								DebugMessageType::General,
								"Submit: Texture acquire old/new layouts and mip/layer range must exactly match the "
								"release barrier.");
							return false;
						}
					}
					else if (operation.oldLayout != TextureLayout::Undefined && state.layout != operation.oldLayout) {
						std::ostringstream message;
						message << "Submit: " << operation.name << ": texture=" << operation.texture << ", mip=" << mip
								<< ", layer=" << layer << ": layout differs from the recorded requirement; expected="
								<< static_cast<uint32_t>(operation.oldLayout)
								<< ", submitted=" << static_cast<uint32_t>(state.layout);
						m_Device->SendDebugMessage(
							DebugSeverity::Error, DebugMessageType::General, message.str().c_str());
						return false;
					}
					if (transfer && !acquire) {
						state.releaseOldLayout = operation.oldLayout;
						state.releaseNewLayout = operation.newLayout;
						state.releaseRange = subresources;
					}
					if (operation.barrier) {
						state.layout = operation.newLayout;
					}
				}
			}
		}
		return true;
	}

	void VulkanResourceStateTransaction::Commit() {
		for (auto& [buffer, ranges] : m_Buffers) {
			buffer->CommitSubmittedRanges(std::move(ranges));
		}
		for (const auto& [key, state] : m_Textures) {
			const_cast<VulkanTexture*>(key.texture)->CommitSubmittedState(key.mipLevel, key.arrayLayer, state);
		}
	}
}
