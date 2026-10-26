#pragma once
#include "RenderingInfo.h"
#include "TextureDesc.h"

#include <array>
#include <cstdint>
#include <string>

namespace RealRHI {
	// Features enabled on this device and available through the RHI.
	struct DeviceFeatures {
		bool wireframe = false;
		bool multiDrawIndirect = false;
		bool drawIndirectFirstInstance = false;
		bool independentBlend = false;
		bool depthBiasClamp = false;
		bool debugLabels = false;
	};

	struct DeviceLimits {
		uint32_t maxTextureDimension2D = 0;
		uint32_t maxTextureDimension3D = 0;
		uint32_t maxTextureArrayLayers = 0;
		uint32_t maxColorAttachments = 0;
		// Bitmasks whose bits are the supported numeric sample counts (1, 2, 4, ...).
		uint32_t colorAttachmentSampleCounts = 0;
		uint32_t integerColorAttachmentSampleCounts = 0;
		uint32_t depthAttachmentSampleCounts = 0;
		uint32_t stencilAttachmentSampleCounts = 0;
		uint32_t noAttachmentSampleCounts = 0;

		uint32_t maxVertexInputBindings = 0;
		uint32_t maxVertexInputAttributes = 0;
		uint32_t maxVertexInputBindingStride = 0;
		uint32_t maxVertexInputAttributeOffset = 0;
		uint32_t maxPushDataSize = 0;
		uint32_t maxDrawIndirectCount = 0;

		std::array<uint32_t, 3> maxComputeWorkGroupCount{};
		std::array<uint32_t, 3> maxComputeWorkGroupSize{};
		uint32_t maxComputeWorkGroupInvocations = 0;
		uint32_t maxComputeSharedMemorySize = 0;

		uint64_t maxUniformBufferRange = 0;
		uint64_t maxStorageBufferRange = 0;
		uint64_t minUniformBufferOffsetAlignment = 0;
		uint64_t minStorageBufferOffsetAlignment = 0;
		uint64_t nonCoherentAtomSize = 0;
		float maxSamplerLodBias = 0.f;
	};

	struct DepthStencilResolveSupport {
		// Bitwise combinations of ResolveMode values.
		uint32_t depthModes = 0;
		uint32_t stencilModes = 0;
		bool independentResolve = false;
		bool independentResolveNone = false;
	};

	struct DeviceCapabilitiesInfo {
		std::string name;
		uint32_t vendorId = 0;
		uint32_t deviceId = 0;
		DeviceFeatures features;
		DeviceLimits limits;
		DepthStencilResolveSupport depthStencilResolve;
	};

	struct TextureFormatSupport {
		// Optimal-tiled image usages, supported individually. Use SupportsTexture for combinations.
		TextureUsage usages = TextureUsage::None;
		bool linearFiltering = false;
		bool depthComparison = false;
		bool colorBlending = false;
		bool blitSource = false;
		bool blitDestination = false;
		bool storageAtomics = false;
		uint32_t colorAttachmentSampleCounts = 0;
		uint32_t depthStencilAttachmentSampleCounts = 0;
	};
}
