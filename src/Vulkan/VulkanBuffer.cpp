#include "VulkanBuffer.h"

#include "VulkanConversions.h"
#include "VulkanDescriptorManager.h"

namespace RealRHI {
	VulkanBuffer::VulkanBuffer(const VulkanDevice* device)
		: m_Device(device) { }

	VulkanBuffer::~VulkanBuffer() {
		if (m_Buffer != VK_NULL_HANDLE) {
			m_Device->RecordResourceDestroyed(VulkanProfiledResource::Buffer, m_AllocInfo.size);
		}
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
		if (desc.size == 0 || desc.usage == BufferUsage::None ||
			(desc.initialData != nullptr && desc.memoryUsage == MemoryUsage::GPUOnly)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateBuffer: Provide a nonzero size and usage; initialData requires "
				"CPU-accessible memory. Use a staging copy for GPUOnly buffers.");
			return Result::InvalidParameter;
		}
		m_Usage = desc.usage;
		m_Size = desc.size;
		m_SubmittedRanges = {{.offset = 0, .size = m_Size}};

		VkBufferCreateInfo bufferInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = desc.size,
			.usage = Utils::BufferUsageToVkBufferUsage(desc.usage),
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};

		VmaAllocationCreateInfo vmaAllocCreateInfo = Utils::MemoryUsageToVmaAllocationCreateInfo(desc.memoryUsage);
		if (vmaCreateBuffer(m_Device->GetAllocator(), &bufferInfo, &vmaAllocCreateInfo, &m_Buffer, &m_BufferMemory, &m_AllocInfo) !=VK_SUCCESS) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan buffer.");
			return Result::Failed;
		}

		m_Device->RecordResourceCreated(VulkanProfiledResource::Buffer, m_AllocInfo.size);
		const Result nameResult = m_Device->SetDebugName(VK_OBJECT_TYPE_BUFFER, m_Buffer, desc.debugName);
		if (nameResult != Result::Success) {
			return nameResult;
		}

		if (desc.initialData) {
			if (m_AllocInfo.pMappedData == nullptr ||
				vmaCopyMemoryToAllocation(m_Device->GetAllocator(), desc.initialData, m_BufferMemory, 0, desc.size) !=VK_SUCCESS) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"CreateBuffer: Failed to copy and flush initial data to mapped memory.");
				return Result::Failed;
			}
		}

		if (Any(m_Usage & (BufferUsage::Storage | BufferUsage::Uniform))) {
			m_DescriptorIndex = m_Device->GetDescriptorManager()->AllocateBufferDescriptor(this, desc.debugName);
			if (m_DescriptorIndex == UINT32_MAX) {
				return Result::Failed;
			}
		}

		return Result::Success;
	}

	void VulkanBuffer::WriteData(const void* data, uint64_t size, uint64_t offset) {
		CopyToMappedMemory(data, size, offset);
	}

	Result VulkanBuffer::CopyToMappedMemory(const void* data, uint64_t size, uint64_t offset) {
		if (data == nullptr || m_AllocInfo.pMappedData == nullptr || offset > m_Size || size > m_Size - offset) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"WriteData: Provide non-null data, a mapped buffer, and a range within GetSize().");
			return Result::InvalidParameter;
		}
		if (size == 0) {
			return Result::Success;
		}
		if (vmaCopyMemoryToAllocation(m_Device->GetAllocator(), data, m_BufferMemory, offset, size) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"WriteData: Failed to copy and flush mapped buffer memory.");
			return Result::Failed;
		}
		return Result::Success;
	}

	Result VulkanBuffer::ReadData(void* data, uint64_t size, uint64_t offset) const {
		if (data == nullptr || offset > m_Size || size > m_Size - offset || m_AllocInfo.pMappedData == nullptr) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"ReadData failed: Buffer is not mapped or read range is invalid.");
			return Result::InvalidParameter;
		}
		if (size == 0) {
			return Result::Success;
		}

		if (vmaInvalidateAllocation(m_Device->GetAllocator(), m_BufferMemory, offset, size) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"ReadData failed: Could not invalidate mapped buffer memory.");
			return Result::Failed;
		}

		memcpy(data, static_cast<const char*>(m_AllocInfo.pMappedData) + offset, static_cast<size_t>(size));
		return Result::Success;
	}

	std::vector<VulkanBufferRangeState> VulkanBuffer::GetSubmittedRanges() const {
		std::scoped_lock lock(m_Device->GetStateTrackingMutex());
		return m_SubmittedRanges;
	}

	void VulkanBuffer::CommitSubmittedRanges(std::vector<VulkanBufferRangeState> ranges) {
		std::scoped_lock lock(m_Device->GetStateTrackingMutex());
		// Repeated copies must not leave an ever-growing list of equivalent byte ranges.
		size_t count = 0;
		for (size_t i = 0; i < ranges.size(); ++i) {
			if (count > 0 && ranges[count - 1].ownership == ranges[i].ownership &&
				(ranges[i].ownership.pendingFamily == UINT32_MAX ||
					(ranges[count - 1].releaseOffset == ranges[i].releaseOffset &&
						ranges[count - 1].releaseSize == ranges[i].releaseSize))) {
				ranges[count - 1].size += ranges[i].size;
			}
			else {
				ranges[count++] = ranges[i];
			}
		}
		ranges.resize(count);
		m_SubmittedRanges = std::move(ranges);
	}

	VkDeviceAddress VulkanBuffer::GetBufferDeviceAddress() const {
		VkBufferDeviceAddressInfo addrInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			.buffer = m_Buffer,
		};

		return vkGetBufferDeviceAddress(m_Device->GetDevice(), &addrInfo);
	}
}
