#include "VulkanDescriptorManager.h"

#include "VulkanBuffer.h"
#include "VulkanConversions.h"

#include <algorithm>

namespace RealRHI {
	VulkanDescriptorManager::VulkanDescriptorManager(const VulkanDevice* device)
		: m_Device(device) { }

	Result VulkanDescriptorManager::Init() {
		m_DescriptorHeaps = std::make_unique<VulkanDescriptorHeaps>(m_Device);
		if (m_DescriptorHeaps->Init(m_Device->GetDescriptorHeapProperties()) != Result::Success) {
			return Result::Failed;
		}

		const VkPhysicalDeviceDescriptorHeapPropertiesEXT& heapProperties = m_DescriptorHeaps->GetHeapProperties();
		m_BufferDescriptorSize = heapProperties.bufferDescriptorSize;
		m_ImageDescriptorSize = heapProperties.imageDescriptorSize;
		m_SamplerDescriptorSize = heapProperties.samplerDescriptorSize;
		if (m_BufferDescriptorSize == 0 || m_ImageDescriptorSize == 0 || m_SamplerDescriptorSize == 0) {
			return Result::Failed;
		}

		m_NextBufferDescriptorIndex = 0;
		// For image descriptors, we start from the end of the heap and decrement
		m_ImageDescriptorCapacity =
			static_cast<uint32_t>(m_DescriptorHeaps->GetResourceHeap().usableSize / m_ImageDescriptorSize);
		m_NextImageDescriptorIndex = m_ImageDescriptorCapacity;
		m_NextSamplerDescriptorIndex = 0;

		return Result::Success;
	}

	void VulkanDescriptorManager::Cleanup() {
		m_DescriptorHeaps.reset();
	}

	void VulkanDescriptorManager::BindDescriptorHeaps(VkCommandBuffer commandBuffer) const {
		m_DescriptorHeaps->BindDescriptorHeaps(commandBuffer);
	}

	uint32_t VulkanDescriptorManager::AllocateBufferDescriptor(const VulkanBuffer* buffer, const char* debugName) {
		std::scoped_lock lock(m_DescriptorMutex);
		uint32_t bufferDescriptorIndex;
		if (!m_FreeBufferDescriptors.empty()) {
			bufferDescriptorIndex = m_FreeBufferDescriptors.back();
			m_FreeBufferDescriptors.pop_back();
		}
		else {
			if ((static_cast<VkDeviceSize>(m_NextBufferDescriptorIndex) + 1) * m_BufferDescriptorSize >
				static_cast<VkDeviceSize>(m_NextImageDescriptorIndex) * m_ImageDescriptorSize) {
				return UINT32_MAX;
			}
			bufferDescriptorIndex = m_NextBufferDescriptorIndex++;
		}

		VkDeviceAddressRangeEXT deviceAddressRange{
			.address = buffer->GetBufferDeviceAddress(),
			.size = buffer->GetSize(),
		};

		VkResourceDescriptorInfoEXT descriptorInfo{
			.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
			.type = Utils::BufferUsageToVkDescriptorType(buffer->GetUsage()),
			.data = {.pAddressRange = &deviceAddressRange},
		};
		const VkDebugUtilsObjectNameInfoEXT nameInfo{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			.objectType = VK_OBJECT_TYPE_UNKNOWN,
			.objectHandle = 0,
			.pObjectName = RetainDebugName(debugName),
		};
		if (nameInfo.pObjectName != nullptr) {
			descriptorInfo.pNext = &nameInfo;
		}

		std::vector<uint8_t> descriptorMemory(m_BufferDescriptorSize);
		VkHostAddressRangeEXT hostAddressRange{
			.address = descriptorMemory.data(),
			.size = m_BufferDescriptorSize,
		};

		if (vkWriteResourceDescriptorsEXT(m_Device->GetDevice(), 1, &descriptorInfo, &hostAddressRange) != VK_SUCCESS) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to write Vulkan buffer descriptor.");
			m_FreeBufferDescriptors.push_back(bufferDescriptorIndex);

			return UINT32_MAX;
		}

		m_DescriptorHeaps->GetResourceHeap().buffer->WriteData(
			descriptorMemory.data(), m_BufferDescriptorSize, bufferDescriptorIndex * m_BufferDescriptorSize);
		m_AllocatedBufferDescriptors.insert(bufferDescriptorIndex);
		UpdateHighWaterMarks();
		return bufferDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeBufferDescriptor(uint32_t index) {
		std::scoped_lock lock(m_DescriptorMutex);
		FreeDescriptor(index, m_AllocatedBufferDescriptors, m_FreeBufferDescriptors);
	}

	uint32_t VulkanDescriptorManager::AllocateImageDescriptor(const VkImageViewCreateInfo& viewInfo,
		VkDescriptorType descriptorType,
		VkImageLayout descriptorAccessLayout,
		const char* debugName) {
		std::scoped_lock lock(m_DescriptorMutex);
		uint32_t imageDescriptorIndex;
		if (!m_FreeImageDescriptors.empty()) {
			imageDescriptorIndex = m_FreeImageDescriptors.back();
			m_FreeImageDescriptors.pop_back();
		}
		else {
			if (m_NextImageDescriptorIndex == 0 ||
				static_cast<VkDeviceSize>(m_NextImageDescriptorIndex - 1) * m_ImageDescriptorSize <
					static_cast<VkDeviceSize>(m_NextBufferDescriptorIndex) * m_BufferDescriptorSize) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"AllocateImageDescriptor: Resource descriptor heap is full.");

				return UINT32_MAX;
			}
			imageDescriptorIndex = --m_NextImageDescriptorIndex;
		}

		VkImageDescriptorInfoEXT imageDescriptorInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
			.pView = &viewInfo,
			.layout = descriptorAccessLayout,
		};

		VkResourceDescriptorInfoEXT descriptorInfo{
			.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
			.type = descriptorType,
			.data = {
				.pImage = &imageDescriptorInfo,
			},
		};
		const VkDebugUtilsObjectNameInfoEXT nameInfo{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			.objectType = VK_OBJECT_TYPE_UNKNOWN,
			.objectHandle = 0,
			.pObjectName = RetainDebugName(debugName),
		};
		if (nameInfo.pObjectName != nullptr) {
			descriptorInfo.pNext = &nameInfo;
		}

		std::vector<uint8_t> descriptorMemory(m_ImageDescriptorSize);
		VkHostAddressRangeEXT hostAddressRange{
			.address = descriptorMemory.data(),
			.size = m_ImageDescriptorSize,
		};

		if (vkWriteResourceDescriptorsEXT(m_Device->GetDevice(), 1, &descriptorInfo, &hostAddressRange) != VK_SUCCESS) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to write Vulkan image descriptor.");
			m_FreeImageDescriptors.push_back(imageDescriptorIndex);

			return UINT32_MAX;
		}

		m_DescriptorHeaps->GetResourceHeap().buffer->WriteData(
			descriptorMemory.data(), m_ImageDescriptorSize, imageDescriptorIndex * m_ImageDescriptorSize);
		m_AllocatedImageDescriptors.insert(imageDescriptorIndex);
		UpdateHighWaterMarks();
		return imageDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeImageDescriptor(uint32_t index) {
		std::scoped_lock lock(m_DescriptorMutex);
		FreeDescriptor(index, m_AllocatedImageDescriptors, m_FreeImageDescriptors);
	}

	uint32_t VulkanDescriptorManager::AllocateSamplerDescriptor(const VkSamplerCreateInfo& samplerCI,
		const char* debugName) {
		std::scoped_lock lock(m_DescriptorMutex);
		uint32_t samplerDescriptorIndex;
		if (!m_FreeSamplerDescriptors.empty()) {
			samplerDescriptorIndex = m_FreeSamplerDescriptors.back();
			m_FreeSamplerDescriptors.pop_back();
		}
		else {
			if ((static_cast<VkDeviceSize>(m_NextSamplerDescriptorIndex) + 1) * m_SamplerDescriptorSize >
				m_DescriptorHeaps->GetSamplerHeap().usableSize) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"AllocateSamplerDescriptor: Sampler descriptor heap is full.");

				return UINT32_MAX;
			}
			samplerDescriptorIndex = m_NextSamplerDescriptorIndex++;
		}

		std::vector<uint8_t> descriptorMemory(m_SamplerDescriptorSize);
		VkHostAddressRangeEXT hostAddressRange{
			.address = descriptorMemory.data(),
			.size = m_SamplerDescriptorSize,
		};

		const VkDebugUtilsObjectNameInfoEXT nameInfo{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			.pNext = samplerCI.pNext,
			.objectType = VK_OBJECT_TYPE_UNKNOWN,
			.objectHandle = 0,
			.pObjectName = RetainDebugName(debugName),
		};
		VkSamplerCreateInfo namedSampler = samplerCI;
		if (nameInfo.pObjectName != nullptr) {
			namedSampler.pNext = &nameInfo;
		}

		if (vkWriteSamplerDescriptorsEXT(m_Device->GetDevice(), 1, &namedSampler, &hostAddressRange) != VK_SUCCESS) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to write Vulkan sampler descriptor.");
			m_FreeSamplerDescriptors.push_back(samplerDescriptorIndex);

			return UINT32_MAX;
		}

		m_DescriptorHeaps->GetSamplerHeap().buffer->WriteData(
			descriptorMemory.data(), m_SamplerDescriptorSize, samplerDescriptorIndex * m_SamplerDescriptorSize);
		m_AllocatedSamplerDescriptors.insert(samplerDescriptorIndex);
		UpdateHighWaterMarks();
		return samplerDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeSamplerDescriptor(uint32_t index) {
		std::scoped_lock lock(m_DescriptorMutex);
		FreeDescriptor(index, m_AllocatedSamplerDescriptors, m_FreeSamplerDescriptors);
	}

	void VulkanDescriptorManager::FreeDescriptor(uint32_t index,
		std::unordered_set<uint32_t>& allocatedDescriptors,
		std::vector<uint32_t>& freeDescriptors) {
		if (index == UINT32_MAX) {
			return;
		}
		if (!allocatedDescriptors.erase(index)) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Invalid or duplicate descriptor free.");
			return;
		}
		freeDescriptors.push_back(index);
	}

	const char* VulkanDescriptorManager::RetainDebugName(const char* name) {
		if (!m_Device->SupportsDebugLabels() || name == nullptr || name[0] == '\0') {
			return nullptr;
		}

		return m_DescriptorDebugNames.emplace(name).first->c_str();
	}

	void VulkanDescriptorManager::UpdateHighWaterMarks() {
		const uint64_t bytes = m_AllocatedBufferDescriptors.size() * m_BufferDescriptorSize +
			m_AllocatedImageDescriptors.size() * m_ImageDescriptorSize;
		m_PeakResourceBytes = std::max(m_PeakResourceBytes, bytes);
		m_PeakSamplerCount = std::max(m_PeakSamplerCount, static_cast<uint64_t>(m_AllocatedSamplerDescriptors.size()));
	}

	DescriptorStatistics VulkanDescriptorManager::GetStatistics() const {
		std::scoped_lock lock(m_DescriptorMutex);
		DescriptorStatistics statistics{
			.bufferCount = m_AllocatedBufferDescriptors.size(),
			.imageCount = m_AllocatedImageDescriptors.size(),
			.samplerCount = m_AllocatedSamplerDescriptors.size(),
			.resourceBytes = m_AllocatedBufferDescriptors.size() * m_BufferDescriptorSize +
				m_AllocatedImageDescriptors.size() * m_ImageDescriptorSize,
			.resourceReservedBytes = m_NextBufferDescriptorIndex * m_BufferDescriptorSize +
				(m_ImageDescriptorCapacity - m_NextImageDescriptorIndex) * m_ImageDescriptorSize,
			.peakResourceBytes = m_PeakResourceBytes,
			.peakSamplerCount = m_PeakSamplerCount,
		};
		if (m_DescriptorHeaps) {
			statistics.resourceCapacityBytes = m_DescriptorHeaps->GetResourceHeap().usableSize;
			statistics.samplerCapacity = m_DescriptorHeaps->GetSamplerHeap().usableSize / m_SamplerDescriptorSize;
		}
		return statistics;
	}
}
