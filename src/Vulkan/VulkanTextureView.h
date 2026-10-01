#pragma once
#include "TextureView.h"
#include "TextureViewDesc.h"

#include "Result.h"
#include "VulkanDevice.h"

#include <volk.h>

namespace RealRHI {
	class VulkanTextureView : public TextureView {
	public:
		VulkanTextureView() = default;
		~VulkanTextureView();

		// Does not use Create pattern because this should be an object not a ref
		// counted pointer because this is coupled to the lifetime of the texture
		Result Init(const VulkanDevice* device, const TextureViewDesc& desc);

	protected:
		friend class VulkanCommandList;
		friend class VulkanDescriptorManager;
		VkImageView GetImageView() const { return m_ImageView; }
		const VkImageViewCreateInfo& GetImageViewCreateInfo() const { return m_ImageViewCreateInfo; }
	private:
		const VulkanDevice* m_Device = nullptr;
		VkImage m_Image = VK_NULL_HANDLE;
		VkFormat m_Format = VK_FORMAT_UNDEFINED;

		VkImageViewCreateInfo m_ImageViewCreateInfo{};
		VkImageView m_ImageView = VK_NULL_HANDLE;
	};
}
