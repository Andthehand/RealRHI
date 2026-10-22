#pragma once
#include "CommandList.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanCommandAllocator.h"

#include <volk.h>

#include <mutex>
#include <vector>
#include <unordered_map>

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
		Result CopyTextureToBuffer(Texture* source, Buffer* destination, const BufferTextureCopyRegion& region) override;
		Result CopyTexture(Texture* source, Texture* destination, const TextureCopyRegion& region) override;

		Result Barrier(const BarrierDesc& barriers) override;

		void Draw(uint32_t vertexCount) override;
		void DrawIndexed(uint32_t indexCount) override;

		QueueId GetQueueId() const { return m_QueueId; }

	protected:
		friend class VulkanDevice;
		friend class VulkanTexture;
		friend class VulkanCommandQueue;
		VkCommandBuffer GetCommandBuffer() const { return m_CommandBuffer; }
	public:
		struct SubresourceKey {
			const VulkanTexture* texture = nullptr;
			uint32_t mipLevel = 0;
			uint32_t arrayLayer = 0;
			bool operator==(const SubresourceKey&) const = default;
		};
		struct SubresourceKeyHash {
			size_t operator()(const SubresourceKey& key) const {
				return std::hash<const VulkanTexture*>{}(key.texture) ^
					(static_cast<size_t>(key.mipLevel) << 16) ^ static_cast<size_t>(key.arrayLayer);
			}
		};
		using LayoutMap = std::unordered_map<SubresourceKey, TextureLayout, SubresourceKeyHash>;

		TextureLayout GetExpectedLayout(const VulkanTexture* texture, uint32_t mip, uint32_t layer) const;
		bool ValidateSubmissionLayouts(LayoutMap& predicted) const;
		void CommitSubmissionLayouts() const;
	private:
		void RetainResource(RefCounted* resource);
		void RegisterRequiredLayout(const VulkanTexture* texture, uint32_t mipLevel, uint32_t baseLayer,
			uint32_t layerCount, TextureLayout expected);
		bool HasRecordedLayout(const VulkanTexture* texture, uint32_t mipLevel, uint32_t baseLayer,
			uint32_t layerCount, TextureLayout expected) const;

	private:
		std::unordered_map<SubresourceKey, TextureLayout, SubresourceKeyHash> m_RecordedLayouts;
		std::unordered_map<SubresourceKey, TextureLayout, SubresourceKeyHash> m_InitialLayouts;
		const VulkanDevice* m_Device;
        Ref<VulkanCommandAllocator> m_Allocator;
		
		mutable std::mutex m_StateMutex;
		std::vector<Ref<RefCounted>> m_ReferencedResources;
		CommandListState m_State = CommandListState::Initial;
		QueueId m_QueueId{};

		VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
	};
}
