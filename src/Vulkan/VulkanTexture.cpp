#include "VulkanTexture.h"
#include "VulkanConvertions.h"

#include "VulkanBuffer.h"
#include "VulkanCommandList.h"
#include "VulkanDescriptorManager.h"

namespace RealRHI {
    VulkanTexture::VulkanTexture(const VulkanDevice* device) 
        : m_Device(device) {}

	VulkanTexture::~VulkanTexture() {
		if (m_SampledImageDescriptorIndex != UINT32_MAX) {
			m_Device->GetDescriptorManager()->FreeImageDescriptor(m_SampledImageDescriptorIndex);
		}
		if (m_StorageImageDescriptorIndex != UINT32_MAX) {
			m_Device->GetDescriptorManager()->FreeImageDescriptor(m_StorageImageDescriptorIndex);
		}
		if (m_SamplerDescriptorIndex != UINT32_MAX) {
			m_Device->GetDescriptorManager()->FreeSamplerDescriptor(m_SamplerDescriptorIndex);
		}
		if (m_Sampler != VK_NULL_HANDLE) {
			vkDestroySampler(m_Device->GetDevice(), m_Sampler, nullptr);
		}

		if (!m_IsExternal) {
			vmaDestroyImage(m_Device->GetAllocator(), m_Image, m_Allocation);
		}
	}

    Result VulkanTexture::Create(const VulkanDevice* device, const TextureDesc& desc, Ref<VulkanTexture>& outTexture) {
        Ref<VulkanTexture> texture = Ref<VulkanTexture>::Create(device);
        Result res = texture->Init(desc);
        if (res != Result::Success) {
            return res;
        }

        outTexture = texture;
        return Result::Success;
    }

	Result VulkanTexture::Init(const TextureDesc& desc) {
		m_Format = Utils::TextureFormatToVkFormat(desc.format);
		m_Layout = TextureLayout::Undefined;
		m_Usage = desc.usage;
		m_MipLevels = desc.mipLevels;
		m_ArrayLayers = desc.arrayLayers;
		m_ImageExtent = {
			.width = desc.width,
			.height = desc.height,
			.depth = desc.depth,
		};

        VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = Utils::TextureDescToVkImageType(desc),
            .format = m_Format,
            .extent = m_ImageExtent,
            .mipLevels = m_MipLevels,
            .arrayLayers = m_ArrayLayers,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = Utils::TextureUsageToVkImageUsage(desc.usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, // I don't think we'll ever support sharing buffers between queues
            .initialLayout = Utils::TextureLayoutToVkImageLayout(m_Layout),
        };

        // Always allocate images on dedicated GPU memory
        VmaAllocationCreateInfo allocInfo{
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        if (vmaCreateImage(m_Device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan image.");
            return Result::Failed;
        }

		const bool createNativeImageView = Any(m_Usage & TextureUsage::RenderTarget) || Any(m_Usage & TextureUsage::DepthStencil);
		if (m_TextureView.Init(m_Device, this, TextureViewDesc{
			.mipLevelCount = m_MipLevels,
			.arrayLayerCount = m_ArrayLayers,
			}, createNativeImageView) != Result::Success) {
			return Result::Failed;
		}

		if (Any(m_Usage & TextureUsage::ShaderResource)) {
			m_SamplerDescriptorIndex = m_Device->GetDescriptorManager()->AllocateSamplerDescriptor(GetSampler());
			if (m_SamplerDescriptorIndex == UINT32_MAX) {
				return Result::Failed;
			}

			m_SampledImageDescriptorIndex = m_Device->GetDescriptorManager()->AllocateImageDescriptor(
				&m_TextureView,
				VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			);
			if (m_SampledImageDescriptorIndex == UINT32_MAX) {
				return Result::Failed;
			}
		}

		if (Any(m_Usage & TextureUsage::Storage)) {
			m_StorageImageDescriptorIndex = m_Device->GetDescriptorManager()->AllocateImageDescriptor(
				&m_TextureView,
				VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				VK_IMAGE_LAYOUT_GENERAL
			);
			if (m_StorageImageDescriptorIndex == UINT32_MAX) {
				return Result::Failed;
			}
		}

		return Result::Success;
    }

    Result VulkanTexture::ChangeLayout(CommandList* cmd, TextureLayout newLayout) {
		VulkanCommandList* vkCmd = static_cast<VulkanCommandList*>(cmd);

        VkImageSubresourceRange subresourceRange{
            .aspectMask = Utils::TextureFormatToVkImageAspect(Utils::VkFormatToTextureFormat(m_Format)), // TODO: Clean this up?
            .baseMipLevel = 0, //TODO: look into
            .levelCount = m_MipLevels,
            .baseArrayLayer = 0, //TODO: look into
            .layerCount = m_ArrayLayers,
        };

        VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = Utils::TextureLayoutToVkPipelineStageFlags(m_Layout),
            .srcAccessMask = Utils::TextureLayoutToVkAccessFlags(m_Layout),
            .dstStageMask = Utils::TextureLayoutToVkPipelineStageFlags(newLayout),
            .dstAccessMask = Utils::TextureLayoutToVkAccessFlags(newLayout),
            .oldLayout = Utils::TextureLayoutToVkImageLayout(m_Layout),
            .newLayout = Utils::TextureLayoutToVkImageLayout(newLayout),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_Image,
            .subresourceRange = subresourceRange,
        };

        VkDependencyInfo depInfo{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };

        vkCmdPipelineBarrier2(vkCmd->GetCommandBuffer(), &depInfo);
		m_Layout = newLayout;

        return Result::Success;
    }

    Result VulkanTexture::UploadData(const void* data, uint32_t size) {
        Ref<VulkanCommandList> commandList;
        Result res = VulkanCommandList::Create(m_Device, commandList);
        if (res != Result::Success) {
            return res;
        }

        res = commandList->Begin();
        if (res != Result::Success) {
            return res;
        }

        ChangeLayout(commandList.Raw(), TextureLayout::TransferDst);

        BufferDesc stagingBufferDesc{
            .size = size,
            .usage = BufferUsage::TransferSrc,
            .memoryUsage = MemoryUsage::CPUToGPU,
            .initialData = data,
        };
        Ref<VulkanBuffer> transferBuffer;
        res = VulkanBuffer::Create(m_Device, stagingBufferDesc, transferBuffer);
        if (res != Result::Success) {
            return res;
        }

        VkImageSubresourceLayers subresourceRange{
            .aspectMask = Utils::TextureFormatToVkImageAspect(Utils::VkFormatToTextureFormat(m_Format)), // TODO: Clean this up?
            .mipLevel = 0, //TODO: look into
            .baseArrayLayer = 0, //TODO: look into
            .layerCount = m_ArrayLayers,
        };

        VkBufferImageCopy copyRegion = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = subresourceRange,
            .imageExtent = m_ImageExtent,

        };

        vkCmdCopyBufferToImage(commandList->GetCommandBuffer(), transferBuffer->GetBuffer(), m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
        ChangeLayout(commandList.Raw(), TextureLayout::ShaderRead);

        res = commandList->End();
        if (res != Result::Success) {
            return res;
        }

        return m_Device->ImmediateSubmit(commandList.Raw());
    }

    Result VulkanTexture::CreateFromSwapChain(const VulkanDevice* device, VkFormat format, VkImage image, Ref<VulkanTexture>& outTexture) {
        Ref<VulkanTexture> texture = Ref<VulkanTexture>::Create(device);
        Result res = texture->InitSwapChainTexture(format, image);
        if (res != Result::Success) {
            return res;
        }

        outTexture = Ref<VulkanTexture>(texture);
        return Result::Success;
    }

	Result VulkanTexture::InitSwapChainTexture(VkFormat format, VkImage image) {
		m_IsExternal = true;
		m_Layout = TextureLayout::Undefined;
		m_Usage = TextureUsage::RenderTarget;
		m_Format = format;
		m_Image = image;

		if (m_TextureView.Init(m_Device, this, TextureViewDesc{}, true) != Result::Success) {
			return Result::Failed;
		}

		return Result::Success;
	}

    VkSamplerCreateInfo VulkanTexture::GetSampler() {
        VkSamplerCreateInfo samplerInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .mipLodBias = 0.0f,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0f,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .minLod = 0.0f,
            .maxLod = VK_LOD_CLAMP_NONE,
            .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };

        return samplerInfo;
    }
}
