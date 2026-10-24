#pragma once
#include "Result.h"
#include "TextureView.h"
#include "TextureViewDesc.h"
#include "VulkanDevice.h"

#include <volk.h>

namespace RealRHI {
	class VulkanTexture;

	class VulkanTextureView : public TextureView {
	public:
		VulkanTextureView() = default;
		~VulkanTextureView();

		// The texture owns its default view and controls its lifetime.
		Result Init(const VulkanDevice* device,
			const VulkanTexture* texture,
			const TextureViewDesc& desc,
			bool createNativeImageView);

	protected:
		friend class VulkanCommandList;
		friend class VulkanDescriptorManager;
		VkImageView GetImageView() const { return m_ImageView; }
		bool HasNativeImageView() const { return m_ImageView != VK_NULL_HANDLE; }
		const VkImageViewCreateInfo& GetImageViewCreateInfo() const { return m_ImageViewCreateInfo; }
		const VulkanTexture* GetTexture() const { return m_Texture; }

	private:
		VkImageViewCreateInfo GetImageViewCreateInfo(const TextureViewDesc& desc) const;

	private:
		const VulkanDevice* m_Device = nullptr;
		const VulkanTexture* m_Texture = nullptr;
		VkImage m_Image = VK_NULL_HANDLE;
		VkFormat m_Format = VK_FORMAT_UNDEFINED;

		VkImageViewCreateInfo m_ImageViewCreateInfo{};
		VkImageView m_ImageView = VK_NULL_HANDLE;
	};
}
