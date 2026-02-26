#pragma once
#include "TextureView.h"
#include "TextureViewDesc.h"

#include "VulkanDevice.h"

#include <Vulkan/vulkan.h>

namespace RealRHI {
	class VulkanTexture;

	class VulkanTextureView : public TextureView {
	public:
		VulkanTextureView() = default;
		VulkanTextureView(const VulkanDevice* device, const TextureViewDesc& desc);
		~VulkanTextureView();

		//TODO: Remove jank
		VkImageView GetImageView() const { return m_ImageView; }
	private:
		const VulkanDevice* m_Device = nullptr;
		const VulkanTexture* m_Texture = nullptr;

		VkImageView m_ImageView;
	};
}
