#pragma once

#include "Queue.h"
#include "CommandList.h"

#include <volk.h>

#include <mutex>
#include <unordered_map>
#include <vector>

namespace RealRHI {
    class VulkanDevice;
    class VulkanCommandList;

    class VulkanCommandQueue : public CommandQueue {
    public:
        VulkanCommandQueue(const VulkanDevice* device, uint32_t familyIndex, uint32_t queueIndex, VkQueue queue);
        ~VulkanCommandQueue() override;

        SubmissionToken Submit(CommandList& cmd) override;
        SubmissionToken Submit(std::span<CommandList*> commandLists) override;
        SubmissionToken Submit(const SubmitInfo& submitInfo) override;
        SubmissionToken SubmitTracked(
            std::span<const VkCommandBufferSubmitInfo> commandBuffers,
            std::span<const VkSemaphoreSubmitInfo> waitSemaphores,
            std::span<const VkSemaphoreSubmitInfo> signalSemaphores,
            std::span<CommandList*> trackedLists = {},
            VkFence fence = VK_NULL_HANDLE);
        bool IsComplete(const SubmissionToken& token) const override;
        void Wait(const SubmissionToken& token) override;
        void WaitIdle() override;

        VkQueue GetQueue() const { return m_Queue; }
        VkSemaphore GetTimelineSemaphore() const { return m_TimelineSemaphore; }
        QueueId GetQueueId() const { return { m_FamilyIndex, m_QueueIndex }; }
    private:
        void FinalizeCompleted(uint64_t completedValue) const;

        const VulkanDevice* m_Device = nullptr;
        VkQueue m_Queue = VK_NULL_HANDLE;
        VkSemaphore m_TimelineSemaphore = VK_NULL_HANDLE;
        uint32_t m_FamilyIndex = 0;
        uint32_t m_QueueIndex = 0;

		// mutable because we need to allow const methods to check completion status and finalize completed command lists
        mutable std::mutex m_SubmissionMutex;
        uint64_t m_NextFrameValue = 1;
        mutable std::unordered_map<uint64_t, std::vector<Ref<CommandList>>> m_PendingSubmissions;
        mutable uint64_t m_LastCompletedValue = 0;
    };
}
