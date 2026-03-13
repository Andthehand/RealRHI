#pragma once
#include "Texture.h"
#include "TextureDesc.h"

#include "VulkanTextureView.h"

#include <Vulkan/vulkan.h>

namespace RealRHI {
	class VulkanTexture : public Texture {
	public:
		VulkanTexture(const VulkanDevice* device);
		~VulkanTexture();

		static Result Create(const VulkanDevice* device, const TextureDesc& desc, Ref<VulkanTexture>& outTexture);
		Result Init(const TextureDesc& desc);

		TextureView* GetTextureView() override {
			return &m_TextureView;
		}

		const TextureView* GetTextureView() const override {
			return &m_TextureView;
		}

	protected:
		friend class VulkanSwapchain;
		friend class VulkanTextureView;
		VkImage GetImage() const { return m_Image; }
		VkFormat GetFormat() const { return m_Format; }

		static Result CreateFromSwapChain(const VulkanDevice* device, VkFormat format, VkImage image, Ref<VulkanTexture>& outTexture);

		Result InitSwapChainTexture(VkFormat format, VkImage image);
	private:
		const VulkanDevice* m_Device = nullptr;
		VkImage m_Image = VK_NULL_HANDLE;
		VmaAllocation m_Allocation = VK_NULL_HANDLE;

		VkFormat m_Format = VK_FORMAT_UNDEFINED;
		bool m_IsExternal = false; // Whether the image is owned by us or external (e.g. swapchain)
		
		// This need to be at the end because of weird c++ rules
		// https://stackoverflow.com/questions/6308915/member-fields-order-of-construction
		VulkanTextureView m_TextureView; 
	};
}
