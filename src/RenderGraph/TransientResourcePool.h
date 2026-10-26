#pragma once
#include "Buffer.h"
#include "BufferDesc.h"
#include "Queue.h"
#include "Texture.h"
#include "TextureDesc.h"

#include <mutex>
#include <vector>

namespace RealRHI {
	class Device;
	class RenderGraph;

	struct TransientResourcePoolStatistics {
		uint64_t textureCount = 0;
		uint64_t bufferCount = 0;
		uint64_t idleTextureCount = 0;
		uint64_t idleBufferCount = 0;
		uint64_t textureAllocations = 0;
		uint64_t bufferAllocations = 0;
		uint64_t textureReuses = 0;
		uint64_t bufferReuses = 0;
		uint64_t budgetBytes = 0;
		uint64_t residentBytes = 0;
		uint64_t idleBytes = 0;
		uint64_t peakResidentBytes = 0;
		uint64_t evictions = 0;
	};

	// Caches complete allocations between executions. The budget is a soft resident
	// limit: busy allocations are never evicted and may temporarily exceed it.
	// Acquire and Collect evict idle entries in least-recently-acquired order.
	// Device must outlive the pool and its resources. Use this pool with that device.
	// Resources are idle only when the pool is their sole owner: completed command
	// lists must be reset/destroyed, and graphs/exported Ref objects must release them.
	// Raw pointers and descriptor indices do not reserve an allocation; keep a Ref.
	class TransientResourcePool {
	public:
		explicit TransientResourcePool(Device& device, uint64_t budgetBytes = 256ull * 1024 * 1024);
		TransientResourcePool(const TransientResourcePool&) = delete;
		TransientResourcePool& operator=(const TransientResourcePool&) = delete;

		TransientResourcePoolStatistics GetStatistics() const;
		// Release only idle allocations; recordings and exported outputs stay valid.
		void Trim();
		void SetBudget(uint64_t budgetBytes);
		// Enforce the budget after completed recordings/exports release their refs.
		// No CPU/GPU waits. Zero budget disables retention of idle allocations.
		void Collect();

	private:
		struct TextureEntry {
			TextureDesc desc;
			QueueId queue;
			Ref<Texture> texture;
			uint64_t lastUse = 0;
		};
		struct BufferEntry {
			BufferDesc desc;
			QueueId queue;
			Ref<Buffer> buffer;
			uint64_t lastUse = 0;
		};

	private:
		friend class RenderGraph;
		Result AcquireTexture(QueueId queue, const TextureDesc& desc, Ref<Texture>& outTexture);
		Result AcquireBuffer(QueueId queue, const BufferDesc& desc, Ref<Buffer>& outBuffer);

		void EvictToBudget(uint64_t budgetBytes);
		static bool IsIdle(const TextureEntry& entry);
		static bool IsIdle(const BufferEntry& entry);

		static bool Matches(const TextureDesc& left, const TextureDesc& right);
		static bool Matches(const BufferDesc& left, const BufferDesc& right);

	private:
		Device* m_Device;
		mutable std::mutex m_Mutex;
		std::vector<TextureEntry> m_Textures;
		std::vector<BufferEntry> m_Buffers;
		uint64_t m_BudgetBytes;
		uint64_t m_ResidentBytes = 0;
		uint64_t m_PeakResidentBytes = 0;
		uint64_t m_Evictions = 0;
		uint64_t m_Clock = 0;
		uint64_t m_TextureAllocations = 0;
		uint64_t m_BufferAllocations = 0;
		uint64_t m_TextureReuses = 0;
		uint64_t m_BufferReuses = 0;
	};
}
