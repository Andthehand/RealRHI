#include "VulkanBuffer.h"

#include "VulkanConvertions.h"
#include "VulkanDescriptorManager.h"

namespace RealRHI {
    VulkanBuffer::VulkanBuffer(const VulkanDevice* device)
        : m_Device(device) {
    }

    VulkanBuffer::~VulkanBuffer() {
        if (m_DescriptorIndex != UINT32_MAX) {
            m_Device->GetDescriptorManager()->FreeBufferDescriptor(m_DescriptorIndex);
        }
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
        m_Usage = desc.usage;

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
        
        if (Any(m_Usage & (BufferUsage::Storage | BufferUsage::Uniform))) {
            m_DescriptorIndex = m_Device->GetDescriptorManager()->AllocateBufferDescriptor(this);
            if (m_DescriptorIndex == UINT32_MAX) {
                return Result::Failed;
            }
        }

        return Result::Success;
    }

    void VulkanBuffer::WriteData(const void* data, uint64_t size, uint64_t offset) {
        if (offset + size > m_AllocInfo.size) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Trying to write data outside of the buffer bounds.");
            return;
		}

        memcpy((char*)m_AllocInfo.pMappedData + offset, data, (size_t)size);

		return;
    }

	Result VulkanBuffer::ReadData(void* data, uint64_t size, uint64_t offset) const {
		if (data == nullptr || offset > m_AllocInfo.size || size > m_AllocInfo.size - offset ||
			m_AllocInfo.pMappedData == nullptr) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "ReadData failed: Buffer is not mapped or read range is invalid.");
			return Result::InvalidParameter;
		}

		if (vmaInvalidateAllocation(m_Device->GetAllocator(), m_BufferMemory, offset, size) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "ReadData failed: Could not invalidate mapped buffer memory.");
			return Result::Failed;
		}

		memcpy(data, static_cast<const char*>(m_AllocInfo.pMappedData) + offset, static_cast<size_t>(size));
		return Result::Success;
	}

    VkDeviceAddress VulkanBuffer::GetBufferDeviceAddress() const {
        VkBufferDeviceAddressInfo addrInfo{ 
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			.buffer = m_Buffer
        };

        return vkGetBufferDeviceAddress(m_Device->GetDevice(), &addrInfo);
    }
}
