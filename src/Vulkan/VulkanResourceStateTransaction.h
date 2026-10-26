#pragma once
#include "VulkanDevice.h"
#include "VulkanResourceOperation.h"

#include <volk.h>

#include <span>
#include <unordered_map>
#include <vector>

namespace RealRHI {
	// One transaction spans every command list in a queue batch. The queue holds
	// the device state-tracking lock from validation through successful native submit
	// and Commit. A failed validation or native submit discards this transaction.
	class VulkanResourceStateTransaction {
	public:
		VulkanResourceStateTransaction(const VulkanDevice* device,
			SubmissionToken submission,
			std::span<const VkSemaphoreSubmitInfo> waits);
		VulkanResourceStateTransaction(const VulkanResourceStateTransaction&) = delete;
		VulkanResourceStateTransaction& operator=(const VulkanResourceStateTransaction&) = delete;

		bool Validate(std::span<const VulkanResourceOperation> operations);
		void Commit();

	private:
		bool ValidateOwnership(VulkanQueueOwnership& ownership, const VulkanResourceOperation& operation) const;

	private:
		const VulkanDevice* m_Device;
		SubmissionToken m_Submission;
		// Borrowed from the enclosing queue submission.
		std::span<const VkSemaphoreSubmitInfo> m_Waits;
		std::unordered_map<VulkanSubresourceKey, VulkanTextureState, VulkanSubresourceKeyHash> m_Textures;
		std::unordered_map<VulkanBuffer*, std::vector<VulkanBufferRangeState>> m_Buffers;
	};
}
