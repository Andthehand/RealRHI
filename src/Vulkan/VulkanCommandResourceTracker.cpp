#include "VulkanCommandResourceTracker.h"

#include "VulkanResourceStateTransaction.h"
#include "VulkanTexture.h"

namespace RealRHI {

	void VulkanCommandResourceTracker::RecordBufferUse(VulkanBuffer* buffer,
		uint64_t offset,
		uint64_t size,
		const char* operation) {
		m_Operations.push_back({.name = operation, .buffer = buffer, .offset = offset, .size = size});
	}

	void VulkanCommandResourceTracker::RecordTextureUse(VulkanTexture* texture,
		TextureSubresourceRange subresources,
		TextureLayout layout,
		const char* operation) {
		// Swapchain ownership/layouts are managed by frame submission rather than the
		// ordinary resource tracker. Graph barriers still record native transitions.
		if (!texture->IsExternal()) {
			m_Operations.push_back(
				{.name = operation, .texture = texture, .subresources = subresources, .oldLayout = layout});
		}
	}

	TextureLayout VulkanCommandResourceTracker::GetExpectedLayout(const VulkanTexture* texture,
		uint32_t mip,
		uint32_t layer) const {
		const auto it = m_RecordedLayouts.find({texture, mip, layer});
		if (it != m_RecordedLayouts.end()) {
			return it->second;
		}

		return texture->GetSubmittedState(mip, layer).layout;
	}

	bool VulkanCommandResourceTracker::HasRecordedLayout(const VulkanTexture* texture,
		uint32_t mipLevel,
		uint32_t baseLayer,
		uint32_t layerCount,
		TextureLayout expected) const {
		for (uint32_t layer = baseLayer; layer < baseLayer + layerCount; ++layer) {
			const auto recorded = m_RecordedLayouts.find({texture, mipLevel, layer});
			if (expected != TextureLayout::Undefined && recorded != m_RecordedLayouts.end() &&
				recorded->second != expected) {
				return false;
			}
		}
		return true;
	}

	void VulkanCommandResourceTracker::RecordBarriers(std::span<const VulkanResourceOperation> operations) {
		for (const auto& operation : operations) {
			if (!operation.texture) {
				continue;
			}
			const auto& range = operation.subresources;
			for (uint32_t mip = range.baseMipLevel; mip < range.baseMipLevel + range.mipLevelCount; ++mip) {
				for (uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.arrayLayerCount;
					++layer) {
					m_RecordedLayouts[{operation.texture, mip, layer}] = operation.newLayout;
				}
			}
		}
		m_Operations.insert(m_Operations.end(), operations.begin(), operations.end());
	}

	bool VulkanCommandResourceTracker::Replay(VulkanResourceStateTransaction& transaction) const {
		return transaction.Validate(m_Operations);
	}

	void VulkanCommandResourceTracker::Reset() {
		m_Operations.clear();
		m_RecordedLayouts.clear();
	}
}
