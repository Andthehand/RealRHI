#pragma once
#include "VulkanResourceState.h"

#include <cstddef>
#include <functional>

namespace RealRHI {
	class VulkanBuffer;
	class VulkanTexture;

	struct VulkanSubresourceKey {
		const VulkanTexture* texture = nullptr;
		uint32_t mipLevel = 0;
		uint32_t arrayLayer = 0;

	public:
		bool operator==(const VulkanSubresourceKey&) const = default;
	};

	struct VulkanSubresourceKeyHash {
		size_t operator()(const VulkanSubresourceKey& key) const {
			return std::hash<const VulkanTexture*>{}(key.texture) ^ (static_cast<size_t>(key.mipLevel) << 16) ^
				static_cast<size_t>(key.arrayLayer);
		}
	};

	// Non-owning log entry; the command list retains each referenced resource.
	struct VulkanResourceOperation {
		const char* name = nullptr;
		VulkanBuffer* buffer = nullptr;
		VulkanTexture* texture = nullptr;
		uint64_t offset = 0;
		uint64_t size = 0;
		TextureSubresourceRange subresources{};
		TextureLayout oldLayout = TextureLayout::Undefined;
		TextureLayout newLayout = TextureLayout::Undefined;
		uint32_t sourceFamily = UINT32_MAX;
		uint32_t destinationFamily = UINT32_MAX;
		bool barrier = false;
	};
}
