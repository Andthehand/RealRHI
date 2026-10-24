#pragma once

#include "Buffer.h"
#include "Queue.h"
#include "Texture.h"
#include "TextureDesc.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace RealRHI {
	enum class PipelineStage : uint64_t {
		None = 0,
		AllCommands = 1ull << 0,
		VertexShader = 1ull << 1,
		FragmentShader = 1ull << 2,
		ComputeShader = 1ull << 3,
		ColorAttachmentOutput = 1ull << 4,
		EarlyFragmentTests = 1ull << 5,
		LateFragmentTests = 1ull << 6,
		Transfer = 1ull << 7,
		DrawIndirect = 1ull << 8,
		VertexInput = 1ull << 9,
	};

	enum class ResourceAccess : uint64_t {
		None = 0,
		MemoryRead = 1ull << 0,
		MemoryWrite = 1ull << 1,
		ShaderRead = 1ull << 2,
		ShaderWrite = 1ull << 3,
		ColorAttachmentRead = 1ull << 4,
		ColorAttachmentWrite = 1ull << 5,
		DepthStencilRead = 1ull << 6,
		DepthStencilWrite = 1ull << 7,
		TransferRead = 1ull << 8,
		TransferWrite = 1ull << 9,
		IndexRead = 1ull << 10,
		VertexAttributeRead = 1ull << 11,
		IndirectCommandRead = 1ull << 12,
	};

	constexpr PipelineStage operator|(PipelineStage left, PipelineStage right) {
		return static_cast<PipelineStage>(static_cast<uint64_t>(left) | static_cast<uint64_t>(right));
	}

	constexpr ResourceAccess operator|(ResourceAccess left, ResourceAccess right) {
		return static_cast<ResourceAccess>(static_cast<uint64_t>(left) | static_cast<uint64_t>(right));
	}

	struct ResourceState {
		PipelineStage stages = PipelineStage::None;
		ResourceAccess access = ResourceAccess::None;
	};

	struct TextureSubresourceRange {
		uint32_t baseMipLevel = 0;
		uint32_t mipLevelCount = 1;
		uint32_t baseArrayLayer = 0;
		uint32_t arrayLayerCount = 1;
	};

	struct TextureBarrier {
		Texture* texture = nullptr;
		ResourceState before{};
		ResourceState after{};
		TextureLayout oldLayout = TextureLayout::Undefined;
		TextureLayout newLayout = TextureLayout::Undefined;
		TextureSubresourceRange subresources{};
		std::optional<QueueType> sourceQueue;
		std::optional<QueueType> destinationQueue;
	};

	struct BufferBarrier {
		Buffer* buffer = nullptr;
		ResourceState before{};
		ResourceState after{};
		uint64_t offset = 0;
		uint64_t size = 0;
		std::optional<QueueType> sourceQueue;
		std::optional<QueueType> destinationQueue;
	};

	struct MemoryBarrier {
		ResourceState before{};
		ResourceState after{};
	};

	struct BarrierDesc {
		std::vector<TextureBarrier> textures;
		std::vector<BufferBarrier> buffers;
		std::vector<MemoryBarrier> memory;
	};
}
