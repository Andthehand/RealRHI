#include "TransientResourcePool.h"

#include "Device.h"

#include <algorithm>

namespace RealRHI {
	TransientResourcePool::TransientResourcePool(Device& device)
		: m_Device(&device) { }

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
		for (auto& entry : m_Textures) {
			if (entry.queue == queue && Matches(entry.desc, desc) && IsIdle(entry)) {
				outTexture = entry.texture;
				++m_TextureReuses;
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
		m_Textures.push_back({storedDesc, queue, texture});
		++m_TextureAllocations;
		outTexture = std::move(texture);
		return Result::Success;
	}

	Result TransientResourcePool::AcquireBuffer(QueueId queue, const BufferDesc& desc, Ref<Buffer>& outBuffer) {
		std::scoped_lock lock(m_Mutex);
		for (auto& entry : m_Buffers) {
			if (entry.queue == queue && Matches(entry.desc, desc) && IsIdle(entry)) {
				outBuffer = entry.buffer;
				++m_BufferReuses;
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
		m_Buffers.push_back({storedDesc, queue, buffer});
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
		};
		for (const auto& entry : m_Textures) {
			statistics.idleTextureCount += IsIdle(entry);
		}
		for (const auto& entry : m_Buffers) {
			statistics.idleBufferCount += IsIdle(entry);
		}
		return statistics;
	}

	void TransientResourcePool::Trim() {
		std::scoped_lock lock(m_Mutex);
		std::erase_if(m_Textures, [](const auto& entry) {
			return IsIdle(entry);
		});
		std::erase_if(m_Buffers, [](const auto& entry) {
			return IsIdle(entry);
		});
	}
}
