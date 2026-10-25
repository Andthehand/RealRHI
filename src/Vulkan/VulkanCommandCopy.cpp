#include "VulkanCommandList.h"

#include "VulkanConvertions.h"
#include "VulkanTexture.h"

#include <algorithm>

namespace RealRHI {
	void VulkanCommandList::CopyBuffer(Buffer* source, Buffer* destination, const BufferCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.size == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBuffer failed: Command list is not recording or region size is zero.");

			return;
		}

		auto* sourceBuffer = static_cast<VulkanBuffer*>(source);
		auto* destinationBuffer = static_cast<VulkanBuffer*>(destination);
		if (sourceBuffer == nullptr || destinationBuffer == nullptr ||
			!Any(sourceBuffer->GetUsage() & BufferUsage::TransferSrc) ||
			!Any(destinationBuffer->GetUsage() & BufferUsage::TransferDst)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBuffer: Provide non-null source and destination buffers with TransferSrc "
				"and TransferDst usage respectively.");

			return;
		}

		if (region.sourceOffset > sourceBuffer->GetSize() ||
			region.size > sourceBuffer->GetSize() - region.sourceOffset ||
			region.destinationOffset > destinationBuffer->GetSize() ||
			region.size > destinationBuffer->GetSize() - region.destinationOffset) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBuffer failed: Copy region is out of bounds of source or destination buffer.");

			return;
		}

		RetainResource(sourceBuffer);
		RetainResource(destinationBuffer);
		RecordBufferUse(sourceBuffer, region.sourceOffset, region.size, "CopyBuffer source");
		RecordBufferUse(destinationBuffer, region.destinationOffset, region.size, "CopyBuffer destination");

		VkBufferCopy copyRegion{
			.srcOffset = region.sourceOffset,
			.dstOffset = region.destinationOffset,
			.size = region.size,
		};
		vkCmdCopyBuffer(m_CommandBuffer, sourceBuffer->GetBuffer(), destinationBuffer->GetBuffer(), 1, &copyRegion);
	}

	void VulkanCommandList::CopyBufferToTexture(Buffer* source,
		Texture* destination,
		const BufferTextureCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.width == 0 || region.height == 0 || region.depth == 0 ||
			region.layerCount == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBufferToTexture failed: Command list is not recording or region dimensions are zero.");

			return;
		}

		auto* sourceBuffer = static_cast<VulkanBuffer*>(source);
		auto* destinationTexture = static_cast<VulkanTexture*>(destination);
		if (sourceBuffer == nullptr || destinationTexture == nullptr ||
			region.mipLevel >= destinationTexture->GetMipLevelCount() ||
			region.baseArrayLayer >= destinationTexture->GetArrayLayerCount() ||
			region.layerCount > destinationTexture->GetArrayLayerCount() - region.baseArrayLayer ||
			!Any(sourceBuffer->GetUsage() & BufferUsage::TransferSrc) ||
			!Any(destinationTexture->GetUsage() & TextureUsage::TransferDst)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBufferToTexture failed: Invalid source buffer or destination texture, or "
				"invalid copy region parameters.");

			return;
		}

		const VkExtent3D mipExtent{
			.width = std::max(1u, destinationTexture->GetExtent().width >> region.mipLevel),
			.height = std::max(1u, destinationTexture->GetExtent().height >> region.mipLevel),
			.depth = std::max(1u, destinationTexture->GetExtent().depth >> region.mipLevel),
		};
		if (region.width > mipExtent.width || region.height > mipExtent.height || region.depth > mipExtent.depth ||
			region.bufferOffset >= sourceBuffer->GetSize()) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBufferToTexture failed: Copy region dimensions exceed mip level extent or "
				"buffer offset is out of bounds.");

			return;
		}

		const uint32_t texelSize =
			Utils::TextureFormatByteSize(Utils::VkFormatToTextureFormat(destinationTexture->GetFormat()));
		if (texelSize == 0 || region.bufferOffset % texelSize != 0 ||
			region.width > UINT64_MAX / region.height / region.depth / region.layerCount / texelSize ||
			static_cast<uint64_t>(region.width) * region.height * region.depth * region.layerCount * texelSize >
				sourceBuffer->GetSize() - region.bufferOffset) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBufferToTexture: Unsupported format, misaligned buffer offset, or "
				"insufficient source buffer capacity.");
			return;
		}

		RecordTextureUse(destinationTexture,
			{region.mipLevel, 1, region.baseArrayLayer, region.layerCount},
			TextureLayout::TransferDst,
			"CopyBufferToTexture");
		RetainResource(sourceBuffer);
		RetainResource(destinationTexture);
		RecordBufferUse(sourceBuffer,
			region.bufferOffset,
			static_cast<uint64_t>(region.width) * region.height * region.depth * region.layerCount * texelSize,
			"CopyBufferToTexture");
		VkBufferImageCopy copyRegion{
			.bufferOffset = region.bufferOffset,
			.imageSubresource =
				{
					.aspectMask = Utils::TextureFormatToVkImageAspect(
						Utils::VkFormatToTextureFormat(destinationTexture->GetFormat())),
					.mipLevel = region.mipLevel,
					.baseArrayLayer = region.baseArrayLayer,
					.layerCount = region.layerCount,
				},
			.imageExtent = {region.width, region.height, region.depth},
		};
		vkCmdCopyBufferToImage(m_CommandBuffer,
			sourceBuffer->GetBuffer(),
			destinationTexture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1,
			&copyRegion);
	}

	void VulkanCommandList::CopyTextureToBuffer(Texture* source,
		Buffer* destination,
		const BufferTextureCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.width == 0 || region.height == 0 || region.depth == 0 ||
			region.layerCount == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Texture copy failed: Command list is not recording or region dimensions are zero.");
			return;
		}

		auto* texture = static_cast<VulkanTexture*>(source);
		auto* buffer = static_cast<VulkanBuffer*>(destination);
		if (texture == nullptr || buffer == nullptr || !Any(texture->GetUsage() & TextureUsage::TransferSrc) ||
			!Any(buffer->GetUsage() & BufferUsage::TransferDst) ||

			region.mipLevel >= texture->GetMipLevelCount() || region.baseArrayLayer >= texture->GetArrayLayerCount() ||
			region.layerCount > texture->GetArrayLayerCount() - region.baseArrayLayer ||
			region.bufferOffset >= buffer->GetSize() ||
			!HasRecordedLayout(
				texture, region.mipLevel, region.baseArrayLayer, region.layerCount, TextureLayout::TransferSrc)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyTextureToBuffer failed: Invalid source texture, destination buffer, or subresource range.");
			return;
		}

		const VkExtent3D mipExtent{
			.width = std::max(1u, texture->GetExtent().width >> region.mipLevel),
			.height = std::max(1u, texture->GetExtent().height >> region.mipLevel),
			.depth = std::max(1u, texture->GetExtent().depth >> region.mipLevel),
		};
		if (region.width > mipExtent.width || region.height > mipExtent.height || region.depth > mipExtent.depth) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyTextureToBuffer failed: Copy dimensions exceed source mip extent.");
			return;
		}

		// Initial readback supports tightly packed RGBA8 data. Other formats
		// need format-aware block size and row pitch handling.
		if (texture->GetFormat() != VK_FORMAT_R8G8B8A8_UNORM ||
			region.width > UINT64_MAX / region.height / region.depth / region.layerCount / 4 ||
			static_cast<uint64_t>(region.width) * region.height * region.depth * region.layerCount * 4 >
				buffer->GetSize() - region.bufferOffset) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyTextureToBuffer failed: Unsupported texture format or insufficient readback buffer capacity.");
			return;
		}

		RecordTextureUse(texture,
			{region.mipLevel, 1, region.baseArrayLayer, region.layerCount},
			TextureLayout::TransferSrc,
			"CopyTextureToBuffer");
		RetainResource(texture);
		RetainResource(buffer);
		RecordBufferUse(buffer,
			region.bufferOffset,
			static_cast<uint64_t>(region.width) * region.height * region.depth * region.layerCount * 4,
			"CopyTextureToBuffer");
		VkBufferImageCopy copyRegion{
			.bufferOffset = region.bufferOffset,
			.imageSubresource =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.mipLevel = region.mipLevel,
					.baseArrayLayer = region.baseArrayLayer,
					.layerCount = region.layerCount,
				},
			.imageExtent = {region.width, region.height, region.depth},
		};
		vkCmdCopyImageToBuffer(m_CommandBuffer,
			texture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			buffer->GetBuffer(),
			1,
			&copyRegion);
	}

	void VulkanCommandList::CopyTexture(Texture* source, Texture* destination, const TextureCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.width == 0 || region.height == 0 || region.depth == 0 ||
			region.layerCount == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Texture copy failed: Command list is not recording or region dimensions are zero.");
			return;
		}

		auto* sourceTexture = static_cast<VulkanTexture*>(source);
		auto* destinationTexture = static_cast<VulkanTexture*>(destination);
		if (sourceTexture == nullptr || destinationTexture == nullptr ||
			!Any(sourceTexture->GetUsage() & TextureUsage::TransferSrc) ||
			!Any(destinationTexture->GetUsage() & TextureUsage::TransferDst) ||

			sourceTexture == destinationTexture || sourceTexture->GetFormat() != destinationTexture->GetFormat() ||
			sourceTexture->GetFormat() != VK_FORMAT_R8G8B8A8_UNORM ||
			region.sourceMipLevel >= sourceTexture->GetMipLevelCount() ||
			region.destinationMipLevel >= destinationTexture->GetMipLevelCount() ||
			region.sourceBaseArrayLayer >= sourceTexture->GetArrayLayerCount() ||
			region.destinationBaseArrayLayer >= destinationTexture->GetArrayLayerCount() ||
			region.layerCount > sourceTexture->GetArrayLayerCount() - region.sourceBaseArrayLayer ||
			region.layerCount > destinationTexture->GetArrayLayerCount() - region.destinationBaseArrayLayer ||
			!HasRecordedLayout(sourceTexture,
				region.sourceMipLevel,
				region.sourceBaseArrayLayer,
				region.layerCount,
				TextureLayout::TransferSrc) ||
			!HasRecordedLayout(destinationTexture,
				region.destinationMipLevel,
				region.destinationBaseArrayLayer,
				region.layerCount,
				TextureLayout::TransferDst)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyTexture failed: Invalid textures, formats, or subresource range.");
			return;
		}

		const auto fits = [&](const VulkanTexture* texture, uint32_t mipLevel) {
			return region.width <= std::max(1u, texture->GetExtent().width >> mipLevel) &&
				region.height <= std::max(1u, texture->GetExtent().height >> mipLevel) &&
				region.depth <= std::max(1u, texture->GetExtent().depth >> mipLevel);
		};
		if (!fits(sourceTexture, region.sourceMipLevel) || !fits(destinationTexture, region.destinationMipLevel)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyTexture failed: Copy dimensions exceed source or destination mip extent.");
			return;
		}

		RecordTextureUse(sourceTexture,
			{region.sourceMipLevel, 1, region.sourceBaseArrayLayer, region.layerCount},
			TextureLayout::TransferSrc,
			"CopyTexture source");
		RecordTextureUse(destinationTexture,
			{region.destinationMipLevel, 1, region.destinationBaseArrayLayer, region.layerCount},
			TextureLayout::TransferDst,
			"CopyTexture destination");
		RetainResource(sourceTexture);
		RetainResource(destinationTexture);
		VkImageCopy copyRegion{
			.srcSubresource =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.mipLevel = region.sourceMipLevel,
					.baseArrayLayer = region.sourceBaseArrayLayer,
					.layerCount = region.layerCount,
				},
			.dstSubresource =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.mipLevel = region.destinationMipLevel,
					.baseArrayLayer = region.destinationBaseArrayLayer,
					.layerCount = region.layerCount,
				},
			.extent = {region.width, region.height, region.depth},
		};
		vkCmdCopyImage(m_CommandBuffer,
			sourceTexture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			destinationTexture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1,
			&copyRegion);
	}

}
