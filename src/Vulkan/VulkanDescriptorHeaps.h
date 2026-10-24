#pragma once
#include "VulkanBuffer.h"
#include "VulkanDevice.h"

namespace RealRHI {
	struct DescriptorHeap {
		Ref<VulkanBuffer> buffer;
		VkDeviceAddress heapAddress = 0;
		VkDeviceSize heapSize = 0;
		VkDeviceSize usableSize = 0;
	};

	class VulkanDescriptorHeaps {
	public:
		VulkanDescriptorHeaps(const VulkanDevice* device);
		~VulkanDescriptorHeaps() = default;

		Result Init(VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProperties);

		const VkPhysicalDeviceDescriptorHeapPropertiesEXT& GetHeapProperties() const { return m_HeapProperties; }
		const DescriptorHeap& GetResourceHeap() const { return m_DescriptorHeapResources; }
		const DescriptorHeap& GetSamplerHeap() const { return m_DescriptorHeapSamplers; }
		DescriptorHeap& GetResourceHeap() { return m_DescriptorHeapResources; }
		DescriptorHeap& GetSamplerHeap() { return m_DescriptorHeapSamplers; }

		void BindDescriptorHeaps(VkCommandBuffer commandBuffer) const;

	private:
		Result CreateDescriptorHeaps();
		Result CreateDescriptorHeap(VkDeviceSize hardwareMaxHeapSize,
			VkDeviceSize preferredDescriptorCount,
			VkDeviceSize descriptorSize,
			VkDeviceSize heapAlignment,
			VkDeviceSize reservedSize,
			VkDeviceSize descriptorAlignment,
			DescriptorHeap& heap);

	private:
		static constexpr uint32_t s_MaxResourceDescriptorCount = 1024 * 1024;
		static constexpr uint32_t s_MaxSamplerDescriptorCount = 1024;

		const VulkanDevice* m_Device = nullptr;

		VkPhysicalDeviceDescriptorHeapPropertiesEXT m_HeapProperties{};
		DescriptorHeap m_DescriptorHeapResources;
		DescriptorHeap m_DescriptorHeapSamplers;
	};
}
