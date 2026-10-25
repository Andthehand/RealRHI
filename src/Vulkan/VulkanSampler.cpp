#include "VulkanSampler.h"

#include "VulkanConvertions.h"
#include "VulkanDescriptorManager.h"

#include <cmath>

namespace RealRHI {
	namespace {
		VkSamplerAddressMode ConvertAddressMode(SamplerAddressMode mode) {
			switch (mode) {
				case SamplerAddressMode::Repeat:
					return VK_SAMPLER_ADDRESS_MODE_REPEAT;
				case SamplerAddressMode::MirroredRepeat:
					return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
				case SamplerAddressMode::ClampToEdge:
					return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				case SamplerAddressMode::ClampToBorder:
					return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
				default:
					return VK_SAMPLER_ADDRESS_MODE_MAX_ENUM;
			}
		}

		VkBorderColor ConvertBorderColor(SamplerBorderColor color) {
			switch (color) {
				case SamplerBorderColor::TransparentBlack:
					return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
				case SamplerBorderColor::OpaqueBlack:
					return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
				case SamplerBorderColor::OpaqueWhite:
					return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
				default:
					return VK_BORDER_COLOR_MAX_ENUM;
			}
		}

		bool IsValidFilter(Filter filter) {
			return filter == Filter::Nearest || filter == Filter::Linear;
		}
	}

	VulkanSampler::VulkanSampler(const VulkanDevice* device)
		: m_Device(device) { }

	VulkanSampler::~VulkanSampler() {
		m_Device->GetDescriptorManager()->FreeSamplerDescriptor(m_DescriptorIndex);
	}

	Result VulkanSampler::Init(const SamplerDesc& desc) {
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(m_Device->GetPhysicalDevice(), &properties);

		if (!IsValidFilter(desc.minFilter) || !IsValidFilter(desc.magFilter) || !IsValidFilter(desc.mipFilter) ||
			ConvertAddressMode(desc.addressU) == VK_SAMPLER_ADDRESS_MODE_MAX_ENUM ||
			ConvertAddressMode(desc.addressV) == VK_SAMPLER_ADDRESS_MODE_MAX_ENUM ||
			ConvertAddressMode(desc.addressW) == VK_SAMPLER_ADDRESS_MODE_MAX_ENUM ||
			ConvertBorderColor(desc.borderColor) == VK_BORDER_COLOR_MAX_ENUM || desc.compareOp < CompareOp::Never ||
			desc.compareOp > CompareOp::Always) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateSampler: Provide valid filter, address, comparison and border color values.");

			return Result::InvalidParameter;
		}

		if (!std::isfinite(desc.mipLodBias) || !std::isfinite(desc.minLod) || !std::isfinite(desc.maxLod) ||
			std::abs(desc.mipLodBias) > properties.limits.maxSamplerLodBias || desc.minLod < 0.0f ||
			desc.maxLod < desc.minLod) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateSampler: Provide finite, ordered LOD limits and a mip bias within device limits.");

			return Result::InvalidParameter;
		}

		VkSamplerCreateInfo samplerInfo{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = ConvertAddressMode(desc.addressU),
			.addressModeV = ConvertAddressMode(desc.addressV),
			.addressModeW = ConvertAddressMode(desc.addressW),
			.mipLodBias = desc.mipLodBias,
			.compareEnable = desc.compareEnable,
			.compareOp = Utils::CompareOpToVkCompareOp(desc.compareOp),
			.minLod = desc.minLod,
			.maxLod = desc.maxLod,
			.borderColor = ConvertBorderColor(desc.borderColor),
		};

		if (desc.magFilter == Filter::Linear) {
			samplerInfo.magFilter = VK_FILTER_LINEAR;
		}

		if (desc.minFilter == Filter::Linear) {
			samplerInfo.minFilter = VK_FILTER_LINEAR;
		}

		if (desc.mipFilter == Filter::Linear) {
			samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		}

		m_DescriptorIndex = m_Device->GetDescriptorManager()->AllocateSamplerDescriptor(samplerInfo, desc.debugName);
		if (m_DescriptorIndex == UINT32_MAX) {
			return Result::Failed;
		}

		return Result::Success;
	}
}
