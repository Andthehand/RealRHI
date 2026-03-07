#pragma once
#include "CommandList.h"
#include "VulkanDevice.h"
#include "Result.h"

#include <Vulkan/vulkan.h>

namespace RealRHI {
	class VulkanCommandList : public CommandList {
	public:
		static Result Create(const VulkanDevice* device, Ref<CommandList>& outCommandList);
		~VulkanCommandList();

		Result Begin() override;
		Result End() override;
		
		void BeginRendering(const RenderingInfo& renderingInfo) override;
		void EndRendering() override;

		void SetViewport(const Viewport& vp);
		void SetScissor(const Rect& rect);

		void BindPipeline(Pipeline* pipeline) override;
		void BindVertexBuffer(Buffer* vertexBuffer) override;
		void BindIndexBuffer(Buffer* indexBuffer) override;
		void Draw(uint32_t vertexCount) override;

		VkCommandBuffer GetCommandBuffer() const { return m_CommandBuffer; }
	private:
		VulkanCommandList(const VulkanDevice* device, VkCommandPool commandPool, VkCommandBuffer commandBuffer);
		const VulkanDevice* m_Device;

		VkCommandPool m_CommandPool;
		VkCommandBuffer m_CommandBuffer;
	};
}
