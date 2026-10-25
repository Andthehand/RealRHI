#pragma once
#include "CommandList.h"
#include "VulkanBuffer.h"
#include "VulkanCommandAllocator.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanResourceState.h"
#include "VulkanTextureView.h"

#include <volk.h>

#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace RealRHI {
	class VulkanCommandList : public CommandList {
	public:
		VulkanCommandList(const VulkanDevice* device, Ref<VulkanCommandAllocator> allocator);
		~VulkanCommandList();
		static Result Create(const VulkanDevice* device,
			Ref<VulkanCommandAllocator> allocator,
			Ref<VulkanCommandList>& outCommandList);
		Result Init();

		Result Begin() override;
		Result End() override;
		Result Reset() override;

		void BeginDebugLabel(const char* name) override;
		void EndDebugLabel() override;
		void InsertDebugLabel(const char* name) override;
		bool SupportsTimestamps() const override { return m_TimestampPool != VK_NULL_HANDLE; }
		void WriteTimestamp(const char* name) override;
		Result GetTimestampResults(std::vector<GpuTimestamp>& outTimestamps) const override;

		void BeginRendering(const RenderingInfo& renderingInfo) override;
		void EndRendering() override;

		void SetViewport(const Viewport& vp) override;
		void SetScissor(const Rect& rect) override;

		void UseResource(Texture* texture) override;
		void UseResource(Buffer* buffer) override;
		void UseResource(TextureView* view) override;
		void UseResource(Sampler* sampler) override;

		void BindPipeline(Pipeline* pipeline) override;
		void SetPushConstants(const void* data, uint32_t size, uint32_t offset = 0) override;
		void DispatchCompute(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
		void BindVertexBuffer(Buffer* vertexBuffer, uint32_t binding = 0, uint64_t offset = 0) override;
		void BindIndexBuffer(Buffer* indexBuffer) override;

		void CopyBuffer(Buffer* source, Buffer* destination, const BufferCopyRegion& region) override;
		void CopyBufferToTexture(Buffer* source, Texture* destination, const BufferTextureCopyRegion& region) override;
		void CopyTextureToBuffer(Texture* source, Buffer* destination, const BufferTextureCopyRegion& region) override;
		void CopyTexture(Texture* source, Texture* destination, const TextureCopyRegion& region) override;
		void GenerateMipmaps(Texture* texture, const MipGenerationDesc& desc = {}) override;

		void Barrier(const BarrierDesc& barriers) override;

		void Draw(uint32_t vertexCount,
			uint32_t instanceCount = 1,
			uint32_t firstVertex = 0,
			uint32_t firstInstance = 0) override;
		void DrawIndexed(uint32_t indexCount,
			uint32_t instanceCount = 1,
			uint32_t firstIndex = 0,
			int32_t vertexOffset = 0,
			uint32_t firstInstance = 0) override;

		QueueId GetQueueId() const { return m_QueueId; }
		CommandListState GetState() const override {
			std::scoped_lock lock(m_StateMutex);
			return m_State;
		}
		void SetState(CommandListState state);
		void PrepareTimestampSubmission();

	protected:
		friend class VulkanDevice;
		friend class VulkanTexture;
		friend class VulkanCommandQueue;
		VkCommandBuffer GetCommandBuffer() const { return m_CommandBuffer; }

	private:
		struct SubresourceKey {
			const VulkanTexture* texture = nullptr;
			uint32_t mipLevel = 0;
			uint32_t arrayLayer = 0;

		public:
			bool operator==(const SubresourceKey&) const = default;
		};

		struct SubresourceKeyHash {
			size_t operator()(const SubresourceKey& key) const {
				return std::hash<const VulkanTexture*>{}(key.texture) ^ (static_cast<size_t>(key.mipLevel) << 16) ^
					static_cast<size_t>(key.arrayLayer);
			}
		};
		using LayoutMap = std::unordered_map<SubresourceKey, TextureLayout, SubresourceKeyHash>;

		struct ResourceOperation {
			const char* name = nullptr;
			VulkanBuffer* buffer = nullptr;
			VulkanTexture* texture = nullptr;
			uint64_t offset = 0;
			uint64_t size = 0;
			TextureSubresourceRange subresources{};
			TextureLayout oldLayout = TextureLayout::Undefined;
			TextureLayout newLayout = TextureLayout::Undefined;
			uint32_t sourceFamily = UINT32_MAX;
			uint32_t destinationFamily = UINT32_MAX;
			bool barrier = false;
		};

		struct SubmissionState {
			std::unordered_map<SubresourceKey, VulkanTextureState, SubresourceKeyHash> textures;
			std::unordered_map<VulkanBuffer*, std::vector<VulkanBufferRangeState>> buffers;
		};

	private:
		Result InitTimestamps();
		Result ReadTimestamps();
		bool ValidateDebugLabel(const char* operation, const char* name) const;

		bool ValidateAttachmentView(const VulkanTextureView* view, TextureUsage usage, const Rect& renderArea) const;
		TextureLayout GetExpectedLayout(const VulkanTexture* texture, uint32_t mip, uint32_t layer) const;
		// Replay in command order; publish the complete batch only after native submission succeeds.
		bool ValidateSubmissionState(SubmissionState& predicted,
			SubmissionToken submission,
			std::span<const VkSemaphoreSubmitInfo> waits) const;
		static void CommitSubmissionState(SubmissionState& predicted);
		bool ValidateOwnership(VulkanQueueOwnership& ownership,
			const ResourceOperation& operation,
			SubmissionToken submission,
			std::span<const VkSemaphoreSubmitInfo> waits) const;
		bool RecordVertexBuffers(const char* operation);

		void RecordBufferUse(VulkanBuffer* buffer, uint64_t offset, uint64_t size, const char* operation);
		void RecordTextureUse(VulkanTexture* texture,
			TextureSubresourceRange subresources,
			TextureLayout layout,
			const char* operation);
		void RecordShaderResources(const char* operation);
		bool ValidateMipGeneration(VulkanTexture* texture,
			const MipGenerationDesc& desc,
			TextureSubresourceRange& range) const;
		bool ResolveQueueFamilies(std::optional<QueueType> source,
			std::optional<QueueType> destination,
			uint32_t& sourceFamily,
			uint32_t& destinationFamily) const;

	private:
		// Keep recorded resources alive until this list is reset or destroyed.
		void RetainResource(RefCounted* resource);

		bool HasRecordedLayout(const VulkanTexture* texture,
			uint32_t mipLevel,
			uint32_t baseLayer,
			uint32_t layerCount,
			TextureLayout expected) const;

	private:
		const VulkanDevice* m_Device;
		Ref<VulkanCommandAllocator> m_Allocator;

		const QueueId m_QueueId{};
		mutable std::mutex m_StateMutex;
		std::vector<Ref<RefCounted>> m_ReferencedResources;
		CommandListState m_State = CommandListState::Initial;
		bool m_ComputePipelineBound = false;
		bool m_GraphicsPipelineBound = false;
		bool m_Rendering = false;

		std::vector<ResourceOperation> m_ResourceOperations;
		std::vector<VulkanTextureView*> m_ShaderViews;
		std::vector<VulkanBuffer*> m_ShaderBuffers;
		std::unordered_map<uint32_t, VulkanBuffer*> m_VertexBuffers;
		std::vector<uint32_t> m_ActiveVertexBindings;
		VulkanBuffer* m_IndexBuffer = nullptr;
		// Local layouts check recording consistency without predicting other queues' submissions.
		LayoutMap m_RecordedLayouts;

		VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;

		static constexpr uint32_t s_MaxTimestampCount = 128;
		VkQueryPool m_TimestampPool = VK_NULL_HANDLE;
		uint32_t m_TimestampValidBits = 0;
		float m_TimestampPeriod = 0;
		std::vector<std::string> m_TimestampNames;
		std::vector<GpuTimestamp> m_TimestampResults;
		Result m_TimestampReadResult = Result::NotReady;
	};
}
