#pragma once
#include "CommandList.h"
#include "VulkanBuffer.h"
#include "VulkanCommandResourceTracker.h"
#include "VulkanTexture.h"

#include <volk.h>

namespace RealRHI {
	enum class VulkanTransferDirection {
		Upload,
		Readback
	};

	struct VulkanBufferTextureCopy {
		VkBufferImageCopy region{};
		uint64_t byteSize = 0;
	};

	// Validates complete descriptions before the command list retains resources,
	// updates its operation log, or records a native copy. Output is assigned on success.
	class VulkanTransferValidation {
	public:
		VulkanTransferValidation(const VulkanDevice* device, const VulkanCommandResourceTracker& resources);

		bool PrepareBufferCopy(const VulkanBuffer* source,
			const VulkanBuffer* destination,
			const BufferCopyRegion& region,
			VkBufferCopy& outCopy) const;
		bool PrepareBufferTextureCopy(VulkanTransferDirection direction,
			const VulkanBuffer* buffer,
			const VulkanTexture* texture,
			const BufferTextureCopyRegion& region,
			VulkanBufferTextureCopy& outCopy) const;
		bool PrepareTextureCopy(const VulkanTexture* source,
			const VulkanTexture* destination,
			const TextureCopyRegion& region,
			VkImageCopy& outCopy) const;
		bool PrepareMipGeneration(const VulkanTexture* texture,
			const MipGenerationDesc& desc,
			QueueId queue,
			TextureSubresourceRange& outRange) const;

		static bool TryGetPackedSize(VkExtent3D extent, uint32_t layers, uint64_t texelSize, uint64_t& outSize);
		static bool SupportsFinalLayout(TextureUsage usage, TextureLayout layout);

	private:
		bool Reject(const char* operation, const char* cause) const;
		static bool IsBufferRangeValid(const VulkanBuffer* buffer, uint64_t offset, uint64_t size);
		static bool IsSubresourceRangeValid(const VulkanTexture* texture, TextureSubresourceRange range);
		static bool FitsMipExtent(const VulkanTexture* texture, uint32_t mip, VkExtent3D extent);

	private:
		const VulkanDevice* m_Device;
		const VulkanCommandResourceTracker& m_Resources;
	};
}
