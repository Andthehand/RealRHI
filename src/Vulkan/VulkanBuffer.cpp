#include "VulkanBuffer.h"

#include "VulkanConvertions.h"

namespace RealRHI {
    Result VulkanBuffer::Create(const VulkanDevice* device, const BufferDesc& desc, Ref<Buffer>& outBuffer) {
        VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = desc.size,
            .usage = Utils::BufferUsageToVkBufferUsage(desc.usage),
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE, // I don't think we'll ever support sharing buffers between queues
        };

		VmaAllocationCreateInfo vmaAllocCreateInfo = Utils::MemoryUsageToVmaAllocationCreateInfo(desc.memoryUsage);
		VmaAllocationInfo vmaAllocInfo;
		VkBuffer buffer;
		VmaAllocation allocation;
		if (vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &vmaAllocCreateInfo, &buffer, &allocation, &vmaAllocInfo) != VK_SUCCESS) {
			device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan buffer.");
            return Result::Failed;
        }

        if (desc.initialData) {
            memcpy(vmaAllocInfo.pMappedData, desc.initialData, (size_t)bufferInfo.size);
        }

		outBuffer = Ref<Buffer>(new VulkanBuffer(device, buffer, allocation));
		return Result::Success;
	}

	VulkanBuffer::VulkanBuffer(const VulkanDevice* device, VkBuffer buffer, VmaAllocation allocation) 
        : m_Device(device), m_Buffer(buffer), m_BufferMemory(allocation) {
	}

    VulkanBuffer::~VulkanBuffer() {
		vmaDestroyBuffer(m_Device->GetAllocator(), m_Buffer, m_BufferMemory);
    }
}
