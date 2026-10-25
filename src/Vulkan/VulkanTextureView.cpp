#include "VulkanTextureView.h"

#include "VulkanConvertions.h"
#include "VulkanDescriptorManager.h"
#include "VulkanTexture.h"

namespace RealRHI {
	VulkanTextureView::~VulkanTextureView() {
		m_Device->GetDescriptorManager()->FreeImageDescriptor(m_SampledImageDescriptorIndex);
		m_Device->GetDescriptorManager()->FreeImageDescriptor(m_StorageImageDescriptorIndex);

		if (m_ImageView != VK_NULL_HANDLE) {
			vkDestroyImageView(m_Device->GetDevice(), m_ImageView, nullptr);
		}
	}

	Result VulkanTextureView::Init(const VulkanDevice* device,
		VulkanTexture* texture,
		const TextureViewDesc& desc,
		bool retainTexture) {
		m_Device = device;
		m_Texture = texture;

		if (texture == nullptr) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "CreateTextureView: Provide a non-null texture.");

			return Result::InvalidParameter;
		}

		const TextureUsage allowedUsage = texture->GetUsage() &
			(TextureUsage::ShaderResource | TextureUsage::Storage | TextureUsage::RenderTarget |
				TextureUsage::DepthStencil);

		m_Usage = desc.usage;
		if (m_Usage == TextureUsage::None) {
			m_Usage = allowedUsage;
		}

		if (!ValidateDescription(desc, allowedUsage)) {
			return Result::InvalidParameter;
		}

		if (retainTexture && (texture->IsExternal() || m_Usage == TextureUsage::None)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateTextureView: Custom views require an owned texture with sampled, storage or attachment usage.");

			return Result::InvalidParameter;
		}

		if (retainTexture) {
			m_RetainedTexture.Reset(texture);
		}

		m_Subresources = {desc.baseMipLevel, desc.mipLevelCount, desc.baseArrayLayer, desc.arrayLayerCount};

		const VkImageViewCreateInfo viewInfo = BuildImageViewCreateInfo(desc.type);

		if (Any(m_Usage & (TextureUsage::RenderTarget | TextureUsage::DepthStencil))) {
			if (vkCreateImageView(m_Device->GetDevice(), &viewInfo, nullptr, &m_ImageView) != VK_SUCCESS) {
				m_Device->SendDebugMessage(
					DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan texture view.");

				return Result::Failed;
			}
		}

		if (m_ImageView != VK_NULL_HANDLE) {
			const Result nameResult = m_Device->SetDebugName(VK_OBJECT_TYPE_IMAGE_VIEW, m_ImageView, desc.debugName);
			if (nameResult != Result::Success) {
				return nameResult;
			}
		}

		return CreateDescriptors(viewInfo, desc.debugName);
	}

	bool VulkanTextureView::ValidateDescription(const TextureViewDesc& desc, TextureUsage allowedUsage) const {
		if (desc.mipLevelCount == 0 || desc.baseMipLevel >= m_Texture->GetMipLevelCount() ||
			desc.mipLevelCount > m_Texture->GetMipLevelCount() - desc.baseMipLevel || desc.arrayLayerCount == 0 ||
			desc.baseArrayLayer >= m_Texture->GetArrayLayerCount() ||
			desc.arrayLayerCount > m_Texture->GetArrayLayerCount() - desc.baseArrayLayer) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateTextureView: Select nonzero mip/layer counts within the texture's bounds.");

			return false;
		}

		if ((static_cast<uint32_t>(m_Usage) & ~static_cast<uint32_t>(allowedUsage)) != 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateTextureView: View usage must be a subset of the texture's sampled, storage or attachment "
				"usage.");

			return false;
		}

		if (Any(m_Usage & (TextureUsage::Storage | TextureUsage::RenderTarget | TextureUsage::DepthStencil)) &&
			desc.mipLevelCount != 1) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateTextureView: Storage and attachment views require exactly one mip level.");

			return false;
		}

		if (!IsViewTypeCompatible(desc)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateTextureView: Select a supported 2D, 2D array or 3D view matching the texture dimensions and "
				"layer count.");

			return false;
		}

		return true;
	}

	bool VulkanTextureView::IsViewTypeCompatible(const TextureViewDesc& desc) const {
		const auto extent = m_Texture->GetExtent();
		const bool attachmentUsage = Any(m_Usage & (TextureUsage::RenderTarget | TextureUsage::DepthStencil));

		switch (desc.type) {
			case TextureViewType::View2D:
				return extent.depth == 1 && desc.arrayLayerCount == 1;

			case TextureViewType::View2DArray:
				return extent.depth == 1;

			case TextureViewType::View3D:
				return extent.depth > 1 && desc.baseArrayLayer == 0 && desc.arrayLayerCount == 1 && !attachmentUsage;

			default:
				// Only 2D, 2D array and 3D views are supported curerntly.
				return false;
		}
	}

	VkImageViewCreateInfo VulkanTextureView::BuildImageViewCreateInfo(TextureViewType type) const {
		const VkFormat format = m_Texture->GetFormat();

		constexpr VkComponentMapping componentMapping{
			.r = VK_COMPONENT_SWIZZLE_IDENTITY,
			.g = VK_COMPONENT_SWIZZLE_IDENTITY,
			.b = VK_COMPONENT_SWIZZLE_IDENTITY,
			.a = VK_COMPONENT_SWIZZLE_IDENTITY,
		};

		const VkImageSubresourceRange subresourceRange{
			.aspectMask = Utils::TextureFormatToVkImageAspect(Utils::VkFormatToTextureFormat(format)),
			.baseMipLevel = m_Subresources.baseMipLevel,
			.levelCount = m_Subresources.mipLevelCount,
			.baseArrayLayer = m_Subresources.baseArrayLayer,
			.layerCount = m_Subresources.arrayLayerCount,
		};

		return {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = m_Texture->GetImage(),
			.viewType = Utils::TextureViewTypeToVkImageViewType(type),
			.format = format,
			.components = componentMapping,
			.subresourceRange = subresourceRange,
		};
	}

	Result VulkanTextureView::CreateDescriptors(const VkImageViewCreateInfo& viewInfo, const char* debugName) {
		if (Any(m_Usage & TextureUsage::ShaderResource)) {
			VkImageViewCreateInfo sampledViewInfo = viewInfo;
			if ((sampledViewInfo.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
				// Sample depth independently from stencil; the native attachment view can contain both.
				sampledViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			}

			VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			if (Any(m_Usage & TextureUsage::Storage)) {
				layout = VK_IMAGE_LAYOUT_GENERAL;
			}

			m_SampledImageDescriptorIndex = m_Device->GetDescriptorManager()->AllocateImageDescriptor(
				sampledViewInfo, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, layout, debugName);
			if (m_SampledImageDescriptorIndex == UINT32_MAX) {
				return Result::Failed;
			}
		}

		if (Any(m_Usage & TextureUsage::Storage)) {
			m_StorageImageDescriptorIndex = m_Device->GetDescriptorManager()->AllocateImageDescriptor(
				viewInfo, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_LAYOUT_GENERAL, debugName);
			if (m_StorageImageDescriptorIndex == UINT32_MAX) {
				return Result::Failed;
			}
		}

		return Result::Success;
	}
}
