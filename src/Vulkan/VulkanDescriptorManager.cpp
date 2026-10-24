#include "VulkanDescriptorManager.h"

#include "VulkanBuffer.h"
#include "VulkanConvertions.h"

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

	uint32_t VulkanDescriptorManager::AllocateBufferDescriptor(const VulkanBuffer* buffer) {
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
		return bufferDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeBufferDescriptor(uint32_t index) {
		std::scoped_lock lock(m_DescriptorMutex);
		if (index == UINT32_MAX) {
			return;
		}

		if (!m_AllocatedBufferDescriptors.erase(index)) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Invalid or duplicate descriptor free.");
			return;
		}

		m_FreeBufferDescriptors.push_back(index);
	}

	uint32_t VulkanDescriptorManager::AllocateImageDescriptor(const VkImageViewCreateInfo& viewInfo,
		VkDescriptorType descriptorType,
		VkImageLayout descriptorAccessLayout) {
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
		return imageDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeImageDescriptor(uint32_t index) {
		std::scoped_lock lock(m_DescriptorMutex);
		if (index == UINT32_MAX) {
			return;
		}

		if (!m_AllocatedImageDescriptors.erase(index)) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Invalid or duplicate descriptor free.");
			return;
		}

		m_FreeImageDescriptors.push_back(index);
	}

	uint32_t VulkanDescriptorManager::AllocateSamplerDescriptor(const VkSamplerCreateInfo& samplerCI) {
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

		if (vkWriteSamplerDescriptorsEXT(m_Device->GetDevice(), 1, &samplerCI, &hostAddressRange) != VK_SUCCESS) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to write Vulkan sampler descriptor.");
			m_FreeSamplerDescriptors.push_back(samplerDescriptorIndex);

			return UINT32_MAX;
		}

		m_DescriptorHeaps->GetSamplerHeap().buffer->WriteData(
			descriptorMemory.data(), m_SamplerDescriptorSize, samplerDescriptorIndex * m_SamplerDescriptorSize);
		m_AllocatedSamplerDescriptors.insert(samplerDescriptorIndex);
		return samplerDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeSamplerDescriptor(uint32_t index) {
		std::scoped_lock lock(m_DescriptorMutex);
		if (index == UINT32_MAX) {
			return;
		}

		if (!m_AllocatedSamplerDescriptors.erase(index)) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Invalid or duplicate descriptor free.");
			return;
		}

		m_FreeSamplerDescriptors.push_back(index);
	}

};
