#pragma once
#include "CommandList.h"
#include "VulkanBuffer.h"
#include "VulkanCommandAllocator.h"
#include "VulkanCommandResourceTracker.h"
#include "VulkanCommandTimestamps.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanResourceState.h"
#include "VulkanTextureView.h"

#include <volk.h>

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
		bool SupportsTimestamps() const override { return m_Timestamps.IsSupported(); }
		void WriteTimestamp(const char* name) override;
		uint32_t BeginGpuTiming(const char* name) override;
		void EndGpuTiming(uint32_t timing) override;
		Result GetTimestampResults(std::vector<GpuTimestamp>& outTimestamps) const override;

		void BeginRendering(const RenderingInfo& renderingInfo) override;
		void EndRendering() override;

		void SetViewport(const Viewport& vp) override;
		void SetScissor(const Rect& rect) override;

		void UseResource(Texture* texture) override;
		void UseResource(Buffer* buffer) override;
		void UseResource(TextureView* view) override;
		void UseResource(Sampler* sampler) override;
		void ClearResourceDeclarations() override;
		void RetainResource(RefCounted* resource) override;

		void BindPipeline(Pipeline* pipeline) override;
		void SetPushConstants(const void* data, uint32_t size, uint32_t offset = 0) override;
		void DispatchCompute(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
		void DispatchComputeIndirect(Buffer* arguments, uint64_t offset = 0) override;
		void DrawIndirect(Buffer* arguments,
			uint64_t offset = 0,
			uint32_t drawCount = 1,
			uint32_t stride = sizeof(DrawIndirectArguments)) override;
		void DrawIndexedIndirect(Buffer* arguments,
			uint64_t offset = 0,
			uint32_t drawCount = 1,
			uint32_t stride = sizeof(DrawIndexedIndirectArguments)) override;
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

		QueueId GetQueueId() const override { return m_QueueId; }
		CommandListState GetState() const override {
			std::scoped_lock lock(m_StateMutex);
			return m_State;
		}

	protected:
		friend class VulkanDevice;
		friend class VulkanTexture;
		friend class VulkanCommandQueue;
		VkCommandBuffer GetCommandBuffer() const { return m_CommandBuffer; }

	private:
		void PrepareSubmission();
		void OnSubmitted();
		void OnCompleted();
		void ResetRecordingState();
		bool ValidateDebugLabel(const char* operation, const char* name) const;

		bool ValidateGraphicsState(const char* operation) const;
		bool ValidateAttachmentView(const VulkanTextureView* view, TextureUsage usage, const Rect& renderArea) const;
		TextureLayout GetExpectedLayout(const VulkanTexture* texture, uint32_t mip, uint32_t layer) const;
		bool ReplayResources(VulkanResourceStateTransaction& transaction) const;
		bool ValidateIndirectArguments(const VulkanBuffer* buffer,
			uint64_t offset,
			uint32_t count,
			uint32_t stride,
			uint32_t argumentSize,
			const char* operation) const;
		bool RecordVertexBuffers(const char* operation);

		void RecordShaderResources(const char* operation);
		bool ValidateTransferRecording(const char* operation) const;
		bool ResolveQueueFamilies(std::optional<QueueType> source,
			std::optional<QueueType> destination,
			uint32_t& sourceFamily,
			uint32_t& destinationFamily) const;

	private:
		const VulkanDevice* m_Device;
		Ref<VulkanCommandAllocator> m_Allocator;

		const QueueId m_QueueId{};
		mutable std::mutex m_StateMutex;
		std::vector<Ref<RefCounted>> m_ReferencedResources;
		CommandListState m_State = CommandListState::Initial;
		bool m_ComputePipelineBound = false;
		bool m_Rendering = false;
		VulkanPipeline* m_GraphicsPipeline = nullptr;
		RenderTargetFormats m_RenderTargetFormats;
		uint32_t m_RenderingSampleCount = 0;
		bool m_ReadOnlyDepth = false;
		bool m_ReadOnlyStencil = false;

		VulkanCommandResourceTracker m_ResourceTracker;
		std::vector<VulkanTextureView*> m_ShaderViews;
		std::vector<VulkanBuffer*> m_ShaderBuffers;
		std::unordered_map<uint32_t, VulkanBuffer*> m_VertexBuffers;
		std::vector<uint32_t> m_ActiveVertexBindings;
		VulkanBuffer* m_IndexBuffer = nullptr;

		VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;

		VulkanCommandTimestamps m_Timestamps;
	};
}
