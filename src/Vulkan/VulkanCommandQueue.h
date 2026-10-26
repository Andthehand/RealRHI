#pragma once
#include "CommandList.h"
#include "Queue.h"
#include "Swapchain.h"

#include <volk.h>

#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace RealRHI {
	class VulkanDevice;
	class VulkanCommandList;

	class VulkanCommandQueue : public CommandQueue {
	public:
		~VulkanCommandQueue() override;

		static Result Create(const VulkanDevice* device,
			uint32_t familyIndex,
			uint32_t queueIndex,
			VkQueue queue,
			std::unique_ptr<VulkanCommandQueue>& outQueue);

		using CommandQueue::Submit;
		SubmissionToken Submit(const SubmitInfo& submitInfo) override;

		bool IsComplete(const SubmissionToken& token) override;
		void Wait(const SubmissionToken& token) override;
		void WaitIdle() override;
		void ProcessCompletedSubmissions();

		VkQueue GetQueue() const { return m_Queue; }
		VkSemaphore GetTimelineSemaphore() const { return m_TimelineSemaphore; }
		QueueId GetQueueId() const override { return {m_FamilyIndex, m_QueueIndex}; }

	private:
		struct PendingSubmission {
			uint64_t value = 0;
			Ref<Swapchain> swapchain;
			std::vector<Ref<CommandList>> commandLists;
		};

	private:
		friend class VulkanDevice;
		// Validate, retain, and submit the same lists. Legacy frame submission can
		// bracket them with swapchain-owned layout transition buffers.
		SubmissionToken SubmitTracked(std::span<CommandList* const> trackedLists,
			std::span<const VkSemaphoreSubmitInfo> waitSemaphores,
			std::span<const VkSemaphoreSubmitInfo> signalSemaphores,
			std::span<const SubmissionToken> waitFor = {},
			Swapchain* retainedSwapchain = nullptr,
			VkCommandBuffer preTransition = VK_NULL_HANDLE,
			VkCommandBuffer postTransition = VK_NULL_HANDLE);

		VulkanCommandQueue(const VulkanDevice* device, uint32_t familyIndex, uint32_t queueIndex, VkQueue queue);
		Result Init();
		void FinalizeCompleted(uint64_t completedValue);
		bool HasSubmitted(const SubmissionToken& token) const;

	private:
		const VulkanDevice* m_Device = nullptr;
		VkQueue m_Queue = VK_NULL_HANDLE;
		VkSemaphore m_TimelineSemaphore = VK_NULL_HANDLE;
		uint32_t m_FamilyIndex = 0;
		uint32_t m_QueueIndex = 0;

		mutable std::mutex m_SubmissionMutex;
		uint64_t m_NextFrameValue = 1;
		std::deque<PendingSubmission> m_PendingSubmissions;
	};
}
