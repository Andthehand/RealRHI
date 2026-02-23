#include "VulkanBuffer.h"

#include "VulkanConvertions.h"

namespace RealRHI {
	VulkanBuffer::VulkanBuffer(const VulkanDevice* device, const BufferDesc& desc) 
        : m_Device(device) {
        VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = desc.size,
            .usage = Utils::BufferUsageToVkBufferUsage(desc.usage),
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE, // I don't think we'll ever support sharing buffers between queues
        };

		VmaAllocationCreateInfo vmaAllocCreateInfo = Utils::MemoryUsageToVmaAllocationCreateInfo(desc.memoryUsage);
		VmaAllocationInfo vmaAllocInfo;
		if (vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &vmaAllocCreateInfo, &m_Buffer, &m_BufferMemory, &vmaAllocInfo) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create vertex buffer!");
        }

        if (desc.initialData) {
            memcpy(vmaAllocInfo.pMappedData, desc.initialData, (size_t)bufferInfo.size);
        }
	}

    VulkanBuffer::~VulkanBuffer() {
		vmaDestroyBuffer(m_Device->GetAllocator(), m_Buffer, m_BufferMemory);
    }
}
