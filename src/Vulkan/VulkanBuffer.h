#pragma once
#include "Buffer.h"
#include "BufferDesc.h"
#include "VulkanDevice.h"
#include "VulkanResourceState.h"

#include <volk.h>

#include <vma/vk_mem_alloc.h>

#include <vector>

namespace RealRHI {
	class VulkanBuffer : public Buffer {
	public:
		VulkanBuffer(const VulkanDevice* device);
		~VulkanBuffer();

		static Result Create(const VulkanDevice* device, const BufferDesc& desc, Ref<VulkanBuffer>& outBuffer);
		Result Init(const BufferDesc& desc);

		BufferUsage GetUsage() const { return m_Usage; }
		uint64_t GetAllocationSize() const override { return m_AllocInfo.size; }
		uint64_t GetSize() const override { return m_Size; }
		uint32_t GetDescriptorIndex() const override { return m_DescriptorIndex; }

		Result WriteData(const void* data, uint64_t size, uint64_t offset = 0) override;
		Result ReadData(void* data, uint64_t size, uint64_t offset = 0) const override;

	protected:
		friend class VulkanCommandList;
		friend class VulkanResourceStateTransaction;
		friend class VulkanUploadContext;
		friend class VulkanTexture;
		friend class VulkanDescriptorHeaps;
		friend class VulkanDescriptorManager;
		VkBuffer GetBuffer() const { return m_Buffer; }

		VkDeviceAddress GetBufferDeviceAddress() const;
		std::vector<VulkanBufferRangeState> GetSubmittedRanges() const;
		void CommitSubmittedRanges(std::vector<VulkanBufferRangeState> ranges);

	private:
		const VulkanDevice* m_Device = nullptr;

		BufferUsage m_Usage = BufferUsage::None;
		uint64_t m_Size = 0;
		// Protected by the device's state-tracking mutex, like texture state.
		std::vector<VulkanBufferRangeState> m_SubmittedRanges;

		VkBuffer m_Buffer = VK_NULL_HANDLE;
		VmaAllocation m_BufferMemory = VK_NULL_HANDLE;
		VmaAllocationInfo m_AllocInfo{};
		uint32_t m_DescriptorIndex = UINT32_MAX;
	};
}
