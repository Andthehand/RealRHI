#include "VulkanBuffer.h"

#include "VulkanConvertions.h"

namespace RealRHI {
    VulkanBuffer::VulkanBuffer(const VulkanDevice* device)
        : m_Device(device) {
    }

    VulkanBuffer::~VulkanBuffer() {
        vmaDestroyBuffer(m_Device->GetAllocator(), m_Buffer, m_BufferMemory);
    }

    Result VulkanBuffer::Create(const VulkanDevice* device, const BufferDesc& desc, Ref<VulkanBuffer>& outBuffer) {
        Ref<VulkanBuffer> buffer = Ref<VulkanBuffer>::Create(device);
        Result res = buffer->Init(desc);
        if (res != Result::Success) {
            return res;
        }

        outBuffer = buffer;
        return Result::Success;
	}

    Result VulkanBuffer::Init(const BufferDesc& desc) {
        VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = desc.size,
            .usage = Utils::BufferUsageToVkBufferUsage(desc.usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, // I don't think we'll ever support sharing buffers between queues
        };

        VmaAllocationCreateInfo vmaAllocCreateInfo = Utils::MemoryUsageToVmaAllocationCreateInfo(desc.memoryUsage);
        if (vmaCreateBuffer(m_Device->GetAllocator(), &bufferInfo, &vmaAllocCreateInfo, &m_Buffer, &m_BufferMemory, &m_AllocInfo) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan buffer.");
            return Result::Failed;
        }

        if (desc.initialData) {
            memcpy(m_AllocInfo.pMappedData, desc.initialData, (size_t)bufferInfo.size);
        }
        
        return Result::Success;
    }

    Result VulkanBuffer::WriteData(const void* data, uint64_t size, uint64_t offset) {
        if (offset + size > m_AllocInfo.size) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Trying to write data outside of the buffer bounds.");
            return Result::InvalidParameter;
		}

        memcpy((char*)m_AllocInfo.pMappedData + offset, data, (size_t)size);

		return Result::Success;
    }
}
