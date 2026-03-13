#include "VulkanCommandList.h"

#include "VulkanTextureView.h"
#include "VulkanPipeline.h"
#include "VulkanBuffer.h"

#include "VulkanConvertions.h"

namespace RealRHI {
    VulkanCommandList::VulkanCommandList(const VulkanDevice* device)
        : m_Device(device) {
    }

    VulkanCommandList::~VulkanCommandList() {
        vkDestroyCommandPool(m_Device->GetDevice(), m_CommandPool, nullptr);
    }

	Result VulkanCommandList::Create(const VulkanDevice* device, Ref<VulkanCommandList>& outCommandList) {
        Ref<VulkanCommandList> commandList = Ref<VulkanCommandList>::Create(device);
        Result res = commandList->Init();
        if (res != Result::Success) {
            return res;
        }

        outCommandList = commandList;
        return Result::Success;
	}

    Result VulkanCommandList::Init() {
        VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = m_Device->GetGraphicsQueueFamily(),
        };

        if (vkCreateCommandPool(m_Device->GetDevice(), &poolInfo, nullptr, &m_CommandPool) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan command pool.");
            return Result::Failed;
        }

        VkCommandBufferAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = m_CommandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };

        if (vkAllocateCommandBuffers(m_Device->GetDevice(), &allocInfo, &m_CommandBuffer) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to allocate Vulkan command buffer.");
            vkDestroyCommandPool(m_Device->GetDevice(), m_CommandPool, nullptr);
            return Result::Failed;
        }

		return Result::Success;
    }

    Result VulkanCommandList::Begin() {
		vkResetCommandBuffer(m_CommandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        if (vkBeginCommandBuffer(m_CommandBuffer, &beginInfo) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to begin Vulkan command buffer.");
			return Result::Failed;
        }
		return Result::Success;
    }

    Result VulkanCommandList::End() {
        if (vkEndCommandBuffer(m_CommandBuffer) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to end Vulkan command buffer.");
            return Result::Failed;
        }
		return Result::Success;
    }

    void VulkanCommandList::BeginRendering(const RenderingInfo& renderingInfo) {
		std::vector<VkRenderingAttachmentInfo> colorAttachmentInfos(renderingInfo.colorAttachments.size());
        for (uint8_t i = 0; i < renderingInfo.colorAttachments.size(); i++) {
            const auto& attachment = renderingInfo.colorAttachments[i];

			VkClearValue clearColor = { {{attachment.clearColor.r, attachment.clearColor.g, attachment.clearColor.b, attachment.clearColor.a}} };
            colorAttachmentInfos[i] = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = static_cast<VulkanTextureView*>(attachment.target)->GetImageView(),
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .loadOp = Utils::LoadOpToVkLoadOp(attachment.loadOp),
                .storeOp = Utils::StoreOpToVkStoreOp(attachment.storeOp),
                .clearValue = clearColor
			};
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
			.colorAttachmentCount = (uint32_t)colorAttachmentInfos.size(),
			.pColorAttachments = colorAttachmentInfos.data(),
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

    void VulkanCommandList::BindPipeline(Pipeline* pipeline) {
		vkCmdBindPipeline(m_CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<VulkanPipeline*>(pipeline)->GetPipeline());
    }

    void VulkanCommandList::BindVertexBuffer(Buffer* vertexBuffer) {
        VkBuffer vertexBuffers[] = { static_cast<VulkanBuffer*>(vertexBuffer)->GetBuffer() };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(m_CommandBuffer, 0, 1, vertexBuffers, offsets);
    }

    void VulkanCommandList::BindIndexBuffer(Buffer* indexBuffer) {
        vkCmdBindIndexBuffer(m_CommandBuffer, static_cast<VulkanBuffer*>(indexBuffer)->GetBuffer(), 0, VK_INDEX_TYPE_UINT32);
	}

    void VulkanCommandList::Draw(uint32_t vertexCount) {
        vkCmdDraw(m_CommandBuffer, vertexCount, 1, 0, 0);
	}

    void VulkanCommandList::DrawIndexed(uint32_t indexCount){
		vkCmdDrawIndexed(m_CommandBuffer, indexCount, 1, 0, 0, 0);
    }
}
