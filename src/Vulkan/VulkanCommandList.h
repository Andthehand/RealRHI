#pragma once
#include "CommandList.h"
#include "VulkanDevice.h"

#include <Vulkan/vulkan.h>

namespace RealRHI {
	class VulkanCommandList : public CommandList {
	public:
		VulkanCommandList(const VulkanDevice* device);
		~VulkanCommandList();

		static Result Create(const VulkanDevice* device, Ref<VulkanCommandList>& outCommandList);
		Result Init();

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
		void DrawIndexed(uint32_t indexCount) override;
	protected:
		friend class VulkanDevice;
		friend class VulkanTexture;
		VkCommandBuffer GetCommandBuffer() const { return m_CommandBuffer; }
	private:
		const VulkanDevice* m_Device;

		VkCommandBuffer m_CommandBuffer;
	};
}
