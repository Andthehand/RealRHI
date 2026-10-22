#include "VulkanDescriptorManager.h"

#include "VulkanBuffer.h"
#include "VulkanConvertions.h"
#include "VulkanTextureView.h"

namespace RealRHI {
	VulkanDescriptorManager::VulkanDescriptorManager(const VulkanDevice* device) 
		: m_Device(device) {}

	Result VulkanDescriptorManager::Init() {
		// Initialize descriptor heaps
		m_DescriptorHeaps = std::make_unique<VulkanDescriptorHeaps>(m_Device);
		if (m_DescriptorHeaps->Init(m_Device->GetDescriptorHeapProperties()) != Result::Success) {
			return Result::Failed;
		}

		// Get descriptor sizes from the heap properties
		const VkPhysicalDeviceDescriptorHeapPropertiesEXT& heapProperties = m_DescriptorHeaps->GetHeapProperties();
		m_BufferDescriptorSize = heapProperties.bufferDescriptorSize;
		m_ImageDescriptorSize = heapProperties.imageDescriptorSize;
		m_SamplerDescriptorSize = heapProperties.samplerDescriptorSize;

		// Initialize descriptor indices
		m_NextBufferDescriptorIndex = 0;
		// For image descriptors, we start from the end of the heap and decrement
		m_ImageDescriptorCapacity = static_cast<uint32_t>(m_DescriptorHeaps->GetResourceHeap().heapSize / m_ImageDescriptorSize);
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
		// Check if there are any free buffer descriptors available
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
			.data = {
				.pAddressRange = &deviceAddressRange
			}
		};

		std::vector<uint8_t> descriptorMemory(m_BufferDescriptorSize);
		VkHostAddressRangeEXT hostAddressRange{
			.address = descriptorMemory.data(),
			.size = m_BufferDescriptorSize,
		};

		// Write the descriptor to descriptorMemory
		if (vkWriteResourceDescriptorsEXT(
			m_Device->GetDevice(),
			1,
			&descriptorInfo,
			&hostAddressRange
		) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to write Vulkan buffer descriptor.");
			m_FreeBufferDescriptors.push_back(bufferDescriptorIndex);

			return UINT32_MAX;
		}

		// Upload descriptorMemory to the m_DescriptorHeaps at the correct offset
		m_DescriptorHeaps->GetResourceHeap().buffer->WriteData(descriptorMemory.data(), m_BufferDescriptorSize, bufferDescriptorIndex * m_BufferDescriptorSize);
		m_AllocatedBufferDescriptors.insert(bufferDescriptorIndex);
		return bufferDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeBufferDescriptor(uint32_t index) {
		std::scoped_lock lock(m_DescriptorMutex);
		if (index == UINT32_MAX) {
			return;
		}
		
		// Prevent double free or invalid index by checking if the index exists in the allocated set
		if (!m_AllocatedBufferDescriptors.erase(index)) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Invalid or duplicate descriptor free.");
			return;
		}

		m_FreeBufferDescriptors.push_back(index);
	}

	uint32_t VulkanDescriptorManager::AllocateImageDescriptor(const VulkanTextureView* textureView, VkDescriptorType descriptorType, VkImageLayout descriptorAccessLayout) {
        std::scoped_lock lock(m_DescriptorMutex);
		// Check if there are any free image descriptors available
		uint32_t imageDescriptorIndex;
		if (!m_FreeImageDescriptors.empty()) {
			imageDescriptorIndex = m_FreeImageDescriptors.back();
			m_FreeImageDescriptors.pop_back();
		}
		else {
			if (m_NextImageDescriptorIndex == 0 ||
				static_cast<VkDeviceSize>(m_NextImageDescriptorIndex - 1) * m_ImageDescriptorSize <
				static_cast<VkDeviceSize>(m_NextBufferDescriptorIndex) * m_BufferDescriptorSize) {
				return UINT32_MAX;
			}
			imageDescriptorIndex = --m_NextImageDescriptorIndex;
		}

		VkImageDescriptorInfoEXT imageDescriptorInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
			.pView = &textureView->GetImageViewCreateInfo(),
			.layout = descriptorAccessLayout,
		};

		VkResourceDescriptorInfoEXT descriptorInfo{
			.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
			.type = descriptorType,
			.data = {
				.pImage = &imageDescriptorInfo,
			}
		};

		std::vector<uint8_t> descriptorMemory(m_ImageDescriptorSize);
		VkHostAddressRangeEXT hostAddressRange{
			.address = descriptorMemory.data(),
			.size = m_ImageDescriptorSize,
		};

		// Write the descriptor to descriptorMemory
		if (vkWriteResourceDescriptorsEXT(
			m_Device->GetDevice(),
			1,
			&descriptorInfo,
			&hostAddressRange
		) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to write Vulkan image descriptor.");
			m_FreeImageDescriptors.push_back(imageDescriptorIndex);

			return UINT32_MAX;
		}

		if (m_DescriptorHeaps->GetResourceHeap().buffer->WriteData(descriptorMemory.data(), m_ImageDescriptorSize, imageDescriptorIndex * m_ImageDescriptorSize) != Result::Success) {
			m_FreeImageDescriptors.push_back(imageDescriptorIndex);

			return UINT32_MAX;
		}
		m_AllocatedImageDescriptors.insert(imageDescriptorIndex);
		return imageDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeImageDescriptor(uint32_t index) {
		std::scoped_lock lock(m_DescriptorMutex);
		if (index == UINT32_MAX) {
			return;
		}

		// Prevent double free or invalid index by checking if the index exists in the allocated set
		if (!m_AllocatedImageDescriptors.erase(index)) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Invalid or duplicate descriptor free.");
			return;
		}

		m_FreeImageDescriptors.push_back(index);
	}

	uint32_t VulkanDescriptorManager::AllocateSamplerDescriptor(const VkSamplerCreateInfo& samplerCI) {
        std::scoped_lock lock(m_DescriptorMutex);
		// Check if there are any free sampler descriptors available
		uint32_t samplerDescriptorIndex;
		if (!m_FreeSamplerDescriptors.empty()) {
			samplerDescriptorIndex = m_FreeSamplerDescriptors.back();
			m_FreeSamplerDescriptors.pop_back();
		}
		else {
			if ((static_cast<VkDeviceSize>(m_NextSamplerDescriptorIndex) + 1) * m_SamplerDescriptorSize >
				m_DescriptorHeaps->GetSamplerHeap().heapSize) {
				return UINT32_MAX;
			}
			samplerDescriptorIndex = m_NextSamplerDescriptorIndex++;
		}

		std::vector<uint8_t> descriptorMemory(m_SamplerDescriptorSize);
		VkHostAddressRangeEXT hostAddressRange{
			.address = descriptorMemory.data(),
			.size = m_SamplerDescriptorSize,
		};

		if (vkWriteSamplerDescriptorsEXT(
			m_Device->GetDevice(),
			1,
			&samplerCI,
			&hostAddressRange
		) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to write Vulkan sampler descriptor.");
			m_FreeSamplerDescriptors.push_back(samplerDescriptorIndex);

			return UINT32_MAX;
		}

		m_DescriptorHeaps->GetSamplerHeap().buffer->WriteData(descriptorMemory.data(), m_SamplerDescriptorSize, samplerDescriptorIndex * m_SamplerDescriptorSize);
		m_AllocatedSamplerDescriptors.insert(samplerDescriptorIndex);
		return samplerDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeSamplerDescriptor(uint32_t index) {
		std::scoped_lock lock(m_DescriptorMutex);
		if (index == UINT32_MAX) {
			return;
		}

		// Prevent double free or invalid index by checking if the index exists in the allocated set
		if (!m_AllocatedSamplerDescriptors.erase(index)) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Invalid or duplicate descriptor free.");
			return;
		}

		m_FreeSamplerDescriptors.push_back(index);
	}

};
