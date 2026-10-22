#pragma once
#include "CommandList.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanCommandAllocator.h"

#include <volk.h>

#include <mutex>
#include <vector>

namespace RealRHI {
	class VulkanCommandList : public CommandList {
	public:
        VulkanCommandList(const VulkanDevice* device, Ref<VulkanCommandAllocator> allocator);
		~VulkanCommandList();
        static Result Create(const VulkanDevice* device, Ref<VulkanCommandAllocator> allocator, Ref<VulkanCommandList>& outCommandList);
		Result Init();

		Result Begin() override;
		Result End() override;
		Result Reset() override;
		CommandListState GetState() const override {
			std::scoped_lock lock(m_StateMutex);
			return m_State;
		}
		void SetState(CommandListState state) {
			std::scoped_lock lock(m_StateMutex);
			m_State = state;
		}

		void BeginRendering(const RenderingInfo& renderingInfo) override;
		void EndRendering() override;

		void SetViewport(const Viewport& vp);
		void SetScissor(const Rect& rect);

		Result UseResource(Texture* texture) override;
		Result UseResource(Buffer* buffer) override;
		void BindPipeline(Pipeline* pipeline) override;
		void BindVertexBuffer(Buffer* vertexBuffer) override;
		void BindIndexBuffer(Buffer* indexBuffer) override;

		Result CopyBuffer(Buffer* source, Buffer* destination, const BufferCopyRegion& region) override;
		Result CopyBufferToTexture(Buffer* source, Texture* destination, const BufferTextureCopyRegion& region) override;

		void Draw(uint32_t vertexCount) override;
		void DrawIndexed(uint32_t indexCount) override;

		QueueId GetQueueId() const { return m_QueueId; }
		const VulkanDevice* GetDevice() const { return m_Device; }

	protected:
		friend class VulkanDevice;
		friend class VulkanTexture;
		friend class VulkanCommandQueue;
		VkCommandBuffer GetCommandBuffer() const { return m_CommandBuffer; }
	private:
		const VulkanDevice* m_Device;
        Ref<VulkanCommandAllocator> m_Allocator;
		VulkanPipeline* m_BoundPipeline = nullptr;
		// Executable command buffers can be resubmitted without rerecording.
		std::vector<Ref<RefCounted>> m_ReferencedResources;
		void RetainResource(RefCounted* resource);
		mutable std::mutex m_StateMutex;
		CommandListState m_State = CommandListState::Initial;
		QueueId m_QueueId{};

		VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
	};
}
