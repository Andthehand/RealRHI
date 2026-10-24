#pragma once
#include "BarrierDesc.h"
#include "Result.h"
#include "Texture.h"
#include "TextureView.h"
#include "TextureViewDesc.h"
#include "VulkanDevice.h"

#include <volk.h>

namespace RealRHI {
	class VulkanTexture;

	class VulkanTextureView : public TextureView {
	public:
		VulkanTextureView() = default;
		~VulkanTextureView() override;

		// Default views borrow their parent to avoid a reference cycle; custom views retain it.
		Result Init(const VulkanDevice* device,
			VulkanTexture* texture,
			const TextureViewDesc& desc,
			bool retainTexture = false);

		uint32_t GetSampledImageDescriptorIndex() const override { return m_SampledImageDescriptorIndex; }
		uint32_t GetStorageImageDescriptorIndex() const override { return m_StorageImageDescriptorIndex; }
	protected:
		friend class VulkanCommandList;

		bool HasNativeImageView() const { return m_ImageView != VK_NULL_HANDLE; }
		
		VkImageView GetImageView() const { return m_ImageView; }
		TextureSubresourceRange GetSubresources() const { return m_Subresources; }

		const VulkanTexture* GetTexture() const { return m_Texture; }
		TextureUsage GetUsage() const { return m_Usage; }
	private:
		bool ValidateDescription(const TextureViewDesc& desc, TextureUsage allowedUsage) const;
		bool IsViewTypeCompatible(const TextureViewDesc& desc) const;

		VkImageViewCreateInfo BuildImageViewCreateInfo(TextureViewType type) const;
		Result CreateDescriptors(const VkImageViewCreateInfo& viewInfo);

	private:
		const VulkanDevice* m_Device = nullptr;
		const VulkanTexture* m_Texture = nullptr;
		Ref<Texture> m_RetainedTexture;
		TextureUsage m_Usage = TextureUsage::None;
		TextureSubresourceRange m_Subresources{};

		VkImageView m_ImageView = VK_NULL_HANDLE;

		uint32_t m_SampledImageDescriptorIndex = UINT32_MAX;
		uint32_t m_StorageImageDescriptorIndex = UINT32_MAX;
	};
}
