#include "VulkanCommandList.h"

#include "VulkanTextureView.h"
#include "VulkanPipeline.h"
#include "VulkanBuffer.h"
#include "VulkanTexture.h"

#include "VulkanConvertions.h"

#include <mutex>
#include <algorithm>
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
        if (!allocator || allocator->GetDevice() != device) {
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
			return;
		}

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
        m_BoundPipeline = nullptr;
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

	Result VulkanCommandList::UseResource(Texture* texture) {
		if (texture == nullptr || GetState() != CommandListState::Recording) {
			return Result::Failed;
		}

		auto* resource = dynamic_cast<VulkanTexture*>(texture);
		if (resource == nullptr || resource->GetDevice() != m_Device) {
			return Result::Failed;
		}

		RetainResource(resource);
		return Result::Success;
	}

	Result VulkanCommandList::UseResource(Buffer* buffer) {
		if (buffer == nullptr || GetState() != CommandListState::Recording) {
			return Result::Failed;
		}

		auto* resource = dynamic_cast<VulkanBuffer*>(buffer);
		if (resource == nullptr || resource->GetDevice() != m_Device) {
			return Result::Failed;
		}

		RetainResource(resource);
		return Result::Success;
	}

    void VulkanCommandList::BindPipeline(Pipeline* pipeline) {
        RetainResource(pipeline);
        m_BoundPipeline = static_cast<VulkanPipeline*>(pipeline);
        vkCmdBindPipeline(m_CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_BoundPipeline->GetPipeline());

        m_Device->BindDescriptorHeaps(m_CommandBuffer);
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

	Result VulkanCommandList::CopyBuffer(Buffer* source, Buffer* destination, const BufferCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.size == 0) {
			return Result::Failed;
		}

		auto* sourceBuffer = dynamic_cast<VulkanBuffer*>(source);
		auto* destinationBuffer = dynamic_cast<VulkanBuffer*>(destination);
		if (sourceBuffer == nullptr || destinationBuffer == nullptr ||
			sourceBuffer->GetDevice() != m_Device || destinationBuffer->GetDevice() != m_Device) {
			return Result::Failed;
		}

		if (region.sourceOffset > sourceBuffer->GetSize() ||
			region.size > sourceBuffer->GetSize() - region.sourceOffset ||
			region.destinationOffset > destinationBuffer->GetSize() ||
			region.size > destinationBuffer->GetSize() - region.destinationOffset) {
			return Result::Failed;
		}

		RetainResource(sourceBuffer);
		RetainResource(destinationBuffer);

		VkBufferCopy copyRegion{
			.srcOffset = region.sourceOffset,
			.dstOffset = region.destinationOffset,
			.size = region.size,
		};
		vkCmdCopyBuffer(m_CommandBuffer, sourceBuffer->GetBuffer(), destinationBuffer->GetBuffer(), 1, &copyRegion);
		return Result::Success;
	}

	Result VulkanCommandList::CopyBufferToTexture(Buffer* source, Texture* destination, const BufferTextureCopyRegion& region) {
		if (GetState() != CommandListState::Recording ||
			region.width == 0 || region.height == 0 || region.depth == 0 || region.layerCount == 0) {
			return Result::Failed;
		}

		auto* sourceBuffer = dynamic_cast<VulkanBuffer*>(source);
		auto* destinationTexture = dynamic_cast<VulkanTexture*>(destination);
		if (sourceBuffer == nullptr || destinationTexture == nullptr ||
			sourceBuffer->GetDevice() != m_Device || destinationTexture->GetDevice() != m_Device ||
			destinationTexture->m_Layout != TextureLayout::TransferDst ||
			region.mipLevel >= destinationTexture->m_MipLevels ||
			region.baseArrayLayer >= destinationTexture->m_ArrayLayers ||
			region.layerCount > destinationTexture->m_ArrayLayers - region.baseArrayLayer) {
			return Result::Failed;
		}

		const VkExtent3D mipExtent{
			.width = std::max(1u, destinationTexture->m_ImageExtent.width >> region.mipLevel),
			.height = std::max(1u, destinationTexture->m_ImageExtent.height >> region.mipLevel),
			.depth = std::max(1u, destinationTexture->m_ImageExtent.depth >> region.mipLevel),
		};
		if (region.width > mipExtent.width || region.height > mipExtent.height || region.depth > mipExtent.depth ||
			region.bufferOffset >= sourceBuffer->GetSize()) {
			return Result::Failed;
		}

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
		return Result::Success;
	}

    void VulkanCommandList::Draw(uint32_t vertexCount) {
        vkCmdDraw(m_CommandBuffer, vertexCount, 1, 0, 0);
    }

    void VulkanCommandList::DrawIndexed(uint32_t indexCount) {
        vkCmdDrawIndexed(m_CommandBuffer, indexCount, 1, 0, 0, 0);
    }
}
