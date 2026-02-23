#pragma once
#include "Buffer.h"
#include "BufferDesc.h"

#include "VulkanDevice.h"

namespace RealRHI {
	class VulkanBuffer : public Buffer {
	public:
		VulkanBuffer(const VulkanDevice* device, const BufferDesc& desc);
		~VulkanBuffer();

		// TODO: Remove this jank
		VkBuffer GetBuffer() const { return m_Buffer; }
	private:
		const VulkanDevice* m_Device;

		VkBuffer m_Buffer;
		VmaAllocation m_BufferMemory;
	};
}
