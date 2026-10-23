#pragma once

#include "Queue.h"
#include "CommandList.h"

#include <volk.h>

#include <mutex>
#include <memory>
#include <deque>
#include <vector>

namespace RealRHI {
    class VulkanDevice;
    class VulkanCommandList;

    class VulkanCommandQueue : public CommandQueue {
    public:
        ~VulkanCommandQueue() override;

        static Result Create(
            const VulkanDevice* device,
            uint32_t familyIndex,
            uint32_t queueIndex,
            VkQueue queue,
            std::unique_ptr<VulkanCommandQueue>& outQueue);

		// Command submission functions
        SubmissionToken Submit(CommandList& cmd) override;
        SubmissionToken Submit(std::span<CommandList*> commandLists) override;
        SubmissionToken Submit(const SubmitInfo& submitInfo) override;

		// Internal submission function that allows for tracking of command lists for lifecycle management.
        SubmissionToken SubmitTracked(
            std::span<const VkCommandBufferSubmitInfo> commandBuffers,
            std::span<const VkSemaphoreSubmitInfo> waitSemaphores,
            std::span<const VkSemaphoreSubmitInfo> signalSemaphores,
            std::span<CommandList*> trackedLists = {},
            VkFence fence = VK_NULL_HANDLE);

        bool IsComplete(const SubmissionToken& token) override;
        void Wait(const SubmissionToken& token) override;
        void WaitIdle() override;
        void ProcessCompletedSubmissions();

        VkQueue GetQueue() const { return m_Queue; }
        VkSemaphore GetTimelineSemaphore() const { return m_TimelineSemaphore; }
        QueueId GetQueueId() const override { return { m_FamilyIndex, m_QueueIndex }; }
    private:
        struct PendingSubmission {
            uint64_t value = 0;
            std::vector<Ref<CommandList>> commandLists;
        };
    private:
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
