#pragma once
#include "CommandList.h"
#include "Texture.h"
#include "TextureDesc.h"
#include "VulkanDevice.h"
#include "VulkanResourceState.h"
#include "VulkanTextureView.h"

#include <volk.h>

#include <vma/vk_mem_alloc.h>

#include <mutex>
#include <vector>

namespace RealRHI {
	class VulkanTexture : public Texture {
	public:
		VulkanTexture(const VulkanDevice* device);
		~VulkanTexture();

		static Result Create(const VulkanDevice* device, const TextureDesc& desc, Ref<VulkanTexture>& outTexture);
		Result Init(const TextureDesc& desc);

		Result ChangeLayout(CommandList* cmd, TextureLayout newLayout);

		Result UploadData(const void* data, uint32_t size);

		TextureView* GetTextureView() override { return &m_TextureView; }

		const TextureView* GetTextureView() const override { return &m_TextureView; }

		uint32_t GetSampledImageDescriptorIndex() const override { return m_SampledImageDescriptorIndex; }
		uint32_t GetStorageImageDescriptorIndex() const override { return m_StorageImageDescriptorIndex; }
		uint32_t GetSamplerDescriptorIndex() const override { return m_SamplerDescriptorIndex; }

	protected:
		friend class VulkanSwapchain;
		friend class VulkanTextureView;
		friend class VulkanCommandList;
		friend class VulkanUploadContext;

		VkImage GetImage() const { return m_Image; }
		VkFormat GetFormat() const { return m_Format; }
		VkExtent3D GetExtent() const { return m_ImageExtent; }
		TextureUsage GetUsage() const { return m_Usage; }
		uint32_t GetMipLevelCount() const { return m_MipLevels; }
		uint32_t GetArrayLayerCount() const { return m_ArrayLayers; }
		bool IsExternal() const { return m_IsExternal; }

		VulkanTextureState GetSubmittedState(uint32_t mip, uint32_t layer) const {
			std::scoped_lock lock(m_Device->GetStateTrackingMutex());
			return m_SubresourceStates[mip * m_ArrayLayers + layer];
		}
		void CommitSubmittedState(uint32_t mip, uint32_t layer, const VulkanTextureState& state) {
			std::scoped_lock lock(m_Device->GetStateTrackingMutex());
			m_SubresourceStates[mip * m_ArrayLayers + layer] = state;
		}

		static Result CreateFromSwapChain(const VulkanDevice* device,
			VkFormat format,
			VkImage image,
			Ref<VulkanTexture>& outTexture);
		Result InitSwapChainTexture(VkFormat format, VkImage image);

		VkSamplerCreateInfo GetSampler(); // TODO: Add sampler desc?

	private:
		const VulkanDevice* m_Device = nullptr;
		VkImage m_Image = VK_NULL_HANDLE;
		VmaAllocation m_Allocation = VK_NULL_HANDLE;

		// Submission-ordered state; never modified during command recording.
		std::vector<VulkanTextureState> m_SubresourceStates;
		TextureUsage m_Usage = TextureUsage::None;
		VkFormat m_Format = VK_FORMAT_UNDEFINED;
		bool m_IsExternal = false; // Whether the image is owned by us or external (e.g. swapchain)

		VkExtent3D m_ImageExtent{
			.width = 0,
			.height = 0,
			.depth = 1,
		};
		uint32_t m_MipLevels = 1;
		uint32_t m_ArrayLayers = 1;
		uint32_t m_SampledImageDescriptorIndex = UINT32_MAX;
		uint32_t m_StorageImageDescriptorIndex = UINT32_MAX;
		uint32_t m_SamplerDescriptorIndex = UINT32_MAX;

		VulkanTextureView m_TextureView;
		VkSampler m_Sampler = VK_NULL_HANDLE;
	};
}
