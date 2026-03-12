#include "VulkanTextureView.h"

#include "VulkanTexture.h"
#include "VulkanConvertions.h"

namespace RealRHI {
    VulkanTextureView::~VulkanTextureView() {
        if (m_ImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_Device->GetDevice(), m_ImageView, nullptr);
        }
    }

    Result VulkanTextureView::Init(const VulkanDevice* device, const TextureViewDesc& desc) {
		m_Device = device;

        m_Texture = static_cast<const VulkanTexture*>(desc.texture);
        constexpr VkComponentMapping componentMapping{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        };
        VkImageSubresourceRange subresourceRange{
            .aspectMask = Utils::TextureFormatToVkImageAspect(Utils::VkFormatToTextureFormat(m_Texture->GetFormat())),
            .baseMipLevel = desc.baseMipLevel,
            .levelCount = desc.mipLevelCount,
            .baseArrayLayer = desc.baseArrayLayer,
            .layerCount = desc.arrayLayerCount,
        };
        VkImageViewCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = m_Texture->GetImage(),
            .viewType = Utils::TextureViewTypeToVkImageViewType(desc.type),
            .format = m_Texture->GetFormat(),
            .components = componentMapping,
            .subresourceRange = subresourceRange,
        };

        if (vkCreateImageView(m_Device->GetDevice(), &createInfo, nullptr, &m_ImageView) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan texture view.");
            return Result::Failed;
        }

        return Result::Success;
    }
}
