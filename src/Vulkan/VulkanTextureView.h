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
		~VulkanTextureView();

		// Does not use Create pattern because this should be an object not a ref
		// counted pointer because this is coupled to the lifetime of the texture
		Result Init(const VulkanDevice* device, const TextureViewDesc& desc);

		VkImageView GetImageView() const { return m_ImageView; }
	private:
		const VulkanDevice* m_Device = nullptr;
		const VulkanTexture* m_Texture = nullptr;

		VkImageView m_ImageView = VK_NULL_HANDLE;
	};
}
