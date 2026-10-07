#pragma once
#include "TextureView.h"
#include "TextureViewDesc.h"

#include "Result.h"
#include "VulkanDevice.h"

#include <volk.h>

namespace RealRHI {
	class VulkanTexture;

	class VulkanTextureView : public TextureView {
	public:
		VulkanTextureView() = default;
		~VulkanTextureView();

		// Does not use Create pattern because this should be an object not a ref
		// counted pointer because this is coupled to the lifetime of the texture
		Result Init(const VulkanDevice* device, const VulkanTexture* texture, const TextureViewDesc& desc, bool createNativeImageView);

	protected:
		friend class VulkanCommandList;
		friend class VulkanDescriptorManager;
		VkImageView GetImageView() const { return m_ImageView; }
		bool HasNativeImageView() const { return m_ImageView != VK_NULL_HANDLE; }
		const VkImageViewCreateInfo& GetImageViewCreateInfo() const { return m_ImageViewCreateInfo; }
	private:
		VkImageViewCreateInfo GetImageViewCreateInfo(const TextureViewDesc& desc) const;
		const VulkanDevice* m_Device = nullptr;
		VkImage m_Image = VK_NULL_HANDLE;
		VkFormat m_Format = VK_FORMAT_UNDEFINED;

		VkImageViewCreateInfo m_ImageViewCreateInfo{};
		VkImageView m_ImageView = VK_NULL_HANDLE;
	};
}
