#include "VulkanTexture.h"
#include "VulkanConvertions.h"

namespace RealRHI {
    VulkanTexture::VulkanTexture(const VulkanDevice* device) : m_Device(device) {
    }

    VulkanTexture::~VulkanTexture() {
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

        VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = Utils::TextureDescToVkImageType(desc),
            .format = m_Format,
            .extent = {
                .width = desc.width,
                .height = desc.height,
                .depth = desc.depth,
            },
            .mipLevels = desc.mipLevels,
            .arrayLayers = desc.arrayLayers,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = Utils::TextureUsageToVkImageUsage(desc.usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, // I don't think we'll ever support sharing buffers between queues
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        // Always allocate images on dedicated GPU memory
        VmaAllocationCreateInfo allocInfo{
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        if (vmaCreateImage(m_Device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan image.");
            return Result::Failed;
        }

		return Result::Success;
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
        m_Format = format;
		m_Image = image;

        m_TextureView.Init(m_Device, TextureViewDesc{ .texture = this });

        return Result::Success;
    }
}
