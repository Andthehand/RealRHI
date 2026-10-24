#pragma once
#include "BarrierDesc.h"
#include "Buffer.h"
#include "Pipeline.h"
#include "RefCounted.h"
#include "RenderingInfo.h"
#include "Result.h"
#include "Sampler.h"
#include "SamplerDesc.h"
#include "Texture.h"

#include <cstdint>

namespace RealRHI {
	struct MipGenerationDesc {
		uint32_t baseMipLevel = 0;
		// Zero selects all remaining mips/layers. At least two mip levels are required.
		uint32_t mipLevelCount = 0;
		uint32_t baseArrayLayer = 0;
		uint32_t arrayLayerCount = 0;

		Filter filter = Filter::Linear;
		TextureLayout finalLayout = TextureLayout::ShaderRead;
		ResourceState finalState{PipelineStage::AllCommands, ResourceAccess::ShaderRead};
	};

	struct BufferCopyRegion {
		uint64_t sourceOffset = 0;
		uint64_t destinationOffset = 0;
		uint64_t size = 0;
	};

	struct BufferTextureCopyRegion {
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t depth = 1;

		uint64_t bufferOffset = 0;
		uint32_t mipLevel = 0;
		uint32_t baseArrayLayer = 0;
		uint32_t layerCount = 1;
	};

	struct TextureCopyRegion {
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t depth = 1;

		uint32_t sourceMipLevel = 0;
		uint32_t destinationMipLevel = 0;
		uint32_t sourceBaseArrayLayer = 0;
		uint32_t destinationBaseArrayLayer = 0;
		uint32_t layerCount = 1;
	};

	enum class CommandListState {
		Initial,
		Recording,
		Executable,
		Pending
	};

	// Callers must serialize recording, reset, and submission of each list.
	class CommandList : public RefCounted {
	public:
		virtual ~CommandList() = default;

		virtual Result Begin() = 0;
		virtual Result End() = 0;
		virtual Result Reset() = 0;
		virtual CommandListState GetState() const = 0;

		virtual void BeginRendering(const RenderingInfo& renderingInfo) = 0;
		virtual void EndRendering() = 0;

		virtual void SetViewport(const Viewport& vp) = 0;
		virtual void SetScissor(const Rect& rect) = 0;

		// Explicitly declare shader-visible bindless resources for this recording.
		// Must be called while recording, before submission. Retained through reset.
		// Declarations apply to every subsequent draw/dispatch until Reset; those
		// operations validate ownership of the whole buffer or selected texture view.
		// Declare independent samplers here as well to retain their descriptor slots.
		virtual void UseResource(Texture* texture) = 0;
		virtual void UseResource(Buffer* buffer) = 0;
		virtual void UseResource(TextureView* view) = 0;
		virtual void UseResource(Sampler* sampler) = 0;

		virtual void BindPipeline(Pipeline* pipeline) = 0;
		virtual void SetPushConstants(const void* data, uint32_t size, uint32_t offset = 0) = 0;
		virtual void DispatchCompute(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
		virtual void BindVertexBuffer(Buffer* vertexBuffer) = 0;
		virtual void BindIndexBuffer(Buffer* indexBuffer) = 0;

		virtual void CopyBuffer(Buffer* source, Buffer* destination, const BufferCopyRegion& region) = 0;
		virtual void CopyBufferToTexture(Buffer* source,
			Texture* destination,
			const BufferTextureCopyRegion& region) = 0;
		virtual void CopyTextureToBuffer(Texture* source,
			Buffer* destination,
			const BufferTextureCopyRegion& region) = 0;
		virtual void CopyTexture(Texture* source, Texture* destination, const TextureCopyRegion& region) = 0;

		virtual void GenerateMipmaps(Texture* texture, const MipGenerationDesc& desc = {}) = 0;

		virtual void Barrier(const BarrierDesc& barriers) = 0;

		virtual void Draw(uint32_t vertexCount) = 0;
		virtual void DrawIndexed(uint32_t indexCount) = 0;
	};
}
