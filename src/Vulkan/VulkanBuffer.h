#pragma once
#include "Buffer.h"
#include "BufferDesc.h"

#include "VulkanDevice.h"

#include <volk.h>
#include <vma/vk_mem_alloc.h>

namespace RealRHI {
	class VulkanBuffer : public Buffer {
	public:
		VulkanBuffer(const VulkanDevice* device);
		~VulkanBuffer();

		static Result Create(const VulkanDevice* device, const BufferDesc& desc, Ref<VulkanBuffer>& outBuffer);
		Result Init(const BufferDesc& desc);

		BufferUsage GetUsage() const { return m_Usage; }
		uint64_t GetSize() const override { return m_AllocInfo.size; }

		Result WriteData(const void* data, uint64_t size, uint64_t offset = 0) override;
	protected:
		friend class VulkanCommandList;
		friend class VulkanTexture;
		friend class VulkanDescriptorHeaps;
		friend class VulkanDescriptorManager;
		VkBuffer GetBuffer() const { return m_Buffer; }

		VkDeviceAddress GetBufferDeviceAddress() const;
	private:
		const VulkanDevice* m_Device = nullptr;

		BufferUsage m_Usage = BufferUsage::None;

		VkBuffer m_Buffer = VK_NULL_HANDLE;
		VmaAllocation m_BufferMemory = VK_NULL_HANDLE;
		VmaAllocationInfo m_AllocInfo;
	};
}
