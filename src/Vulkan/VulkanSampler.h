#pragma once
#include "Sampler.h"
#include "SamplerDesc.h"
#include "Result.h"

#include "VulkanDevice.h"

#include <Vulkan/vulkan.h>

namespace RealRHI {
    class VulkanSampler : public Sampler {
    public:
        VulkanSampler(const VulkanDevice* device);
        ~VulkanSampler();

        static Result Create(const VulkanDevice* device, const SamplerDesc& desc, Ref<VulkanSampler>& outSampler);
        Result Init(const SamplerDesc& desc);

    protected:
        friend class VulkanCommandList;
        VkSampler GetSampler() const { return m_Sampler; }
    private:
        const VulkanDevice* m_Device = nullptr;
        VkSampler m_Sampler = VK_NULL_HANDLE;
    };
}
