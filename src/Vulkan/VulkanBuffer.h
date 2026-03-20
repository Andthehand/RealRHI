#pragma once
#include "Buffer.h"
#include "BufferDesc.h"

#include "VulkanDevice.h"
#include "Result.h"

namespace RealRHI {
	class VulkanBuffer : public Buffer {
	public:
		VulkanBuffer(const VulkanDevice* device);
		~VulkanBuffer();

		static Result Create(const VulkanDevice* device, const BufferDesc& desc, Ref<VulkanBuffer>& outBuffer);
		Result Init(const BufferDesc& desc);

		Result WriteData(const void* data, uint64_t size, uint64_t offset = 0) override;
	protected:
		friend class VulkanCommandList;
		friend class VulkanTexture;
		VkBuffer GetBuffer() const { return m_Buffer; }
	private:
		const VulkanDevice* m_Device = nullptr;

		VkBuffer m_Buffer = VK_NULL_HANDLE;
		VmaAllocation m_BufferMemory = VK_NULL_HANDLE;
		VmaAllocationInfo m_AllocInfo;
	};
}
