#pragma once
#include "BarrierDesc.h"
#include "Queue.h"

#include <cstdint>

namespace RealRHI {
	struct VulkanQueueOwnership {
		uint32_t family = UINT32_MAX;
		uint32_t pendingFamily = UINT32_MAX;
		SubmissionToken release{};

	public:
		bool operator==(const VulkanQueueOwnership&) const = default;
	};

	struct VulkanTextureState {
		VulkanQueueOwnership ownership{};
		TextureLayout layout = TextureLayout::Undefined;
		TextureLayout releaseOldLayout = TextureLayout::Undefined;
		TextureLayout releaseNewLayout = TextureLayout::Undefined;
		TextureSubresourceRange releaseRange{};
	};

	struct VulkanBufferRangeState {
		uint64_t offset = 0;
		uint64_t size = 0;
		VulkanQueueOwnership ownership{};
		uint64_t releaseOffset = 0;
		uint64_t releaseSize = 0;

	public:
		bool CanMergeWith(const VulkanBufferRangeState& next) const {
			return offset + size == next.offset && ownership == next.ownership &&
				(ownership.pendingFamily == UINT32_MAX ||
					(releaseOffset == next.releaseOffset && releaseSize == next.releaseSize));
		}
	};
}
