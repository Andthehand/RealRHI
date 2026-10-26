#pragma once
#include "VulkanResourceOperation.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace RealRHI {
	class VulkanResourceStateTransaction;

	// Recording-local state only. Resources remain owned by the command list.
	class VulkanCommandResourceTracker {
	public:
		void RecordBufferUse(VulkanBuffer* buffer, uint64_t offset, uint64_t size, const char* operation);
		void RecordTextureUse(VulkanTexture* texture,
			TextureSubresourceRange subresources,
			TextureLayout layout,
			const char* operation);
		// The complete barrier batch must validate before updating the log or layouts.
		void RecordBarriers(std::span<const VulkanResourceOperation> operations);
		TextureLayout GetExpectedLayout(const VulkanTexture* texture, uint32_t mip, uint32_t layer) const;
		bool HasRecordedLayout(const VulkanTexture* texture,
			uint32_t mipLevel,
			uint32_t baseLayer,
			uint32_t layerCount,
			TextureLayout expected) const;
		bool Replay(VulkanResourceStateTransaction& transaction) const;
		void Reset();

	private:
		std::vector<VulkanResourceOperation> m_Operations;
		std::unordered_map<VulkanSubresourceKey, TextureLayout, VulkanSubresourceKeyHash> m_RecordedLayouts;
	};
}
