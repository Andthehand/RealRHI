#include "TransientResourcePool.h"

#include "Device.h"

#include <algorithm>

namespace RealRHI {
	TransientResourcePool::TransientResourcePool(Device& device, uint64_t budgetBytes)
		: m_Device(&device), m_BudgetBytes(budgetBytes) { }

	bool TransientResourcePool::Matches(const TextureDesc& left, const TextureDesc& right) {
		return left.width == right.width && left.height == right.height && left.depth == right.depth &&
			left.mipLevels == right.mipLevels && left.arrayLayers == right.arrayLayers && left.format == right.format &&
			left.usage == right.usage;
	}

	bool TransientResourcePool::Matches(const BufferDesc& left, const BufferDesc& right) {
		return left.size == right.size && left.usage == right.usage && left.memoryUsage == right.memoryUsage;
	}

	bool TransientResourcePool::IsIdle(const TextureEntry& entry) {
		if (entry.texture->GetRefCount() != 1) {
			return false;
		}
		// Default views borrow their image. An independently retained default view
		// must also prevent recycling; custom views already retain their parent.
		const auto* view = entry.texture->GetTextureView();
		return view == nullptr || view->GetRefCount() == 1;
	}

	bool TransientResourcePool::IsIdle(const BufferEntry& entry) {
		return entry.buffer->GetRefCount() == 1;
	}

	Result TransientResourcePool::AcquireTexture(QueueId queue, const TextureDesc& desc, Ref<Texture>& outTexture) {
		std::scoped_lock lock(m_Mutex);
		EvictToBudget(m_BudgetBytes);
		for (auto& entry : m_Textures) {
			if (entry.queue == queue && Matches(entry.desc, desc) && IsIdle(entry)) {
				outTexture = entry.texture;
				entry.lastUse = ++m_Clock;
				++m_TextureReuses;
				EvictToBudget(m_BudgetBytes);
				return Result::Success;
			}
		}

		Ref<Texture> texture;
		const Result result = m_Device->CreateTexture(desc, texture);
		if (result != Result::Success) {
			return result;
		}
		auto storedDesc = desc;
		storedDesc.debugName = nullptr;
		m_Textures.push_back({storedDesc, queue, texture, ++m_Clock});
		m_ResidentBytes += texture->GetAllocationSize();
		m_PeakResidentBytes = std::max(m_PeakResidentBytes, m_ResidentBytes);
		EvictToBudget(m_BudgetBytes);
		++m_TextureAllocations;
		outTexture = std::move(texture);
		return Result::Success;
	}

	Result TransientResourcePool::AcquireBuffer(QueueId queue, const BufferDesc& desc, Ref<Buffer>& outBuffer) {
		std::scoped_lock lock(m_Mutex);
		EvictToBudget(m_BudgetBytes);
		for (auto& entry : m_Buffers) {
			if (entry.queue == queue && Matches(entry.desc, desc) && IsIdle(entry)) {
				outBuffer = entry.buffer;
				entry.lastUse = ++m_Clock;
				++m_BufferReuses;
				EvictToBudget(m_BudgetBytes);
				return Result::Success;
			}
		}

		Ref<Buffer> buffer;
		const Result result = m_Device->CreateBuffer(desc, buffer);
		if (result != Result::Success) {
			return result;
		}
		auto storedDesc = desc;
		storedDesc.debugName = nullptr;
		storedDesc.initialData = nullptr;
		m_Buffers.push_back({storedDesc, queue, buffer, ++m_Clock});
		m_ResidentBytes += buffer->GetAllocationSize();
		m_PeakResidentBytes = std::max(m_PeakResidentBytes, m_ResidentBytes);
		EvictToBudget(m_BudgetBytes);
		++m_BufferAllocations;
		outBuffer = std::move(buffer);
		return Result::Success;
	}

	TransientResourcePoolStatistics TransientResourcePool::GetStatistics() const {
		std::scoped_lock lock(m_Mutex);
		TransientResourcePoolStatistics statistics{
			.textureCount = m_Textures.size(),
			.bufferCount = m_Buffers.size(),
			.textureAllocations = m_TextureAllocations,
			.bufferAllocations = m_BufferAllocations,
			.textureReuses = m_TextureReuses,
			.bufferReuses = m_BufferReuses,
			.budgetBytes = m_BudgetBytes,
			.residentBytes = m_ResidentBytes,
			.peakResidentBytes = m_PeakResidentBytes,
			.evictions = m_Evictions,
		};
		for (const auto& entry : m_Textures) {
			if (IsIdle(entry)) {
				++statistics.idleTextureCount;
				statistics.idleBytes += entry.texture->GetAllocationSize();
			}
		}
		for (const auto& entry : m_Buffers) {
			if (IsIdle(entry)) {
				++statistics.idleBufferCount;
				statistics.idleBytes += entry.buffer->GetAllocationSize();
			}
		}
		return statistics;
	}

	void TransientResourcePool::EvictToBudget(uint64_t budgetBytes) {
		while (m_ResidentBytes > budgetBytes) {
			size_t texture = m_Textures.size();
			size_t buffer = m_Buffers.size();
			uint64_t oldest = UINT64_MAX;
			for (size_t i = 0; i < m_Textures.size(); ++i) {
				if (m_Textures[i].lastUse < oldest && IsIdle(m_Textures[i])) {
					oldest = m_Textures[i].lastUse;
					texture = i;
				}
			}
			for (size_t i = 0; i < m_Buffers.size(); ++i) {
				if (m_Buffers[i].lastUse < oldest && IsIdle(m_Buffers[i])) {
					oldest = m_Buffers[i].lastUse;
					buffer = i;
				}
			}
			if (buffer < m_Buffers.size()) {
				m_ResidentBytes -= m_Buffers[buffer].buffer->GetAllocationSize();
				m_Buffers.erase(m_Buffers.begin() + buffer);
			}
			else if (texture < m_Textures.size()) {
				m_ResidentBytes -= m_Textures[texture].texture->GetAllocationSize();
				m_Textures.erase(m_Textures.begin() + texture);
			}
			else {
				break;
			}
			++m_Evictions;
		}
	}

	void TransientResourcePool::SetBudget(uint64_t budgetBytes) {
		std::scoped_lock lock(m_Mutex);
		m_BudgetBytes = budgetBytes;
		EvictToBudget(m_BudgetBytes);
	}

	void TransientResourcePool::Collect() {
		std::scoped_lock lock(m_Mutex);
		EvictToBudget(m_BudgetBytes);
	}

	void TransientResourcePool::Trim() {
		std::scoped_lock lock(m_Mutex);
		EvictToBudget(0);
	}
}
