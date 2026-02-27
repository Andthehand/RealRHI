#pragma once
#include "Texture.h"
#include "TextureDesc.h"

#include "VulkanTextureView.h"

#include <Vulkan/vulkan.h>

namespace RealRHI {
	class VulkanTexture : public Texture {
	public:
		VulkanTexture(const VulkanDevice* device, VkFormat format, VkImage image); // For swapchain images
		VulkanTexture(const VulkanDevice* device, const TextureDesc& desc);
		~VulkanTexture();

		TextureView* GetTextureView() override {
			return &m_TextureView;
		}

		const TextureView* GetTextureView() const override {
			return &m_TextureView;
		}

		// TODO: make this protected
		VkImage GetImage() const { return m_Image; }
		VkFormat GetFormat() const { return m_Format; }
	private:
		const VulkanDevice* m_Device;
		VkImage m_Image;

		VkFormat m_Format;
		bool m_IsExternal = false; // Whether the image is owned by us or external (e.g. swapchain)
		
		// This need to be at the end because of weird c++ rules
		// https://stackoverflow.com/questions/6308915/member-fields-order-of-construction
		VulkanTextureView m_TextureView; 

		friend class VulkanTextureView;
	};
}
