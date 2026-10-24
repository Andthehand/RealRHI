#include "VulkanTexture.h"

#include "VulkanBuffer.h"
#include "VulkanCommandList.h"
#include "VulkanConvertions.h"
#include "VulkanDescriptorManager.h"

#include <algorithm>
#include <bit>

namespace RealRHI {
	VulkanTexture::VulkanTexture(const VulkanDevice* device)
		: m_Device(device) { }

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
		if (desc.width == 0 || desc.height == 0 || desc.depth == 0 || desc.arrayLayers == 0 || desc.mipLevels == 0 ||
			desc.mipLevels > std::bit_width(std::max({desc.width, desc.height, desc.depth})) ||
			desc.format == TextureFormat::Unknown || desc.usage == TextureUsage::None ||
			(desc.depth > 1 && desc.arrayLayers != 1)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateTexture: Provide nonzero dimensions, a valid mip count, format and "
				"usage; 3D textures require one array layer.");

			return Result::InvalidParameter;
		}

		m_Format = Utils::TextureFormatToVkFormat(desc.format);
		m_Usage = desc.usage;
		m_MipLevels = desc.mipLevels;
		m_ArrayLayers = desc.arrayLayers;
		m_SubresourceStates.resize(m_MipLevels * m_ArrayLayers);
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
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		VmaAllocationCreateInfo allocInfo{
			.usage = VMA_MEMORY_USAGE_AUTO,
		};

		if (vmaCreateImage(m_Device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) !=
			VK_SUCCESS) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan image.");
			return Result::Failed;
		}

		TextureViewType viewType = TextureViewType::View2D;
		if (desc.depth > 1) {
			viewType = TextureViewType::View3D;
		}
		else if (desc.arrayLayers > 1) {
			viewType = TextureViewType::View2DArray;
		}

		const bool createNativeImageView =
			Any(m_Usage & TextureUsage::RenderTarget) || Any(m_Usage & TextureUsage::DepthStencil);
		if (m_TextureView.Init(m_Device,
				this,
				TextureViewDesc{
					.type = viewType,
					.mipLevelCount = m_MipLevels,
					.arrayLayerCount = m_ArrayLayers,
				},
				createNativeImageView) != Result::Success) {
			return Result::Failed;
		}

		if (Any(m_Usage & TextureUsage::ShaderResource)) {
			m_SamplerDescriptorIndex = m_Device->GetDescriptorManager()->AllocateSamplerDescriptor(GetSampler());
			if (m_SamplerDescriptorIndex == UINT32_MAX) {
				return Result::Failed;
			}

			VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			if (Any(m_Usage & TextureUsage::Storage)) {
				imageLayout = VK_IMAGE_LAYOUT_GENERAL;
			}
			m_SampledImageDescriptorIndex = m_Device->GetDescriptorManager()->AllocateImageDescriptor(
				&m_TextureView, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, imageLayout);
			if (m_SampledImageDescriptorIndex == UINT32_MAX) {
				return Result::Failed;
			}
		}

		if (Any(m_Usage & TextureUsage::Storage)) {
			m_StorageImageDescriptorIndex = m_Device->GetDescriptorManager()->AllocateImageDescriptor(
				&m_TextureView, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_LAYOUT_GENERAL);
			if (m_StorageImageDescriptorIndex == UINT32_MAX) {
				return Result::Failed;
			}
		}

		return Result::Success;
	}

	Result VulkanTexture::ChangeLayout(CommandList* cmd, TextureLayout newLayout) {
		auto* commandList = static_cast<VulkanCommandList*>(cmd);
		if (commandList == nullptr || commandList->GetState() != CommandListState::Recording) {
			return Result::Failed;
		}

		// A full-texture transition can only use one old layout. Split
		// mixed-layout textures into individual subresource transitions.
		for (uint32_t mip = 0; mip < m_MipLevels; ++mip) {
			for (uint32_t layer = 0; layer < m_ArrayLayers; ++layer) {
				const TextureLayout oldLayout = commandList->GetExpectedLayout(this, mip, layer);
				ResourceState before{};
				if (oldLayout != TextureLayout::Undefined) {
					before.stages = PipelineStage::AllCommands;
					before.access = ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite;
				}

				BarrierDesc barriers;
				barriers.textures.push_back({
					.texture = this,
					.before = before,
					.after =
						{
							.stages = PipelineStage::AllCommands,
							.access = ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite,
						},
					.oldLayout = oldLayout,
					.newLayout = newLayout,
					.subresources = {mip, 1, layer, 1},
				});
				commandList->Barrier(barriers);
			}
		}
		return Result::Success;
	}

	Result VulkanTexture::UploadData(const void* data, uint32_t size) {
		const uint32_t texelSize = Utils::TextureFormatByteSize(Utils::VkFormatToTextureFormat(m_Format));
		if (data == nullptr || texelSize == 0 || m_MipLevels != 1 || m_IsExternal ||
			!Any(m_Usage & TextureUsage::TransferDst) ||
			m_ImageExtent.width > UINT64_MAX / m_ImageExtent.height / m_ImageExtent.depth / m_ArrayLayers / texelSize ||
			static_cast<uint64_t>(m_ImageExtent.width) * m_ImageExtent.height * m_ImageExtent.depth * m_ArrayLayers *
					texelSize >
				size) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadData: Provide sufficient tightly packed color data for a single-mip, "
				"non-swapchain texture with TransferDst usage.");

			return Result::InvalidParameter;
		}

		Ref<VulkanCommandAllocator> commandAllocator;
		Result res = VulkanCommandAllocator::Create(m_Device, m_Device->GetGraphicsQueueFamily(), commandAllocator);
		if (res != Result::Success) {
			return res;
		}

		Ref<VulkanCommandList> commandList;
		res = VulkanCommandList::Create(m_Device, commandAllocator, commandList);
		if (res != Result::Success) {
			return res;
		}

		res = commandList->Begin();
		if (res != Result::Success) {
			return res;
		}

		if (ChangeLayout(commandList.Raw(), TextureLayout::TransferDst) != Result::Success) {
			return Result::Failed;
		}

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

		BufferTextureCopyRegion copyRegion{
			.width = m_ImageExtent.width,
			.height = m_ImageExtent.height,
			.depth = m_ImageExtent.depth,
			.bufferOffset = 0,
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = m_ArrayLayers,
		};
		commandList->CopyBufferToTexture(transferBuffer.Raw(), this, copyRegion);

		TextureLayout finalLayout = TextureLayout::TransferDst;
		if (Any(m_Usage & TextureUsage::Storage)) {
			finalLayout = TextureLayout::General;
		}
		else if (Any(m_Usage & TextureUsage::ShaderResource)) {
			finalLayout = TextureLayout::ShaderRead;
		}
		else if (Any(m_Usage & TextureUsage::RenderTarget)) {
			finalLayout = TextureLayout::ColorAttachment;
		}
		if (ChangeLayout(commandList.Raw(), finalLayout) != Result::Success) {
			return Result::Failed;
		}

		res = commandList->End();
		if (res != Result::Success) {
			return res;
		}

		return m_Device->ImmediateSubmit(commandList.Raw());
	}

	Result VulkanTexture::CreateFromSwapChain(const VulkanDevice* device,
		VkFormat format,
		VkImage image,
		Ref<VulkanTexture>& outTexture) {
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
		m_SubresourceStates.resize(1);
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
