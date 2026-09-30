#include "VulkanDescriptorHeaps.h"

#include <volk.h>
#include <algorithm>

namespace RealRHI {
	VulkanDescriptorHeaps::VulkanDescriptorHeaps(const VulkanDevice* device)
		: m_Device(device) {}

	Result RealRHI::VulkanDescriptorHeaps::Init(VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProperties) {
		m_HeapProperties = heapProperties;

		return CreateDescriptorHeaps();
	}

	void VulkanDescriptorHeaps::BindDescriptorHeaps(VkCommandBuffer commandBuffer) const {
		VkDeviceSize samplerReservedRangeSize = m_HeapProperties.minSamplerHeapReservedRange;
		VkBindHeapInfoEXT samplerBind{
			.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
			.heapRange = { m_DescriptorHeapSamplers.heapAddress, m_DescriptorHeapSamplers.heapSize },
			.reservedRangeOffset = m_DescriptorHeapSamplers.heapSize - samplerReservedRangeSize,
			.reservedRangeSize = samplerReservedRangeSize
		};
		vkCmdBindSamplerHeapEXT(commandBuffer, &samplerBind);

		VkDeviceSize resourceReservedRangeSize = m_HeapProperties.minResourceHeapReservedRange;
		VkBindHeapInfoEXT resourceBind{
			.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
			.heapRange = { m_DescriptorHeapResources.heapAddress, m_DescriptorHeapResources.heapSize },
			.reservedRangeOffset = m_DescriptorHeapResources.heapSize - resourceReservedRangeSize,
			.reservedRangeSize = resourceReservedRangeSize
		};
		vkCmdBindResourceHeapEXT(commandBuffer, &resourceBind);
	}

	Result VulkanDescriptorHeaps::CreateDescriptorHeaps() {
		// Create the resource descriptor heap
		VkDeviceSize resourceDescriptorSize = std::max(m_HeapProperties.bufferDescriptorSize, m_HeapProperties.imageDescriptorSize);
		Result resourceHeapResult = CreateDescriptorHeap(
			m_HeapProperties.maxResourceHeapSize,
			MaxResourceDescriptorHeapSize, 
			resourceDescriptorSize, 
			m_HeapProperties.resourceHeapAlignment, 
			m_DescriptorHeapResources
		);

		if (resourceHeapResult != Result::Success) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create resource descriptor heap.");
			return resourceHeapResult;
		}

		// Create the sampler descriptor heap
		Result samplerHeapResult = CreateDescriptorHeap(
			m_HeapProperties.maxSamplerHeapSize,
			MaxSamplerDescriptorHeapSize,
			m_HeapProperties.samplerDescriptorSize,
			m_HeapProperties.samplerHeapAlignment,
			m_DescriptorHeapSamplers
		);

		if (samplerHeapResult != Result::Success) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create sampler descriptor heap.");
			return samplerHeapResult;
		}

		return Result::Success;
	}

	Result VulkanDescriptorHeaps::CreateDescriptorHeap(VkDeviceSize hardwareMaxHeapSize, VkDeviceSize preferedMaxHeapSize, VkDeviceSize descriptorSize, VkDeviceSize heapAlignment, DescriptorHeap& heap) {
		// Either use the max descriptor size or the max heap size, whichever is smaller
		VkDeviceSize heapSize = std::min(preferedMaxHeapSize * descriptorSize, hardwareMaxHeapSize);

		// Align the heap size to the required alignment
		heapSize = ((heapSize + heapAlignment - 1) / heapAlignment) * heapAlignment;
		BufferDesc bufferDesc = {
			.size = heapSize,
			.usage = BufferUsage::Heap | BufferUsage::Addressable,
			.memoryUsage = MemoryUsage::CPUToGPU
		};

		Ref<Buffer> buffer;
		Result result = m_Device->CreateBuffer(bufferDesc, buffer);
		heap.buffer = std::move(buffer);

		if (result == Result::Success) {
			heap.heapAddress = heap.buffer->GetBufferDeviceAddress();
			heap.heapSize = heapSize;
		}

		return result;
	}
}
