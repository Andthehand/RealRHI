#include "VulkanTextureView.h"

#include "VulkanConvertions.h"
#include "VulkanTexture.h"

namespace RealRHI {
	VulkanTextureView::~VulkanTextureView() {
		if (m_ImageView != VK_NULL_HANDLE) {
			vkDestroyImageView(m_Device->GetDevice(), m_ImageView, nullptr);
		}
	}

	Result VulkanTextureView::Init(const VulkanDevice* device, const VulkanTexture* texture, const TextureViewDesc& desc, bool createNativeImageView) {
		m_Device = device;
		m_Image = texture->GetImage();
		m_Format = texture->GetFormat();
		m_ImageView = VK_NULL_HANDLE;
		m_ImageViewCreateInfo = GetImageViewCreateInfo(desc);

		if (!createNativeImageView) {
			return Result::Success;
		}

		if (vkCreateImageView(m_Device->GetDevice(), &m_ImageViewCreateInfo, nullptr, &m_ImageView) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan texture view.");
			return Result::Failed;
		}

		return Result::Success;
	}

	VkImageViewCreateInfo VulkanTextureView::GetImageViewCreateInfo(const TextureViewDesc& desc) const {
		constexpr VkComponentMapping componentMapping{
			.r = VK_COMPONENT_SWIZZLE_IDENTITY,
			.g = VK_COMPONENT_SWIZZLE_IDENTITY,
			.b = VK_COMPONENT_SWIZZLE_IDENTITY,
			.a = VK_COMPONENT_SWIZZLE_IDENTITY,
		};
		VkImageSubresourceRange subresourceRange{
			.aspectMask = Utils::TextureFormatToVkImageAspect(Utils::VkFormatToTextureFormat(m_Format)),
			.baseMipLevel = desc.baseMipLevel,
			.levelCount = desc.mipLevelCount,
			.baseArrayLayer = desc.baseArrayLayer,
			.layerCount = desc.arrayLayerCount,
		};
		VkImageViewCreateInfo imageViewCreateInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = m_Image,
			.viewType = Utils::TextureViewTypeToVkImageViewType(desc.type),
			.format = m_Format,
			.components = componentMapping,
			.subresourceRange = subresourceRange,
		};

		return imageViewCreateInfo;
	}
}
