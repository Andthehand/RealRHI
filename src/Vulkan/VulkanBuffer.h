#pragma once
#include "Buffer.h"
#include "BufferDesc.h"

#include "VulkanDevice.h"
#include "Result.h"

namespace RealRHI {
	class VulkanBuffer : public Buffer {
	public:
	public:
		static Result Create(const VulkanDevice* device, const BufferDesc& desc, Ref<Buffer>& outBuffer);
		~VulkanBuffer();

		// TODO: Remove this jank
		VkBuffer GetBuffer() const { return m_Buffer; }
	private:
		VulkanBuffer(const VulkanDevice* device, VkBuffer buffer, VmaAllocation allocation);
		const VulkanDevice* m_Device;

		VkBuffer m_Buffer;
		VmaAllocation m_BufferMemory;
	};
}
