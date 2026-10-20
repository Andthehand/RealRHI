#include "VulkanCommandQueue.h"

#include "VulkanCommandList.h"
#include "VulkanDevice.h"

namespace RealRHI {
    VulkanCommandQueue::VulkanCommandQueue(const VulkanDevice* device, uint32_t familyIndex, uint32_t queueIndex, VkQueue queue)
        : m_Device(device), m_Queue(queue), m_FamilyIndex(familyIndex), m_QueueIndex(queueIndex) {
        VkSemaphoreTypeCreateInfo timelineInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };

        VkSemaphoreCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timelineInfo,
        };

        if (vkCreateSemaphore(m_Device->GetDevice(), &createInfo, nullptr, &m_TimelineSemaphore) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan timeline semaphore for command queue.");
        }
    }

    VulkanCommandQueue::~VulkanCommandQueue() {
        if (m_TimelineSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_Device->GetDevice(), m_TimelineSemaphore, nullptr);
        }
    }

    SubmissionToken VulkanCommandQueue::Submit(CommandList& cmd) {
        CommandList* commandLists[] = { &cmd };
        return Submit(std::span<CommandList*>(commandLists, 1));
    }

    SubmissionToken VulkanCommandQueue::Submit(std::span<CommandList*> commandLists) {
        return Submit(SubmitInfo{ .commandLists = commandLists, .waitFor = {} });
    }

    SubmissionToken VulkanCommandQueue::Submit(const SubmitInfo& submitInfo) {
        if (submitInfo.commandLists.empty()) {
            return {};
        }

        std::vector<CommandList*> pendingLists;
        pendingLists.reserve(submitInfo.commandLists.size());
        std::vector<VkCommandBufferSubmitInfo> commandBufferInfos;
        commandBufferInfos.reserve(submitInfo.commandLists.size());

        for (CommandList* commandList : submitInfo.commandLists) {
            auto* vkCommandList = dynamic_cast<VulkanCommandList*>(commandList);
            if (commandList->GetState() != CommandListState::Executable) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Cannot submit a command list that is not in the Executable state.");
                return {};
            }

            if (vkCommandList->GetQueueId() != GetQueueId()) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Cannot submit a command list to a queue that does not match the command list's queue.");
                return {};
            }

            pendingLists.push_back(commandList);
            commandBufferInfos.push_back({
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = vkCommandList->GetCommandBuffer(),
            });
        }

        std::vector<VkSemaphoreSubmitInfo> waitSemaphoreInfos;
        waitSemaphoreInfos.reserve(submitInfo.waitFor.size());

        for (const SubmissionToken& waitToken : submitInfo.waitFor) {
            VulkanCommandQueue* queuedDependency = m_Device->GetCommandQueue(waitToken.queue);
            if (queuedDependency == nullptr) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Cannot submit a command list with a wait token that does not correspond to a valid queue.");
                return {};
            }

            waitSemaphoreInfos.push_back({
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = queuedDependency->GetTimelineSemaphore(),
                .value = waitToken.value,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            });
        }

        uint64_t submissionValue = 0;
        {
            std::scoped_lock lock(m_SubmissionMutex);
            submissionValue = m_NextValue++;
        }

        VkSemaphoreSubmitInfo timelineSignalInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = m_TimelineSemaphore,
            .value = submissionValue,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };

        VkSubmitInfo2 submitInfo2{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = static_cast<uint32_t>(waitSemaphoreInfos.size()),
            .pWaitSemaphoreInfos = waitSemaphoreInfos.empty() ? nullptr : waitSemaphoreInfos.data(),
            .commandBufferInfoCount = static_cast<uint32_t>(commandBufferInfos.size()),
            .pCommandBufferInfos = commandBufferInfos.empty() ? nullptr : commandBufferInfos.data(),
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos = &timelineSignalInfo,
        };

        if (vkQueueSubmit2(m_Queue, 1, &submitInfo2, VK_NULL_HANDLE) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to submit Vulkan command buffers to the queue.");
            return {};
        }

        {
            std::scoped_lock lock(m_SubmissionMutex);
            m_PendingSubmissions[submissionValue] = pendingLists;
            for (CommandList* commandList : pendingLists) {
                static_cast<VulkanCommandList*>(commandList)->SetState(CommandListState::Pending);
            }
        }

        return SubmissionToken{ .queue = GetQueueId(), .value = submissionValue };
    }

    bool VulkanCommandQueue::IsComplete(const SubmissionToken& token) const {
        if (token.queue != GetQueueId()) {
            return false;
        }

        uint64_t currentValue = 0;
        const VkResult result = vkGetSemaphoreCounterValue(m_Device->GetDevice(), m_TimelineSemaphore, &currentValue);
        if (result != VK_SUCCESS) {
            return false;
        }

        if (currentValue < token.value) {
            return false;
        }

        FinalizeCompleted(token.value);
        return true;
    }

    void VulkanCommandQueue::Wait(const SubmissionToken& token) {
        if (token.queue != GetQueueId()) {
            return;
        }

        const uint64_t value = token.value;
        VkSemaphoreWaitInfo waitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &m_TimelineSemaphore,
            .pValues = &value,
        };

        vkWaitSemaphores(m_Device->GetDevice(), &waitInfo, UINT64_MAX);
        FinalizeCompleted(value);
    }

    void VulkanCommandQueue::WaitIdle() {
        vkQueueWaitIdle(m_Queue);
        if (m_NextValue > 1) {
            FinalizeCompleted(m_NextValue - 1);
        }
    }

    void VulkanCommandQueue::FinalizeCompleted(uint64_t completedValue) const {
        std::scoped_lock lock(m_SubmissionMutex);
        auto it = m_PendingSubmissions.begin();
        while (it != m_PendingSubmissions.end()) {
            if (it->first <= completedValue) {
                for (CommandList* commandList : it->second) {
                    if (commandList != nullptr && commandList->GetState() == CommandListState::Pending) {
                        static_cast<VulkanCommandList*>(commandList)->SetState(CommandListState::Executable);
                    }
                }
                it = m_PendingSubmissions.erase(it);
            } else {
                ++it;
            }
        }
        if (completedValue > m_LastCompletedValue) {
            m_LastCompletedValue = completedValue;
        }
    }
}
