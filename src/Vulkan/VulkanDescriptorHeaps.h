#pragma once
#include "VulkanBuffer.h"
#include "VulkanDevice.h"

namespace RealRHI {
	struct DescriptorHeap {
		Ref<VulkanBuffer> buffer;
		VkDeviceAddress heapAddress;
		VkDeviceSize heapSize;
	};

	class VulkanDescriptorHeaps {
	public:
		VulkanDescriptorHeaps(const VulkanDevice* device);
		~VulkanDescriptorHeaps() = default;

		Result Init(VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProperties);

		void BindDescriptorHeaps(VkCommandBuffer commandBuffer) const;
	private:
		Result CreateDescriptorHeaps();
		Result CreateDescriptorHeap(VkDeviceSize hardwareMaxHeapSize, VkDeviceSize preferredMaxHeapSize, VkDeviceSize descriptorSize, VkDeviceSize heapAlignment, DescriptorHeap& heap);
	private:
		static constexpr uint32_t MaxResourceDescriptorHeapSize = 1024 * 1024; // 1 million descriptors, adjust as needed
		static constexpr uint32_t MaxSamplerDescriptorHeapSize = 1024; // 1 thousand descriptors, adjust as needed

		const VulkanDevice* m_Device = nullptr;

		VkPhysicalDeviceDescriptorHeapPropertiesEXT m_HeapProperties;
		DescriptorHeap m_DescriptorHeapResources;
		DescriptorHeap m_DescriptorHeapSamplers;
	};
}
