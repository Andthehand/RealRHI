#include "VulkanTexture.h"

#include "VulkanBuffer.h"
#include "VulkanCommandList.h"
#include "VulkanConversions.h"
#include "VulkanDescriptorManager.h"

#include <algorithm>
#include <bit>

namespace RealRHI {
	VulkanTexture::VulkanTexture(const VulkanDevice* device)
		: m_Device(device) { }

	VkExtent3D VulkanTexture::GetMipExtent(uint32_t mipLevel) const {
		return {
			.width = std::max(1u, m_ImageExtent.width >> mipLevel),
			.height = std::max(1u, m_ImageExtent.height >> mipLevel),
			.depth = std::max(1u, m_ImageExtent.depth >> mipLevel),
		};
	}

	VulkanTexture::~VulkanTexture() {
		// Views must be destroyed before the image they reference.
		m_TextureView.Release();
		m_DefaultSampler.Release();

		if (!m_IsExternal) {
			vmaDestroyImage(m_Device->GetAllocator(), m_Image, m_Allocation);
			if (m_AllocationBytes != 0) {
				m_Device->RecordResourceDestroyed(VulkanProfiledResource::Texture, m_AllocationBytes);
			}
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

		if (!m_Device->SupportsTexture(desc)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateTexture: The format, combined usages, dimensions, mip count, layer count, or sample count is "
				"unsupported by "
				"this device.");
			return Result::InvalidParameter;
		}

		m_SampleCount = desc.sampleCount;
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
			.samples = static_cast<VkSampleCountFlagBits>(desc.sampleCount),
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

		VmaAllocationInfo allocationInfo{};
		vmaGetAllocationInfo(m_Device->GetAllocator(), m_Allocation, &allocationInfo);
		m_AllocationBytes = allocationInfo.size;
		m_Device->RecordResourceCreated(VulkanProfiledResource::Texture, m_AllocationBytes);
		const Result nameResult = m_Device->SetDebugName(VK_OBJECT_TYPE_IMAGE, m_Image, desc.debugName);
		if (nameResult != Result::Success) {
			return nameResult;
		}

		TextureViewType viewType = TextureViewType::View2D;
		if (desc.depth > 1) {
			viewType = TextureViewType::View3D;
		}
		else if (desc.arrayLayers > 1) {
			viewType = TextureViewType::View2DArray;
		}

		uint32_t viewMipCount = m_MipLevels;
		// Storage and attachment views select one mip, even when the texture is also sampled.
		if (Any(m_Usage & (TextureUsage::Storage | TextureUsage::RenderTarget | TextureUsage::DepthStencil))) {
			viewMipCount = 1;
		}

		m_TextureView = Ref<VulkanTextureView>::Create();
		const Result viewResult = m_TextureView->Init(m_Device,
			this,
			TextureViewDesc{
				.type = viewType,
				.mipLevelCount = viewMipCount,
				.arrayLayerCount = m_ArrayLayers,
				.debugName = desc.debugName,
			});
		if (viewResult != Result::Success) {
			return viewResult;
		}

		if (Any(m_Usage & TextureUsage::ShaderResource) && m_SampleCount == 1) {
			const Result samplerResult = m_Device->CreateSampler({.debugName = desc.debugName}, m_DefaultSampler);
			if (samplerResult != Result::Success) {
				return samplerResult;
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
		if (data == nullptr || texelSize == 0 || m_MipLevels != 1 || m_SampleCount != 1 || m_IsExternal ||
			!Any(m_Usage & TextureUsage::TransferDst) ||
			m_ImageExtent.width > UINT64_MAX / m_ImageExtent.height / m_ImageExtent.depth / m_ArrayLayers / texelSize ||
			static_cast<uint64_t>(m_ImageExtent.width) * m_ImageExtent.height * m_ImageExtent.depth * m_ArrayLayers *
					texelSize >
				size) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UploadData: Provide sufficient tightly packed color data for a single-mip, "
				"single-sampled, non-swapchain texture with TransferDst usage.");

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
		VkExtent2D extent,
		Ref<VulkanTexture>& outTexture) {
		Ref<VulkanTexture> texture = Ref<VulkanTexture>::Create(device);
		Result res = texture->InitSwapChainTexture(format, image, extent);
		if (res != Result::Success) {
			return res;
		}

		outTexture = texture;
		return Result::Success;
	}

	Result VulkanTexture::InitSwapChainTexture(VkFormat format, VkImage image, VkExtent2D extent) {
		m_IsExternal = true;
		m_ImageExtent = {extent.width, extent.height, 1};
		m_SubresourceStates.resize(1);
		m_Usage = TextureUsage::RenderTarget;
		m_Format = format;
		m_Image = image;

		m_TextureView = Ref<VulkanTextureView>::Create();
		if (m_TextureView->Init(m_Device, this, TextureViewDesc{}) != Result::Success) {
			return Result::Failed;
		}

		return Result::Success;
	}

}
