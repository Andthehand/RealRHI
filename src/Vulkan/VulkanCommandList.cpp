#include "VulkanCommandList.h"

#include "VulkanTextureView.h"
#include "VulkanPipeline.h"
#include "VulkanBuffer.h"
#include "VulkanTexture.h"

#include "VulkanConvertions.h"
#include "VulkanBarrierConversions.h"

#include <mutex>
#include <algorithm>
#include <unordered_set>
#include <utility>

namespace RealRHI {

    VulkanCommandList::VulkanCommandList(const VulkanDevice* device, Ref<VulkanCommandAllocator> allocator)
        : m_Device(device), m_Allocator(std::move(allocator)), m_QueueId{ device->GetGraphicsQueueFamily() } {}

    VulkanCommandList::~VulkanCommandList() {
        std::scoped_lock lock(m_StateMutex);

        if (m_CommandBuffer != VK_NULL_HANDLE && m_State != CommandListState::Pending) {
            m_Allocator->Free(m_CommandBuffer);
            m_CommandBuffer = VK_NULL_HANDLE;
        }
    }

    Result VulkanCommandList::Create(const VulkanDevice* device, Ref<VulkanCommandAllocator> allocator, Ref<VulkanCommandList>& out) {
        if (!allocator) {
            return Result::Failed;
        }

        Ref<VulkanCommandList> list = Ref<VulkanCommandList>::Create(device, std::move(allocator));
        if (list->Init() != Result::Success) {
            return Result::Failed;
        }

        out = list;
        return Result::Success;
    }

    Result VulkanCommandList::Init() {
        return m_Allocator->Allocate(m_CommandBuffer) == VK_SUCCESS
            ? Result::Success : Result::Failed;
    }

    Result VulkanCommandList::Begin() {
        {
            std::scoped_lock lock(m_StateMutex);
            if (m_State == CommandListState::Recording || m_State == CommandListState::Pending) {
                m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Command buffer cannot begin while already recording or pending.");
                
                return Result::Failed;
            }
        }

        if (GetState() == CommandListState::Executable) {
            const Result resetResult = Reset();
            if (resetResult != Result::Success) {
                return resetResult;
            }
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        {
            std::scoped_lock lock(m_StateMutex);
            if (vkBeginCommandBuffer(m_CommandBuffer, &beginInfo) != VK_SUCCESS) {
                m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to begin Vulkan command buffer.");
                
                return Result::Failed;
            }
            m_State = CommandListState::Recording;
            m_ComputePipelineBound = false;
        }

        return Result::Success;
    }

    Result VulkanCommandList::End() {
        {
            std::scoped_lock lock(m_StateMutex);
            if (m_State != CommandListState::Recording) {
                m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Command buffer can only end while recording.");
                
                return Result::Failed;
            }
        }

        {
            std::scoped_lock lock(m_StateMutex);
            if (vkEndCommandBuffer(m_CommandBuffer) != VK_SUCCESS) {
                m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to end Vulkan command buffer.");
                
                return Result::Failed;
            }
            m_State = CommandListState::Executable;
        }

        return Result::Success;
    }

	void VulkanCommandList::RetainResource(RefCounted* resource) {
		if (resource == nullptr) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Attempted to retain a null resource.");
			return;
		}

		// Check if the resource is already retained to avoid duplicates
		for (const auto& retained : m_ReferencedResources) {
			if (retained.Raw() == resource) {
				return;
			}
		}

		m_ReferencedResources.emplace_back(resource);
	}

    Result VulkanCommandList::Reset() {
        CommandListState state = GetState();
        if (state == CommandListState::Pending || state == CommandListState::Recording) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Cannot reset a command buffer while it is pending or recording.");
            return Result::Failed;
        }

        if (state == CommandListState::Initial) {
            return Result::Success;
        }

        {
            std::scoped_lock lock(m_StateMutex);
            VkResult resetResult = m_Allocator->ResetBuffer(m_CommandBuffer);
            if (resetResult != VK_SUCCESS) {
                m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to reset Vulkan command buffer.");
                return Result::Failed;
            }

            m_State = CommandListState::Initial;
        }

        // The native command buffer no longer references the old recording.
        // Release outside the state mutex (destructors may invoke other locks).
        std::vector<Ref<RefCounted>> released;
        released.swap(m_ReferencedResources);
        m_ComputePipelineBound = false;
        m_RecordedLayouts.clear();
        m_InitialLayouts.clear();
        return Result::Success;
    }

    void VulkanCommandList::BeginRendering(const RenderingInfo& renderingInfo) {
        std::vector<VkRenderingAttachmentInfo> colorAttachmentInfos(renderingInfo.colorAttachments.size());
        for (uint32_t i = 0; i < renderingInfo.colorAttachments.size(); i++) {
            const auto& attachment = renderingInfo.colorAttachments[i];
            if (attachment.target == nullptr) {
                m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Rendering color attachment is missing a texture view.");
                return;
            }

            auto* textureView = static_cast<VulkanTextureView*>(attachment.target);
            RetainResource(const_cast<VulkanTexture*>(textureView->GetTexture()));

            VkClearValue clearColor = { {{attachment.clearColor.r, attachment.clearColor.g, attachment.clearColor.b, attachment.clearColor.a}} };
            colorAttachmentInfos[i] = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = textureView->GetImageView(),
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .loadOp = Utils::LoadOpToVkLoadOp(attachment.loadOp),
                .storeOp = Utils::StoreOpToVkStoreOp(attachment.storeOp),
                .clearValue = clearColor
            };
        }

        VkRenderingAttachmentInfo depthAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        };
        const VkRenderingAttachmentInfo* pDepthAttachment = nullptr;
        const VkRenderingAttachmentInfo* pStencilAttachment = nullptr;

        if (renderingInfo.hasDepth) {
            const auto& attachment = renderingInfo.depthAttachment;
            if (attachment.target == nullptr) {
                m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Rendering depth attachment is missing a texture view.");
                return;
            }

            auto* textureView = static_cast<VulkanTextureView*>(attachment.target);
            RetainResource(const_cast<VulkanTexture*>(textureView->GetTexture()));

            const VkImageAspectFlags aspectMask = textureView->GetImageViewCreateInfo().subresourceRange.aspectMask;
            const bool hasStencilAspect = (aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
            const bool readOnly = attachment.readOnlyDepth && (!hasStencilAspect || attachment.readOnlyStencil);
            VkClearValue clearValue{};
            clearValue.depthStencil = {
                .depth = attachment.clear.depth,
                .stencil = attachment.clear.stencil,
            };

            depthAttachmentInfo = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = textureView->GetImageView(),
                .imageLayout = readOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .loadOp = Utils::LoadOpToVkLoadOp(attachment.depthLoadOp),
                .storeOp = Utils::StoreOpToVkStoreOp(attachment.depthStoreOp),
                .clearValue = clearValue,
            };

            if ((aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
                pDepthAttachment = &depthAttachmentInfo;
            }
            if (hasStencilAspect) {
                pStencilAttachment = &depthAttachmentInfo;
            }
        }

        auto& renderAreaRect = renderingInfo.renderArea;
        VkRect2D renderArea{
            .offset = {
                .x = renderAreaRect.x,
                .y = renderAreaRect.y,
            },
            .extent = {
                .width = renderAreaRect.width,
                .height = renderAreaRect.height,
            }
        };

        VkRenderingInfo renderPassInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = renderArea,
            .layerCount = 1,
            .colorAttachmentCount = static_cast<uint32_t>(colorAttachmentInfos.size()),
            .pColorAttachments = colorAttachmentInfos.data(),
            .pDepthAttachment = pDepthAttachment,
            .pStencilAttachment = pStencilAttachment,
        };

        vkCmdBeginRendering(m_CommandBuffer, &renderPassInfo);
    }

    void VulkanCommandList::EndRendering() {
        vkCmdEndRendering(m_CommandBuffer);
    }

    void VulkanCommandList::SetViewport(const Viewport& vp) {
        VkViewport viewport{
            .x = vp.x,
            .y = vp.y,
            .width = vp.width,
            .height = vp.height,
            .minDepth = vp.minDepth,
            .maxDepth = vp.maxDepth
        };
        vkCmdSetViewport(m_CommandBuffer, 0, 1, &viewport);
    }

    void VulkanCommandList::SetScissor(const Rect& rect) {
        VkRect2D scissor{
            .offset = {
                .x = rect.x,
                .y = rect.y
            },
            .extent = {
                .width = rect.width,
                .height = rect.height
            }
        };
        vkCmdSetScissor(m_CommandBuffer, 0, 1, &scissor);
    }

	void VulkanCommandList::UseResource(Texture* texture) {
		if (texture == nullptr || GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
				"UseResource(Texture): Requires an active recording and a non-null texture.");
			return;
		}

		auto* resource = dynamic_cast<VulkanTexture*>(texture);
		if (resource == nullptr) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
				"UseResource: Resource must belong to the Vulkan backend.");
			return;
		}

		RetainResource(resource);
	}

	void VulkanCommandList::UseResource(Buffer* buffer) {
		if (buffer == nullptr || GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
				"UseResource(Buffer): Requires an active recording and a non-null buffer.");
			return;
		}

		auto* resource = dynamic_cast<VulkanBuffer*>(buffer);
		if (resource == nullptr) {
			return;
		}

		RetainResource(resource);
	}

    void VulkanCommandList::BindPipeline(Pipeline* pipeline) {
        auto* vulkanPipeline = dynamic_cast<VulkanPipeline*>(pipeline);
        if (GetState() != CommandListState::Recording || vulkanPipeline == nullptr) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
                "BindPipeline: Begin command recording and provide a valid Vulkan pipeline before binding.");
            
			return;
        }

        RetainResource(vulkanPipeline);
        vkCmdBindPipeline(m_CommandBuffer, vulkanPipeline->GetBindPoint(), vulkanPipeline->GetPipeline());
        m_Device->BindDescriptorHeaps(m_CommandBuffer);

		// Since you can bind graphics and compute pipelines
		// we need to track if a compute pipeline is bound for DispatchCompute validation.
		if (vulkanPipeline->GetBindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE) {
			m_ComputePipelineBound = true;
		}

    }

	void VulkanCommandList::DispatchCompute(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
		if (GetState() != CommandListState::Recording || !m_ComputePipelineBound ||
			groupCountX == 0 || groupCountY == 0 || groupCountZ == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
				"DispatchCompute: A compute pipeline must be bound, the command list must be recording, and all three dispatch group counts must be nonzero.");
			return;
		}

		vkCmdDispatch(m_CommandBuffer, groupCountX, groupCountY, groupCountZ);
	}

	void VulkanCommandList::SetPushConstants(const void* data, uint32_t size, uint32_t offset) {
		const uint32_t maxSize = m_Device->GetMaxPushDataSize();
		if (GetState() != CommandListState::Recording || data == nullptr ||
			size == 0 || (size % 4) != 0 || (offset % 4) != 0 ||
			offset > maxSize || size > maxSize - offset) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
				"SetPushConstants: Requires an active recording, non-null data, nonzero 4-byte-aligned size and offset, and a range within maxPushDataSize.");
			return;
		}

		const VkPushDataInfoEXT pushDataInfo{
			.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
			.offset = offset,
			.data = {
				.address = data,
				.size = size,
			},
		};
		vkCmdPushDataEXT(m_CommandBuffer, &pushDataInfo);
	}

    void VulkanCommandList::BindVertexBuffer(Buffer* vertexBuffer) {
        RetainResource(vertexBuffer);
        VkBuffer vertexBuffers[] = { static_cast<VulkanBuffer*>(vertexBuffer)->GetBuffer() };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(m_CommandBuffer, 0, 1, vertexBuffers, offsets);
    }

    void VulkanCommandList::BindIndexBuffer(Buffer* indexBuffer) {
        RetainResource(indexBuffer);
        vkCmdBindIndexBuffer(m_CommandBuffer, static_cast<VulkanBuffer*>(indexBuffer)->GetBuffer(), 0, VK_INDEX_TYPE_UINT32);
    }

	void VulkanCommandList::CopyBuffer(Buffer* source, Buffer* destination, const BufferCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.size == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "CopyBuffer failed: Command list is not recording or region size is zero.");
			
            return;
		}

		auto* sourceBuffer = dynamic_cast<VulkanBuffer*>(source);
		auto* destinationBuffer = dynamic_cast<VulkanBuffer*>(destination);
		if (sourceBuffer == nullptr || destinationBuffer == nullptr ) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "CopyBuffer failed: Source or destination buffer is not a VulkanBuffer.");

			return;
		}

		if (region.sourceOffset > sourceBuffer->GetSize() ||
			region.size > sourceBuffer->GetSize() - region.sourceOffset ||
			region.destinationOffset > destinationBuffer->GetSize() ||
			region.size > destinationBuffer->GetSize() - region.destinationOffset) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "CopyBuffer failed: Copy region is out of bounds of source or destination buffer.");

			return;
		}

		RetainResource(sourceBuffer);
		RetainResource(destinationBuffer);

		VkBufferCopy copyRegion{
			.srcOffset = region.sourceOffset,
			.dstOffset = region.destinationOffset,
			.size = region.size,
		};
		vkCmdCopyBuffer(m_CommandBuffer, sourceBuffer->GetBuffer(), destinationBuffer->GetBuffer(), 1, &copyRegion);
	}

	TextureLayout VulkanCommandList::GetExpectedLayout(const VulkanTexture* texture, uint32_t mip, uint32_t layer) const {
		const auto it = m_RecordedLayouts.find({ texture, mip, layer });
		return it != m_RecordedLayouts.end() ? it->second : texture->GetSubmittedLayout(mip, layer);
	}

	bool VulkanCommandList::ValidateSubmissionLayouts(LayoutMap& predicted) const {
		for (const auto& [key, layout] : m_InitialLayouts) {
			const auto it = predicted.find(key);
			const TextureLayout current = it != predicted.end() ? it->second :
				key.texture->GetSubmittedLayout(key.mipLevel, key.arrayLayer);
			if (layout != TextureLayout::Undefined && current != layout) {
				return false;
			}
		}
		for (const auto& [key, layout] : m_RecordedLayouts) {
			predicted[key] = layout;
		}
		return true;
	}

	void VulkanCommandList::CommitSubmissionLayouts() const {
		for (const auto& [key, layout] : m_RecordedLayouts) {
			const_cast<VulkanTexture*>(key.texture)->SetSubmittedLayout(key.mipLevel, key.arrayLayer, layout);
		}
	}

	void VulkanCommandList::RegisterRequiredLayout(const VulkanTexture* texture, uint32_t mipLevel,
		uint32_t baseLayer, uint32_t layerCount, TextureLayout expected) {
		for (uint32_t layer = baseLayer; layer < baseLayer + layerCount; ++layer) {
			const SubresourceKey key{ texture, mipLevel, layer };
			if (!m_RecordedLayouts.contains(key)) {
				m_InitialLayouts.emplace(key, expected);
			}
		}
	}

	bool VulkanCommandList::HasRecordedLayout(const VulkanTexture* texture, uint32_t mipLevel,
		uint32_t baseLayer, uint32_t layerCount, TextureLayout expected) const {
		for (uint32_t layer = baseLayer; layer < baseLayer + layerCount; ++layer) {
			if (expected != TextureLayout::Undefined && GetExpectedLayout(texture, mipLevel, layer) != expected) {
				return false;
			}
		}
		return true;
	}

	void VulkanCommandList::CopyBufferToTexture(Buffer* source, Texture* destination, const BufferTextureCopyRegion& region) {
		if (GetState() != CommandListState::Recording ||
			region.width == 0 || region.height == 0 || region.depth == 0 || region.layerCount == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "CopyBufferToTexture failed: Command list is not recording or region dimensions are zero.");
            
            return;
		}

		auto* sourceBuffer = dynamic_cast<VulkanBuffer*>(source);
		auto* destinationTexture = dynamic_cast<VulkanTexture*>(destination);
		if (sourceBuffer == nullptr || destinationTexture == nullptr ||
			!HasRecordedLayout(destinationTexture, region.mipLevel, region.baseArrayLayer,
                region.layerCount, TextureLayout::TransferDst) ||
			region.mipLevel >= destinationTexture->m_MipLevels ||
			region.baseArrayLayer >= destinationTexture->m_ArrayLayers ||
			region.layerCount > destinationTexture->m_ArrayLayers - region.baseArrayLayer) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "CopyBufferToTexture failed: Invalid source buffer or destination texture, or invalid copy region parameters.");
			
            return;
		}

		const VkExtent3D mipExtent{
			.width  = std::max(1u, destinationTexture->m_ImageExtent.width >> region.mipLevel),
			.height = std::max(1u, destinationTexture->m_ImageExtent.height >> region.mipLevel),
			.depth  = std::max(1u, destinationTexture->m_ImageExtent.depth >> region.mipLevel),
		};
		if (region.width > mipExtent.width || region.height > mipExtent.height || region.depth > mipExtent.depth ||
			region.bufferOffset >= sourceBuffer->GetSize()) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "CopyBufferToTexture failed: Copy region dimensions exceed mip level extent or buffer offset is out of bounds.");

			return;
		}

		RegisterRequiredLayout(destinationTexture, region.mipLevel, region.baseArrayLayer,
			region.layerCount, TextureLayout::TransferDst);
		RetainResource(sourceBuffer);
		RetainResource(destinationTexture);
		VkBufferImageCopy copyRegion{
			.bufferOffset = region.bufferOffset,
			.imageSubresource = {
				.aspectMask = Utils::TextureFormatToVkImageAspect(Utils::VkFormatToTextureFormat(destinationTexture->m_Format)),
				.mipLevel = region.mipLevel,
				.baseArrayLayer = region.baseArrayLayer,
				.layerCount = region.layerCount,
			},
			.imageExtent = { region.width, region.height, region.depth },
		};
		vkCmdCopyBufferToImage(m_CommandBuffer, sourceBuffer->GetBuffer(), destinationTexture->m_Image,
			                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
	}

	void VulkanCommandList::CopyTextureToBuffer(Texture* source, Buffer* destination, const BufferTextureCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.width == 0 || region.height == 0 ||
			region.depth == 0 || region.layerCount == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Texture copy failed: Command list is not recording or region dimensions are zero.");
			return;
		}

		auto* texture = dynamic_cast<VulkanTexture*>(source);
		auto* buffer = dynamic_cast<VulkanBuffer*>(destination);
		if (texture == nullptr || buffer == nullptr ||

			region.mipLevel >= texture->m_MipLevels ||
			region.baseArrayLayer >= texture->m_ArrayLayers ||
			region.layerCount > texture->m_ArrayLayers - region.baseArrayLayer ||
			region.bufferOffset >= buffer->GetSize() ||
            !HasRecordedLayout(texture, region.mipLevel, region.baseArrayLayer,
                region.layerCount, TextureLayout::TransferSrc)) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "CopyTextureToBuffer failed: Invalid source texture, destination buffer, or subresource range.");
			return;
		}

		const VkExtent3D mipExtent{
			.width = std::max(1u, texture->m_ImageExtent.width >> region.mipLevel),
			.height = std::max(1u, texture->m_ImageExtent.height >> region.mipLevel),
			.depth = std::max(1u, texture->m_ImageExtent.depth >> region.mipLevel),
		};
		if (region.width > mipExtent.width || region.height > mipExtent.height ||
			region.depth > mipExtent.depth) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "CopyTextureToBuffer failed: Copy dimensions exceed source mip extent.");
			return;
		}

		// Initial readback supports tightly packed RGBA8 data. Other formats
		// need format-aware block size and row pitch handling.
		if (texture->m_Format != VK_FORMAT_R8G8B8A8_UNORM ||
			region.width > UINT64_MAX / region.height / region.depth / region.layerCount / 4 ||
			static_cast<uint64_t>(region.width) * region.height * region.depth * region.layerCount * 4 >
				buffer->GetSize() - region.bufferOffset) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "CopyTextureToBuffer failed: Unsupported texture format or insufficient readback buffer capacity.");
			return;
		}

		RegisterRequiredLayout(texture, region.mipLevel, region.baseArrayLayer,
			region.layerCount, TextureLayout::TransferSrc);
		RetainResource(texture);
		RetainResource(buffer);
		VkBufferImageCopy copyRegion{
			.bufferOffset = region.bufferOffset,
			.imageSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = region.mipLevel,
				.baseArrayLayer = region.baseArrayLayer,
				.layerCount = region.layerCount,
			},
			.imageExtent = { region.width, region.height, region.depth },
		};
		vkCmdCopyImageToBuffer(m_CommandBuffer, texture->m_Image,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->GetBuffer(), 1, &copyRegion);
	}

	void VulkanCommandList::CopyTexture(Texture* source, Texture* destination, const TextureCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.width == 0 || region.height == 0 ||
			region.depth == 0 || region.layerCount == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Texture copy failed: Command list is not recording or region dimensions are zero.");
			return;
		}

		auto* sourceTexture = dynamic_cast<VulkanTexture*>(source);
		auto* destinationTexture = dynamic_cast<VulkanTexture*>(destination);
		if (sourceTexture == nullptr || destinationTexture == nullptr ||

			sourceTexture == destinationTexture ||
			sourceTexture->m_Format != destinationTexture->m_Format ||
			sourceTexture->m_Format != VK_FORMAT_R8G8B8A8_UNORM ||
			region.sourceMipLevel >= sourceTexture->m_MipLevels ||
			region.destinationMipLevel >= destinationTexture->m_MipLevels ||
			region.sourceBaseArrayLayer >= sourceTexture->m_ArrayLayers ||
			region.destinationBaseArrayLayer >= destinationTexture->m_ArrayLayers ||
			region.layerCount > sourceTexture->m_ArrayLayers - region.sourceBaseArrayLayer ||
			region.layerCount > destinationTexture->m_ArrayLayers - region.destinationBaseArrayLayer ||
            !HasRecordedLayout(sourceTexture, region.sourceMipLevel, region.sourceBaseArrayLayer,
                region.layerCount, TextureLayout::TransferSrc) ||
            !HasRecordedLayout(destinationTexture, region.destinationMipLevel, region.destinationBaseArrayLayer,
                region.layerCount, TextureLayout::TransferDst)) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "CopyTexture failed: Invalid textures, formats, or subresource range.");
			return;
		}

		const auto fits = [&](const VulkanTexture* texture, uint32_t mipLevel) {
			return region.width <= std::max(1u, texture->m_ImageExtent.width >> mipLevel) &&
				region.height <= std::max(1u, texture->m_ImageExtent.height >> mipLevel) &&
				region.depth <= std::max(1u, texture->m_ImageExtent.depth >> mipLevel);
		};
		if (!fits(sourceTexture, region.sourceMipLevel) ||
			!fits(destinationTexture, region.destinationMipLevel)) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "CopyTexture failed: Copy dimensions exceed source or destination mip extent.");
			return;
		}

		RegisterRequiredLayout(sourceTexture, region.sourceMipLevel, region.sourceBaseArrayLayer,
			region.layerCount, TextureLayout::TransferSrc);
		RegisterRequiredLayout(destinationTexture, region.destinationMipLevel, region.destinationBaseArrayLayer,
			region.layerCount, TextureLayout::TransferDst);
		RetainResource(sourceTexture);
		RetainResource(destinationTexture);
		VkImageCopy copyRegion{
			.srcSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = region.sourceMipLevel,
				.baseArrayLayer = region.sourceBaseArrayLayer,
				.layerCount = region.layerCount,
			},
			.dstSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = region.destinationMipLevel,
				.baseArrayLayer = region.destinationBaseArrayLayer,
				.layerCount = region.layerCount,
			},
			.extent = { region.width, region.height, region.depth },
		};
		vkCmdCopyImage(m_CommandBuffer, sourceTexture->m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			destinationTexture->m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
	}

	void VulkanCommandList::Barrier(const BarrierDesc& barriers) {
		if (GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
				"Barrier: Call Begin() before recording resource barriers.");
			return;
		}

		std::vector<VkMemoryBarrier2> memoryBarriers;
		std::vector<VkBufferMemoryBarrier2> bufferBarriers;
		std::vector<VkImageMemoryBarrier2> imageBarriers;
		memoryBarriers.reserve(barriers.memory.size());
		bufferBarriers.reserve(barriers.buffers.size());
		imageBarriers.reserve(barriers.textures.size());

		for (const MemoryBarrier& barrier : barriers.memory) {
			memoryBarriers.push_back({
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				.srcStageMask = Utils::ToVkPipelineStages(barrier.before.stages),
				.srcAccessMask = Utils::ToVkResourceAccess(barrier.before.access),
				.dstStageMask = Utils::ToVkPipelineStages(barrier.after.stages),
				.dstAccessMask = Utils::ToVkResourceAccess(barrier.after.access),
			});
		}

		for (const BufferBarrier& barrier : barriers.buffers) {
			auto* buffer = dynamic_cast<VulkanBuffer*>(barrier.buffer);
			if (buffer == nullptr || 
				barrier.size == 0 || barrier.offset >= buffer->GetSize() ||
				barrier.size > buffer->GetSize() - barrier.offset) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
					"Barrier: Buffer range is invalid. Provide a non-null buffer, nonzero size, and in-bounds offset and size.");
				return;
			}

			bufferBarriers.push_back({
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
				.srcStageMask = Utils::ToVkPipelineStages(barrier.before.stages),
				.srcAccessMask = Utils::ToVkResourceAccess(barrier.before.access),
				.dstStageMask = Utils::ToVkPipelineStages(barrier.after.stages),
				.dstAccessMask = Utils::ToVkResourceAccess(barrier.after.access),
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = buffer->GetBuffer(),
				.offset = barrier.offset,
				.size = barrier.size,
			});
		}

		for (const TextureBarrier& barrier : barriers.textures) {
			auto* texture = dynamic_cast<VulkanTexture*>(barrier.texture);
			if (texture == nullptr || 
				barrier.subresources.mipLevelCount == 0 || barrier.subresources.arrayLayerCount == 0 ||
				barrier.subresources.baseMipLevel >= texture->m_MipLevels ||
				barrier.subresources.mipLevelCount > texture->m_MipLevels - barrier.subresources.baseMipLevel ||
				barrier.subresources.baseArrayLayer >= texture->m_ArrayLayers ||
				barrier.subresources.arrayLayerCount > texture->m_ArrayLayers - barrier.subresources.baseArrayLayer) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
					"Barrier: Texture subresource range is invalid. Check mip and array-layer bounds.");
				return;
			}

            for (uint32_t mip = barrier.subresources.baseMipLevel;
                mip < barrier.subresources.baseMipLevel + barrier.subresources.mipLevelCount; ++mip) {
                if (!HasRecordedLayout(texture, mip, barrier.subresources.baseArrayLayer,
                    barrier.subresources.arrayLayerCount, barrier.oldLayout)) {
                    m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
                        "Texture barrier old layout disagrees with an earlier transition in this command list.");
                    return;
                }
            }

			imageBarriers.push_back({
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = Utils::ToVkPipelineStages(barrier.before.stages),
				.srcAccessMask = Utils::ToVkResourceAccess(barrier.before.access),
				.dstStageMask = Utils::ToVkPipelineStages(barrier.after.stages),
				.dstAccessMask = Utils::ToVkResourceAccess(barrier.after.access),
				.oldLayout = Utils::TextureLayoutToVkImageLayout(barrier.oldLayout),
				.newLayout = Utils::TextureLayoutToVkImageLayout(barrier.newLayout),
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = texture->m_Image,
				.subresourceRange = {
					.aspectMask = Utils::TextureFormatToVkImageAspect(Utils::VkFormatToTextureFormat(texture->m_Format)),
					.baseMipLevel = barrier.subresources.baseMipLevel,
					.levelCount = barrier.subresources.mipLevelCount,
					.baseArrayLayer = barrier.subresources.baseArrayLayer,
					.layerCount = barrier.subresources.arrayLayerCount,
				},
			});
		}

        // Reject overlapping ranges: barriers within one dependency are not sequential.
        std::unordered_set<SubresourceKey, SubresourceKeyHash> touched;
        for (const TextureBarrier& barrier : barriers.textures) {
            auto* texture = static_cast<VulkanTexture*>(barrier.texture);
            for (uint32_t mip = barrier.subresources.baseMipLevel;
                mip < barrier.subresources.baseMipLevel + barrier.subresources.mipLevelCount; ++mip) {
                for (uint32_t layer = barrier.subresources.baseArrayLayer;
                    layer < barrier.subresources.baseArrayLayer + barrier.subresources.arrayLayerCount; ++layer) {
                    if (!touched.insert({ texture, mip, layer }).second) {
                        m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
                            "Overlapping texture barriers must be recorded in separate calls.");
                        return;
                    }
                }
            }
        }

        // Update the command list's layout knowledge only after all barriers validate.
        for (const TextureBarrier& barrier : barriers.textures) {
            auto* texture = static_cast<VulkanTexture*>(barrier.texture);
            for (uint32_t mip = barrier.subresources.baseMipLevel;
                mip < barrier.subresources.baseMipLevel + barrier.subresources.mipLevelCount; ++mip) {
                for (uint32_t layer = barrier.subresources.baseArrayLayer;
                    layer < barrier.subresources.baseArrayLayer + barrier.subresources.arrayLayerCount; ++layer) {
                    const SubresourceKey key{ texture, mip, layer };
                    if (!m_RecordedLayouts.contains(key)) {
                        m_InitialLayouts.emplace(key, barrier.oldLayout);
                    }
                    m_RecordedLayouts[key] = barrier.newLayout;
                }
            }
        }

		for (const BufferBarrier& barrier : barriers.buffers) {
			RetainResource(barrier.buffer);
		}
		for (const TextureBarrier& barrier : barriers.textures) {
			RetainResource(barrier.texture);
		}

		VkDependencyInfo dependencyInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = static_cast<uint32_t>(memoryBarriers.size()),
			.pMemoryBarriers = memoryBarriers.data(),
			.bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
			.pBufferMemoryBarriers = bufferBarriers.data(),
			.imageMemoryBarrierCount = static_cast<uint32_t>(imageBarriers.size()),
			.pImageMemoryBarriers = imageBarriers.data(),
		};
		vkCmdPipelineBarrier2(m_CommandBuffer, &dependencyInfo);
	}

    void VulkanCommandList::Draw(uint32_t vertexCount) {
        vkCmdDraw(m_CommandBuffer, vertexCount, 1, 0, 0);
    }

    void VulkanCommandList::DrawIndexed(uint32_t indexCount) {
        vkCmdDrawIndexed(m_CommandBuffer, indexCount, 1, 0, 0, 0);
    }
}
