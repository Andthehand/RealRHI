#include "VulkanTransferValidation.h"

#include "VulkanConversions.h"

#include <string>

namespace RealRHI {
	VulkanTransferValidation::VulkanTransferValidation(const VulkanDevice* device,
		const VulkanCommandResourceTracker& resources)
		: m_Device(device), m_Resources(resources) { }

	bool VulkanTransferValidation::Reject(const char* operation, const char* cause) const {
		m_Device->SendDebugMessage(
			DebugSeverity::Error, DebugMessageType::General, (std::string(operation) + ": " + cause).c_str());
		return false;
	}

	bool VulkanTransferValidation::IsBufferRangeValid(const VulkanBuffer* buffer, uint64_t offset, uint64_t size) {
		return size != 0 && offset <= buffer->GetSize() && size <= buffer->GetSize() - offset;
	}

	bool VulkanTransferValidation::IsSubresourceRangeValid(const VulkanTexture* texture,
		TextureSubresourceRange range) {
		return range.mipLevelCount != 0 && range.arrayLayerCount != 0 &&
			range.baseMipLevel < texture->GetMipLevelCount() &&
			range.mipLevelCount <= texture->GetMipLevelCount() - range.baseMipLevel &&
			range.baseArrayLayer < texture->GetArrayLayerCount() &&
			range.arrayLayerCount <= texture->GetArrayLayerCount() - range.baseArrayLayer;
	}

	bool VulkanTransferValidation::FitsMipExtent(const VulkanTexture* texture, uint32_t mip, VkExtent3D extent) {
		const auto available = texture->GetMipExtent(mip);
		return extent.width <= available.width && extent.height <= available.height && extent.depth <= available.depth;
	}

	bool VulkanTransferValidation::TryGetPackedSize(VkExtent3D extent,
		uint32_t layers,
		uint64_t texelSize,
		uint64_t& outSize) {
		if (texelSize == 0) {
			return false;
		}
		uint64_t size = texelSize;
		for (uint32_t factor : {extent.width, extent.height, extent.depth, layers}) {
			if (factor == 0 || size > UINT64_MAX / factor) {
				return false;
			}
			size *= factor;
		}
		outSize = size;
		return true;
	}

	bool VulkanTransferValidation::SupportsFinalLayout(TextureUsage usage, TextureLayout layout) {
		switch (layout) {
			case TextureLayout::General:
				return true;
			case TextureLayout::TransferSrc:
				return Any(usage & TextureUsage::TransferSrc);
			case TextureLayout::TransferDst:
				return Any(usage & TextureUsage::TransferDst);
			case TextureLayout::ShaderRead:
				return Any(usage & TextureUsage::ShaderResource);
			case TextureLayout::ColorAttachment:
				return Any(usage & TextureUsage::RenderTarget);
			default:
				return false;
		}
	}

	bool VulkanTransferValidation::PrepareBufferCopy(const VulkanBuffer* source,
		const VulkanBuffer* destination,
		const BufferCopyRegion& region,
		VkBufferCopy& outCopy) const {
		if (region.size == 0) {
			return Reject("CopyBuffer", "Provide a nonzero region size.");
		}
		if (!source || !destination || !Any(source->GetUsage() & BufferUsage::TransferSrc) ||
			!Any(destination->GetUsage() & BufferUsage::TransferDst)) {
			return Reject("CopyBuffer",
				"Provide non-null source and destination buffers with TransferSrc and TransferDst usage respectively.");
		}
		if (!IsBufferRangeValid(source, region.sourceOffset, region.size) ||
			!IsBufferRangeValid(destination, region.destinationOffset, region.size)) {
			return Reject("CopyBuffer", "Copy region is out of bounds of source or destination buffer.");
		}
		if (source == destination && region.sourceOffset < region.destinationOffset + region.size &&
			region.destinationOffset < region.sourceOffset + region.size) {
			return Reject("CopyBuffer", "Source and destination ranges must not overlap within the same buffer.");
		}
		outCopy = {.srcOffset = region.sourceOffset, .dstOffset = region.destinationOffset, .size = region.size};
		return true;
	}

	bool VulkanTransferValidation::PrepareBufferTextureCopy(VulkanTransferDirection direction,
		const VulkanBuffer* buffer,
		const VulkanTexture* texture,
		const BufferTextureCopyRegion& region,
		VulkanBufferTextureCopy& outCopy) const {
		const bool upload = direction == VulkanTransferDirection::Upload;
		const char* operation = "CopyTextureToBuffer";
		BufferUsage bufferUsage = BufferUsage::TransferDst;
		TextureUsage textureUsage = TextureUsage::TransferSrc;
		TextureLayout layout = TextureLayout::TransferSrc;
		if (upload) {
			operation = "CopyBufferToTexture";
			bufferUsage = BufferUsage::TransferSrc;
			textureUsage = TextureUsage::TransferDst;
			layout = TextureLayout::TransferDst;
		}
		if (region.width == 0 || region.height == 0 || region.depth == 0 || region.layerCount == 0) {
			return Reject(operation, "Provide nonzero copy dimensions and layer count.");
		}
		if (texture && texture->GetSampleCount() != 1) {
			return Reject(operation,
				"Buffer transfers require a single-sampled texture; resolve multisampled textures before readback.");
		}
		const TextureSubresourceRange subresources{region.mipLevel, 1, region.baseArrayLayer, region.layerCount};
		if (!buffer || !texture || !Any(buffer->GetUsage() & bufferUsage) || !Any(texture->GetUsage() & textureUsage) ||
			!IsSubresourceRangeValid(texture, subresources) ||
			!m_Resources.HasRecordedLayout(
				texture, region.mipLevel, region.baseArrayLayer, region.layerCount, layout)) {
			return Reject(operation, "Invalid source or destination usage, recorded layout, or subresource range.");
		}
		const VkExtent3D extent{region.width, region.height, region.depth};
		if (!FitsMipExtent(texture, region.mipLevel, extent) || region.bufferOffset >= buffer->GetSize()) {
			return Reject(operation, "Copy dimensions exceed mip extent or buffer offset is out of bounds.");
		}
		const auto format = Utils::VkFormatToTextureFormat(texture->GetFormat());
		const uint32_t texelSize = Utils::TextureFormatByteSize(format);
		// Readback retains its existing RGBA8 restriction; upload supports packed color formats.
		if (!upload && texture->GetFormat() != VK_FORMAT_R8G8B8A8_UNORM) {
			return Reject(operation, "Unsupported texture format for readback; use RGBA8_UNorm.");
		}
		uint64_t byteSize = 0;
		if (!TryGetPackedSize(extent, region.layerCount, texelSize, byteSize) || region.bufferOffset % texelSize != 0 ||
			!IsBufferRangeValid(buffer, region.bufferOffset, byteSize)) {
			return Reject(operation, "Unsupported format, misaligned buffer offset, or insufficient buffer capacity.");
		}
		outCopy = {
			.region =
				{.bufferOffset = region.bufferOffset,
					.imageSubresource =
						{.aspectMask = Utils::TextureFormatToVkImageAspect(format),
							.mipLevel = region.mipLevel,
							.baseArrayLayer = region.baseArrayLayer,
							.layerCount = region.layerCount},
					.imageExtent = extent},
			.byteSize = byteSize
		};
		return true;
	}

	bool VulkanTransferValidation::PrepareTextureCopy(const VulkanTexture* source,
		const VulkanTexture* destination,
		const TextureCopyRegion& region,
		VkImageCopy& outCopy) const {
		if (region.width == 0 || region.height == 0 || region.depth == 0 || region.layerCount == 0) {
			return Reject("CopyTexture", "Provide nonzero copy dimensions and layer count.");
		}
		if (source && destination && source->GetSampleCount() != destination->GetSampleCount()) {
			return Reject("CopyTexture",
				"Source and destination must have matching sample counts; use attachment resolves to reduce samples.");
		}
		const TextureSubresourceRange sourceRange{
			region.sourceMipLevel, 1, region.sourceBaseArrayLayer, region.layerCount
		};
		const TextureSubresourceRange destinationRange{
			region.destinationMipLevel, 1, region.destinationBaseArrayLayer, region.layerCount
		};
		if (!source || !destination || source == destination || !Any(source->GetUsage() & TextureUsage::TransferSrc) ||
			!Any(destination->GetUsage() & TextureUsage::TransferDst) ||
			source->GetFormat() != destination->GetFormat() || source->GetFormat() != VK_FORMAT_R8G8B8A8_UNORM ||
			!IsSubresourceRangeValid(source, sourceRange) || !IsSubresourceRangeValid(destination, destinationRange) ||
			!m_Resources.HasRecordedLayout(source,
				region.sourceMipLevel,
				region.sourceBaseArrayLayer,
				region.layerCount,
				TextureLayout::TransferSrc) ||
			!m_Resources.HasRecordedLayout(destination,
				region.destinationMipLevel,
				region.destinationBaseArrayLayer,
				region.layerCount,
				TextureLayout::TransferDst)) {
			return Reject("CopyTexture", "Invalid textures, formats, recorded layouts, or subresource range.");
		}
		const VkExtent3D extent{region.width, region.height, region.depth};
		if (!FitsMipExtent(source, region.sourceMipLevel, extent) ||
			!FitsMipExtent(destination, region.destinationMipLevel, extent)) {
			return Reject("CopyTexture", "Copy dimensions exceed source or destination mip extent.");
		}
		outCopy = {
			.srcSubresource =
				{VK_IMAGE_ASPECT_COLOR_BIT, region.sourceMipLevel, region.sourceBaseArrayLayer, region.layerCount},
			.dstSubresource =
				{VK_IMAGE_ASPECT_COLOR_BIT,
					region.destinationMipLevel,
					region.destinationBaseArrayLayer,
					region.layerCount},
			.extent = extent
		};
		return true;
	}

	bool VulkanTransferValidation::PrepareMipGeneration(const VulkanTexture* texture,
		const MipGenerationDesc& desc,
		QueueId queue,
		TextureSubresourceRange& outRange) const {
		TextureSubresourceRange range;
		if (texture == nullptr) {
			return Reject("GenerateMipmaps", "Requires recording outside rendering and a non-null texture.");
		}

		if (desc.baseMipLevel >= texture->GetMipLevelCount() || desc.baseArrayLayer >= texture->GetArrayLayerCount()) {
			return Reject("GenerateMipmaps", "Select an in-bounds base mip and array layer.");
		}

		range = {desc.baseMipLevel, desc.mipLevelCount, desc.baseArrayLayer, desc.arrayLayerCount};
		if (range.mipLevelCount == 0) {
			range.mipLevelCount = texture->GetMipLevelCount() - range.baseMipLevel;
		}

		if (range.arrayLayerCount == 0) {
			range.arrayLayerCount = texture->GetArrayLayerCount() - range.baseArrayLayer;
		}

		if (range.mipLevelCount < 2 || !IsSubresourceRangeValid(texture, range)) {
			return Reject("GenerateMipmaps", "Select at least two in-bounds mips and a valid layer range.");
		}

		const TextureUsage usage = texture->GetUsage();
		if (texture->IsExternal() || !Any(usage & TextureUsage::TransferSrc) ||
			!Any(usage & TextureUsage::TransferDst) ||
			Utils::TextureFormatToVkImageAspect(Utils::VkFormatToTextureFormat(texture->GetFormat())) !=
				VK_IMAGE_ASPECT_COLOR_BIT) {
			return Reject("GenerateMipmaps", "Requires an owned color texture with TransferSrc and TransferDst usage.");
		}

		if (!SupportsFinalLayout(usage, desc.finalLayout) ||
			(desc.filter != Filter::Nearest && desc.filter != Filter::Linear)) {
			return Reject(
				"GenerateMipmaps", "Provide a supported filter and final layout compatible with texture usage.");
		}

		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(m_Device->GetPhysicalDevice(), &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(m_Device->GetPhysicalDevice(), &familyCount, families.data());

		if ((families[queue.familyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
			return Reject("GenerateMipmaps", "Blits require a graphics-capable queue family.");
		}

		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(m_Device->GetPhysicalDevice(), texture->GetFormat(), &properties);
		VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
		if (desc.filter == Filter::Linear) {
			requiredFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		}

		if ((properties.optimalTilingFeatures & requiredFeatures) != requiredFeatures) {
			return Reject("GenerateMipmaps", "Texture format does not support blitting with the selected filter.");
		}

		for (uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.arrayLayerCount; ++layer) {
			if (m_Resources.GetExpectedLayout(texture, range.baseMipLevel, layer) == TextureLayout::Undefined) {
				return Reject("GenerateMipmaps", "Initialize the source mip before generating its descendants.");
			}
		}

		outRange = range;
		return true;
	}
}
