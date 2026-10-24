#pragma once
#include "Sampler.h"
#include "SamplerDesc.h"
#include "VulkanDevice.h"

namespace RealRHI {
	class VulkanSampler final : public Sampler {
	public:
		explicit VulkanSampler(const VulkanDevice* device);
		~VulkanSampler() override;

		Result Init(const SamplerDesc& desc);
		uint32_t GetDescriptorIndex() const override { return m_DescriptorIndex; }

	private:
		const VulkanDevice* m_Device;
		uint32_t m_DescriptorIndex = UINT32_MAX;
	};
}
