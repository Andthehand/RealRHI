#pragma once
#include "VulkanTexture.h"

namespace RealRHI {
	VulkanTexture::VulkanTexture(const VulkanDevice* device, VkFormat format, VkImage image) 
		: m_Device(device), m_Image(image), m_IsExternal(true), m_Format(format),
		  m_TextureView(device) {
	}

	Result VulkanTexture::Init() {
		return m_TextureView.Init(TextureViewDesc{ .texture = this });
	}

	VulkanTexture::VulkanTexture(const VulkanDevice* device, const TextureDesc& desc) 
		: m_Device(device) {
		//TODO: implement
	}

	VulkanTexture::~VulkanTexture() {
		if (!m_IsExternal) {
			vkDestroyImage(m_Device->GetDevice(), m_Image, nullptr);
		}
	}
}
