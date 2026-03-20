#include "VulkanSampler.h"
#include "VulkanConvertions.h"

namespace RealRHI {
    VulkanSampler::VulkanSampler(const VulkanDevice* device) : m_Device(device) {
    }

    VulkanSampler::~VulkanSampler() {
        if (m_Sampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_Device->GetDevice(), m_Sampler, nullptr);
        }
    }

    Result VulkanSampler::Create(const VulkanDevice* device, const SamplerDesc& desc, Ref<VulkanSampler>& outSampler) {
        Ref<VulkanSampler> sampler = Ref<VulkanSampler>::Create(device);
        Result res = sampler->Init(desc);
        if (res != Result::Success) {
            return res;
        }

        outSampler = sampler;
        return Result::Success;
    }

    Result VulkanSampler::Init(const SamplerDesc& desc) {
        VkSamplerCreateInfo samplerInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = Utils::SamplerFilterToVkFilter(desc.magFilter),
            .minFilter = Utils::SamplerFilterToVkFilter(desc.minFilter),
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = Utils::SamplerAddressModeToVkAddressMode(desc.addressModeU),
            .addressModeV = Utils::SamplerAddressModeToVkAddressMode(desc.addressModeV),
            .addressModeW = Utils::SamplerAddressModeToVkAddressMode(desc.addressModeW),
            .mipLodBias = 0.0f,
            .anisotropyEnable = desc.anisotropyEnable ? VK_TRUE : VK_FALSE,
            .maxAnisotropy = desc.maxAnisotropy,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .minLod = 0.0f,
            .maxLod = VK_LOD_CLAMP_NONE,
            .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };

        if (vkCreateSampler(m_Device->GetDevice(), &samplerInfo, nullptr, &m_Sampler) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan sampler.");
            return Result::Failed;
        }

        return Result::Success;
    }
}
