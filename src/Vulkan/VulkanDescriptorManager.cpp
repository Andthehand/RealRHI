#include "VulkanDescriptorManager.h"

#include "VulkanConvertions.h"

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
		m_NextImageDescriptorIndex = (m_DescriptorHeaps->GetResourceHeap().heapSize - m_ImageDescriptorSize) / m_ImageDescriptorSize;
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
		// Check if there are any free buffer descriptors available
		uint32_t bufferDescriptorIndex;
		if (!m_FreeBufferDescriptors.empty()) {
			bufferDescriptorIndex = m_FreeBufferDescriptors.back();
			m_FreeBufferDescriptors.pop_back();
		}
		else {
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
		VkResult result = vkWriteResourceDescriptorsEXT(
			m_Device->GetDevice(),
			1,
			&descriptorInfo,
			&hostAddressRange
		);

		// Upload descriptorMemory to the m_DescriptorHeaps at the correct offset
		m_DescriptorHeaps->GetResourceHeap().buffer->WriteData(descriptorMemory.data(), m_BufferDescriptorSize, bufferDescriptorIndex * m_BufferDescriptorSize);
		return bufferDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeBufferDescriptor(uint32_t index) {
		m_FreeBufferDescriptors.push_back(index);
	}

	uint32_t VulkanDescriptorManager::AllocateImageDescriptor(const VulkanTexture* texture) {
		// Check if there are any free image descriptors available
		uint32_t imageDescriptorIndex;
		if (!m_FreeImageDescriptors.empty()) {
			imageDescriptorIndex = m_FreeImageDescriptors.back();
			m_FreeImageDescriptors.pop_back();
		}
		else {
			imageDescriptorIndex = m_NextImageDescriptorIndex--;
		}

		const auto* textureView = static_cast<const VulkanTextureView*>(texture->GetTextureView());
		VkImageDescriptorInfoEXT imageDescriptorInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
			.pView = &textureView->GetImageViewCreateInfo(),
			.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};

		VkResourceDescriptorInfoEXT descriptorInfo{
			.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
			.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
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
		VkResult result = vkWriteResourceDescriptorsEXT(
			m_Device->GetDevice(),
			1,
			&descriptorInfo,
			&hostAddressRange
		);

		// Upload descriptorMemory to the m_DescriptorHeaps at the correct offset
		m_DescriptorHeaps->GetResourceHeap().buffer->WriteData(descriptorMemory.data(), m_ImageDescriptorSize, imageDescriptorIndex * m_ImageDescriptorSize);
		return imageDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeImageDescriptor(uint32_t index) {
		m_FreeImageDescriptors.push_back(index);
	}

	uint32_t VulkanDescriptorManager::AllocateSamplerDescriptor(const VkSamplerCreateInfo& samplerCI) {
		// Check if there are any free sampler descriptors available
		uint32_t samplerDescriptorIndex;
		if (!m_FreeSamplerDescriptors.empty()) {
			samplerDescriptorIndex = m_FreeSamplerDescriptors.back();
			m_FreeSamplerDescriptors.pop_back();
		}
		else {
			samplerDescriptorIndex = m_NextSamplerDescriptorIndex++;
		}

		std::vector<uint8_t> descriptorMemory(m_SamplerDescriptorSize);
		VkHostAddressRangeEXT hostAddressRange{
			.address = descriptorMemory.data(),
			.size = m_SamplerDescriptorSize,
		};

		vkWriteSamplerDescriptorsEXT(
			m_Device->GetDevice(),
			1,
			&samplerCI,
			&hostAddressRange
		);

		m_DescriptorHeaps->GetSamplerHeap().buffer->WriteData(descriptorMemory.data(), m_SamplerDescriptorSize, samplerDescriptorIndex * m_SamplerDescriptorSize);
		return samplerDescriptorIndex;
	}

	void VulkanDescriptorManager::FreeSamplerDescriptor(uint32_t index) {
		m_FreeSamplerDescriptors.push_back(index);
	}
};
