#include "VulkanDescriptorHeaps.h"

#include <volk.h>

#include <algorithm>
#include <limits>

namespace RealRHI {
	VulkanDescriptorHeaps::VulkanDescriptorHeaps(const VulkanDevice* device)
		: m_Device(device) { }

	Result RealRHI::VulkanDescriptorHeaps::Init(VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProperties) {
		m_HeapProperties = heapProperties;

		return CreateDescriptorHeaps();
	}

	void VulkanDescriptorHeaps::BindDescriptorHeaps(VkCommandBuffer commandBuffer) const {
		VkBindHeapInfoEXT samplerBind{
			.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
			.heapRange = {m_DescriptorHeapSamplers.heapAddress, m_DescriptorHeapSamplers.heapSize},
			.reservedRangeOffset = m_DescriptorHeapSamplers.usableSize,
			.reservedRangeSize = m_DescriptorHeapSamplers.heapSize - m_DescriptorHeapSamplers.usableSize,
		};
		vkCmdBindSamplerHeapEXT(commandBuffer, &samplerBind);

		VkBindHeapInfoEXT resourceBind{
			.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
			.heapRange = {m_DescriptorHeapResources.heapAddress, m_DescriptorHeapResources.heapSize},
			.reservedRangeOffset = m_DescriptorHeapResources.usableSize,
			.reservedRangeSize = m_DescriptorHeapResources.heapSize - m_DescriptorHeapResources.usableSize,
		};
		vkCmdBindResourceHeapEXT(commandBuffer, &resourceBind);
	}

	Result VulkanDescriptorHeaps::CreateDescriptorHeaps() {
		VkDeviceSize resourceDescriptorSize =
			std::max(m_HeapProperties.bufferDescriptorSize, m_HeapProperties.imageDescriptorSize);
		Result resourceHeapResult = CreateDescriptorHeap(m_HeapProperties.maxResourceHeapSize,
			s_MaxResourceDescriptorCount,
			resourceDescriptorSize,
			m_HeapProperties.resourceHeapAlignment,
			m_HeapProperties.minResourceHeapReservedRange,
			std::max(m_HeapProperties.bufferDescriptorAlignment, m_HeapProperties.imageDescriptorAlignment),
			m_DescriptorHeapResources);

		if (resourceHeapResult != Result::Success) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to create resource descriptor heap.");
			return resourceHeapResult;
		}

		Result samplerHeapResult = CreateDescriptorHeap(m_HeapProperties.maxSamplerHeapSize,
			s_MaxSamplerDescriptorCount,
			m_HeapProperties.samplerDescriptorSize,
			m_HeapProperties.samplerHeapAlignment,
			m_HeapProperties.minSamplerHeapReservedRange,
			m_HeapProperties.samplerDescriptorAlignment,
			m_DescriptorHeapSamplers);

		if (samplerHeapResult != Result::Success) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to create sampler descriptor heap.");
			return samplerHeapResult;
		}

		return Result::Success;
	}

	Result VulkanDescriptorHeaps::CreateDescriptorHeap(VkDeviceSize hardwareMaxHeapSize,
		VkDeviceSize preferredDescriptorCount,
		VkDeviceSize descriptorSize,
		VkDeviceSize heapAlignment,
		VkDeviceSize reservedSize,
		VkDeviceSize descriptorAlignment,
		DescriptorHeap& heap) {
		if (descriptorSize == 0 || heapAlignment == 0 || descriptorAlignment == 0 ||
			reservedSize >= hardwareMaxHeapSize ||
			preferredDescriptorCount > (std::numeric_limits<VkDeviceSize>::max() - reservedSize) / descriptorSize) {
			return Result::InvalidParameter;
		}

		const VkDeviceSize requestedSize = preferredDescriptorCount * descriptorSize + reservedSize;
		VkDeviceSize heapSize = std::min(requestedSize, hardwareMaxHeapSize);

		// Round up only when the result stays within the hardware limit.
		const VkDeviceSize padding = (heapAlignment - heapSize % heapAlignment) % heapAlignment;
		if (padding <= hardwareMaxHeapSize - heapSize) {
			heapSize += padding;
		}
		else {
			heapSize -= heapSize % heapAlignment;
		}

		if (heapSize <= reservedSize) {
			return Result::InvalidParameter;
		}

		const VkDeviceSize usableSize = (heapSize - reservedSize) / descriptorAlignment * descriptorAlignment;
		if (usableSize < descriptorSize) {
			return Result::InvalidParameter;
		}

		BufferDesc bufferDesc = {
			.size = heapSize,
			.usage = BufferUsage::Heap | BufferUsage::Addressable,
			.memoryUsage = MemoryUsage::CPUToGPU,
		};

		Ref<VulkanBuffer> buffer;
		Result result = VulkanBuffer::Create(m_Device, bufferDesc, buffer);
		heap.buffer = std::move(buffer);

		if (result == Result::Success) {
			heap.heapAddress = heap.buffer->GetBufferDeviceAddress();
			heap.heapSize = heapSize;
			heap.usableSize = usableSize;
		}

		return result;
	}
}
