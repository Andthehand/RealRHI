#include "Device.h"
#include "Vulkan/VulkanDevice.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
	using namespace RealRHI;
	uint32_t s_Errors = 0;
	bool s_ExpectTextureError = false;

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void Capture(const DebugMessage& message) {
		DefaultDebugCallback(message);
		if (message.severity == DebugSeverity::Error) {
			Expect(s_ExpectTextureError && message.type == DebugMessageType::General &&
					std::string_view(message.message).starts_with("CreateTexture:"),
				"No unexpected RHI or Vulkan validation errors");
			++s_Errors;
		}
	}

	void TestSnapshot(Device& device) {
		const auto& capabilities = device.GetCapabilities();
		const auto& limits = capabilities.limits;
		Expect(!capabilities.name.empty(), "Identify the selected device");
		Expect(&capabilities == &device.GetCapabilities(), "Capabilities are a stable device-owned snapshot");
		Expect(limits.maxTextureDimension2D > 0 && limits.maxTextureDimension3D > 0 &&
				limits.maxTextureArrayLayers > 0 && limits.maxColorAttachments > 0,
			"Expose texture and render target limits");
		Expect(limits.maxVertexInputBindings > 0 && limits.maxVertexInputAttributes > 0 &&
				limits.maxVertexInputBindingStride >= 16 && limits.maxPushDataSize >= 32,
			"Expose usable pipeline limits");
		Expect(limits.maxComputeWorkGroupInvocations > 0 && limits.maxComputeSharedMemorySize > 0,
			"Expose compute thread and memory limits");
		for (uint32_t axis = 0; axis < 3; ++axis) {
			Expect(limits.maxComputeWorkGroupCount[axis] > 0 && limits.maxComputeWorkGroupSize[axis] > 0,
				"Expose dispatch and workgroup limits for each axis");
		}
		Expect(limits.maxUniformBufferRange > 0 && limits.maxStorageBufferRange > 0 &&
				limits.minUniformBufferOffsetAlignment > 0 && limits.minStorageBufferOffsetAlignment > 0 &&
				limits.nonCoherentAtomSize > 0,
			"Expose buffer ranges and alignments");

		auto& native = static_cast<VulkanDevice&>(device);
		const auto heap = native.GetDescriptorHeapProperties();
		Expect(limits.maxPushDataSize == heap.maxPushDataSize,
			"Push-data limit comes from descriptor heaps, not legacy push constants");
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(native.GetPhysicalDevice(), &properties);
		Expect(limits.maxComputeWorkGroupInvocations == properties.limits.maxComputeWorkGroupInvocations &&
				limits.minStorageBufferOffsetAlignment == properties.limits.minStorageBufferOffsetAlignment,
			"Public limits agree with the selected physical device");
		Expect(capabilities.features.debugLabels == native.SupportsDebugLabels(), "Report enabled debug-label support");
	}

	void TestFormats(Device& device) {
		const auto color = device.GetTextureFormatSupport(TextureFormat::RGBA8_UNorm);
		Expect(Any(color.usages & TextureUsage::ShaderResource) && Any(color.usages & TextureUsage::RenderTarget),
			"RGBA8 supports sampling and color rendering");
		Expect(!color.depthComparison, "Color formats cannot be used for depth comparison");
		Expect(color.linearFiltering && color.colorBlending, "Report filtering and blending for RGBA8");
		Expect(!device.GetTextureFormatSupport(TextureFormat::R32_UInt).colorBlending,
			"Integer formats do not advertise blending");
		Expect(device.GetTextureFormatSupport(TextureFormat::Unknown).usages == TextureUsage::None &&
				device.GetTextureFormatSupport(static_cast<TextureFormat>(999)).usages == TextureUsage::None,
			"Unknown and invalid formats return no support without diagnostics");
		Expect(device.SupportsVertexFormat(VertexFormat::Float3) && device.SupportsVertexFormat(VertexFormat::UNorm8x4),
			"Query floating-point and normalized vertex storage");
		Expect(!device.SupportsVertexFormat(static_cast<VertexFormat>(999)), "Invalid vertex formats are unsupported");

		TextureDesc depth{.width = 8, .height = 8, .usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource};
		for (auto format : {TextureFormat::D32_Float, TextureFormat::D16_UNorm, TextureFormat::D24_UNorm_S8_UInt}) {
			depth.format = format;
			if (device.SupportsTexture(depth)) {
				const auto support = device.GetTextureFormatSupport(format);
				Expect(Any(support.usages & TextureUsage::DepthStencil) &&
						!Any(support.usages & TextureUsage::RenderTarget),
					"Depth-format capabilities distinguish depth from color attachments");
				Ref<Texture> texture;
				Expect(device.CreateTexture(depth, texture) == Result::Success,
					"Create a sampled depth texture selected through capability queries");
				return;
			}
		}
		Expect(false, "Find a sampled depth format");
	}

	void TestTextureQueries(Device& device) {
		const TextureDesc valid{
			.width = 8,
			.height = 8,
			.mipLevels = 4,
			.format = TextureFormat::RGBA8_UNorm,
			.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget | TextureUsage::TransferSrc
		};
		Expect(
			device.GetCapabilities().limits.colorAttachmentSampleCounts & 1, "Single-sample attachments are supported");
		for (uint32_t count : {0u, 3u, 65u}) {
			auto invalid = valid;
			invalid.mipLevels = 1;
			invalid.sampleCount = count;
			Expect(!device.SupportsTexture(invalid), "Reject invalid sample counts without native calls");
		}
		for (uint32_t count : {2u, 4u, 8u, 16u, 32u, 64u}) {
			auto multisampled = valid;
			multisampled.usage = TextureUsage::RenderTarget;
			multisampled.mipLevels = 1;
			multisampled.sampleCount = count;
			Expect(device.SupportsTexture(multisampled) ==
					((device.GetTextureFormatSupport(valid.format).colorAttachmentSampleCounts & count) != 0),
				"Format sample-count masks agree with exact texture queries");
			multisampled.mipLevels = 2;
			Expect(!device.SupportsTexture(multisampled), "Multisampled textures cannot have mip chains");
			multisampled.mipLevels = 1;
			multisampled.usage |= TextureUsage::Storage;
			Expect(!device.SupportsTexture(multisampled), "Multisampled storage images require an unenabled feature");
		}
		const auto before = device.GetStatistics();
		for (uint32_t i = 0; i < 8; ++i) {
			Expect(device.SupportsTexture(valid), "Check combined color usages and mip levels without allocation");
		}
		const auto after = device.GetStatistics();
		Expect(before.textures.peakCount == after.textures.peakCount &&
				before.textures.liveCount == after.textures.liveCount &&
				before.buffers.peakCount == after.buffers.peakCount &&
				before.buffers.liveCount == after.buffers.liveCount,
			"Capability queries do not create GPU resources");

		Ref<Texture> texture;
		Expect(device.CreateTexture(valid, texture) == Result::Success, "Create the validated color texture");
		auto array = valid;
		array.arrayLayers = 3;
		Expect(device.SupportsTexture(array) && device.CreateTexture(array, texture) == Result::Success,
			"Combined query supports mipmapped 2D arrays");
		auto volume = valid;
		volume.depth = 4;
		volume.usage = TextureUsage::ShaderResource | TextureUsage::Storage;
		Expect(device.SupportsTexture(volume) && device.CreateTexture(volume, texture) == Result::Success,
			"Combined query supports sampled storage volumes");

		const auto& limits = device.GetCapabilities().limits;
		auto invalid = valid;
		invalid.width = 0;
		Expect(!device.SupportsTexture(invalid), "Reject zero dimensions quietly");
		invalid = valid;
		invalid.width = limits.maxTextureDimension2D + 1;
		Expect(!device.SupportsTexture(invalid), "Reject dimensions above the device limit");
		invalid = valid;
		invalid.arrayLayers = limits.maxTextureArrayLayers + 1;
		Expect(!device.SupportsTexture(invalid), "Reject layers above the device limit");
		invalid = valid;
		invalid.mipLevels = 5;
		Expect(!device.SupportsTexture(invalid), "Reject more mips than the dimensions permit");
		invalid = valid;
		invalid.format = TextureFormat::Unknown;
		Expect(!device.SupportsTexture(invalid), "Unknown formats are unsupported");
		invalid = valid;
		invalid.usage = static_cast<TextureUsage>(1u << 31);
		Expect(!device.SupportsTexture(invalid), "Unknown usage flags are unsupported");
		invalid.usage = TextureUsage::None;
		Expect(!device.SupportsTexture(invalid), "Empty usage is unsupported");
		invalid = valid;
		invalid.usage = TextureUsage::DepthStencil;
		Expect(!device.SupportsTexture(invalid), "Color formats cannot be depth attachments");
		invalid.format = TextureFormat::D32_Float;
		invalid.usage = TextureUsage::RenderTarget;
		Expect(!device.SupportsTexture(invalid), "Depth formats cannot be color attachments");
		invalid = volume;
		invalid.arrayLayers = 2;
		Expect(!device.SupportsTexture(invalid), "Volumes cannot have array layers");
		invalid = volume;
		invalid.usage = TextureUsage::RenderTarget;
		Expect(!device.SupportsTexture(invalid), "Do not advertise 3D attachment views unsupported by this RHI");
		Expect(s_Errors == 0, "Unsupported capability queries are quiet");

		// Creation uses the same capability checks before allocation or invalid Vulkan commands.
		invalid = valid;
		invalid.width = limits.maxTextureDimension2D + 1;
		Ref<Texture> rejected;
		s_ExpectTextureError = true;
		Expect(device.CreateTexture(invalid, rejected) == Result::InvalidParameter && !rejected,
			"Creation rejects unsupported texture dimensions consistently");
		s_ExpectTextureError = false;
		Expect(s_Errors == 1, "Unsupported creation reports one actionable diagnostic");
	}
}

int main() {
	std::unique_ptr<RealRHI::Device> device;
	Expect(RealRHI::Device::Create({.applicationName = "CapabilitiesTests",
									   .enableDebug = true,
									   .enableValidationLayers = true,
									   .debugCallback = Capture,
									   .enableProfiling = true},
			   device) == RealRHI::Result::Success,
		"Create device for capability queries");
	TestSnapshot(*device);
	TestFormats(*device);
	TestTextureQueries(*device);
	device->WaitIdle();
	device.reset();
	std::cout << "CapabilitiesTests passed\n";
}
