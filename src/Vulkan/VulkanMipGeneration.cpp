#include "VulkanCommandList.h"
#include "VulkanConvertions.h"
#include "VulkanTexture.h"

#include <algorithm>

namespace RealRHI {
	bool VulkanCommandList::ValidateMipGeneration(VulkanTexture* texture,
		const MipGenerationDesc& desc,
		TextureSubresourceRange& range) const {
		if (GetState() != CommandListState::Recording || m_Rendering || texture == nullptr) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"GenerateMipmaps: Requires recording outside rendering and a non-null texture.");

			return false;
		}

		if (desc.baseMipLevel >= texture->GetMipLevelCount() || desc.baseArrayLayer >= texture->GetArrayLayerCount()) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"GenerateMipmaps: Select an in-bounds base mip and array layer.");

			return false;
		}

		range = {desc.baseMipLevel, desc.mipLevelCount, desc.baseArrayLayer, desc.arrayLayerCount};
		if (range.mipLevelCount == 0) {
			range.mipLevelCount = texture->GetMipLevelCount() - range.baseMipLevel;
		}

		if (range.arrayLayerCount == 0) {
			range.arrayLayerCount = texture->GetArrayLayerCount() - range.baseArrayLayer;
		}

		if (range.mipLevelCount < 2 || range.mipLevelCount > texture->GetMipLevelCount() - range.baseMipLevel ||
			range.arrayLayerCount > texture->GetArrayLayerCount() - range.baseArrayLayer) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"GenerateMipmaps: Select at least two in-bounds mips and a valid layer range.");

			return false;
		}

		const TextureUsage usage = texture->GetUsage();
		if (texture->IsExternal() || !Any(usage & TextureUsage::TransferSrc) ||
			!Any(usage & TextureUsage::TransferDst) ||
			Utils::TextureFormatToVkImageAspect(Utils::VkFormatToTextureFormat(texture->GetFormat())) !=
				VK_IMAGE_ASPECT_COLOR_BIT) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"GenerateMipmaps: Requires an owned color texture with TransferSrc and TransferDst usage.");

			return false;
		}

		bool validFinalLayout = false;
		switch (desc.finalLayout) {
			case TextureLayout::General:
			case TextureLayout::TransferSrc:
			case TextureLayout::TransferDst:
				validFinalLayout = true;
				break;
			case TextureLayout::ShaderRead:
				validFinalLayout = Any(usage & TextureUsage::ShaderResource);
				break;
			case TextureLayout::ColorAttachment:
				validFinalLayout = Any(usage & TextureUsage::RenderTarget);
				break;
			default:
				break;
		}

		if (!validFinalLayout || (desc.filter != Filter::Nearest && desc.filter != Filter::Linear)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"GenerateMipmaps: Provide a supported filter and final layout compatible with texture usage.");

			return false;
		}

		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(m_Device->GetPhysicalDevice(), &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(m_Device->GetPhysicalDevice(), &familyCount, families.data());

		if ((families[m_QueueId.familyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"GenerateMipmaps: Blits require a graphics-capable queue family.");

			return false;
		}

		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(m_Device->GetPhysicalDevice(), texture->GetFormat(), &properties);
		VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
		if (desc.filter == Filter::Linear) {
			requiredFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		}

		if ((properties.optimalTilingFeatures & requiredFeatures) != requiredFeatures) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"GenerateMipmaps: Texture format does not support blitting with the selected filter.");

			return false;
		}

		for (uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.arrayLayerCount; ++layer) {
			if (GetExpectedLayout(texture, range.baseMipLevel, layer) == TextureLayout::Undefined) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"GenerateMipmaps: Initialize the source mip before generating its descendants.");

				return false;
			}
		}

		return true;
	}

	void VulkanCommandList::GenerateMipmaps(Texture* texture, const MipGenerationDesc& desc) {
		auto* vulkanTexture = static_cast<VulkanTexture*>(texture);
		TextureSubresourceRange range;
		if (!ValidateMipGeneration(vulkanTexture, desc, range)) {
			return;
		}

		RetainResource(texture);

		const auto transitionMip = [&](uint32_t mip, TextureLayout layout, ResourceState after) {
			BarrierDesc barriers;
			for (uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.arrayLayerCount; ++layer) {
				barriers.textures.push_back({
					.texture = texture,
					.before = {PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite},
					.after = after,
					.oldLayout = GetExpectedLayout(vulkanTexture, mip, layer),
					.newLayout = layout,
					.subresources = {mip, 1, layer, 1},
				});
			}

			Barrier(barriers);
		};

		VkFilter filter = VK_FILTER_NEAREST;
		if (desc.filter == Filter::Linear) {
			filter = VK_FILTER_LINEAR;
		}

		const VkExtent3D extent = vulkanTexture->GetExtent();
		const uint32_t endMip = range.baseMipLevel + range.mipLevelCount;
		for (uint32_t mip = range.baseMipLevel + 1; mip < endMip; ++mip) {
			transitionMip(mip - 1, TextureLayout::TransferSrc, {PipelineStage::Transfer, ResourceAccess::TransferRead});
			transitionMip(mip, TextureLayout::TransferDst, {PipelineStage::Transfer, ResourceAccess::TransferWrite});

			const VkOffset3D sourceEnd{
				.x = static_cast<int32_t>(std::max(1u, extent.width >> (mip - 1))),
				.y = static_cast<int32_t>(std::max(1u, extent.height >> (mip - 1))),
				.z = static_cast<int32_t>(std::max(1u, extent.depth >> (mip - 1))),
			};
			const VkOffset3D destinationEnd{
				.x = static_cast<int32_t>(std::max(1u, extent.width >> mip)),
				.y = static_cast<int32_t>(std::max(1u, extent.height >> mip)),
				.z = static_cast<int32_t>(std::max(1u, extent.depth >> mip)),
			};

			VkImageBlit blit{
				.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, range.baseArrayLayer, range.arrayLayerCount},
				.srcOffsets = {{0, 0, 0}, sourceEnd},
				.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, range.baseArrayLayer, range.arrayLayerCount},
				.dstOffsets = {{0, 0, 0}, destinationEnd},
			};

			RecordTextureUse(vulkanTexture,
				{mip - 1, 1, range.baseArrayLayer, range.arrayLayerCount},
				TextureLayout::TransferSrc,
				"GenerateMipmaps source");
			RecordTextureUse(vulkanTexture,
				{mip, 1, range.baseArrayLayer, range.arrayLayerCount},
				TextureLayout::TransferDst,
				"GenerateMipmaps destination");

			vkCmdBlitImage(m_CommandBuffer,
				vulkanTexture->GetImage(),
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				vulkanTexture->GetImage(),
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1,
				&blit,
				filter);
		}

		for (uint32_t mip = range.baseMipLevel; mip < endMip; ++mip) {
			transitionMip(mip, desc.finalLayout, desc.finalState);
		}
	}
}
