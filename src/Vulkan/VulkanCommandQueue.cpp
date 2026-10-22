#include "VulkanCommandQueue.h"

#include "VulkanCommandList.h"
#include "VulkanDevice.h"

#include <unordered_set>

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
		if (m_Queue != VK_NULL_HANDLE) {
			vkQueueWaitIdle(m_Queue);
		}

		if (m_TimelineSemaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(m_Device->GetDevice(), m_TimelineSemaphore, nullptr);
		}

		m_PendingSubmissions.clear();
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

		std::vector<VkCommandBufferSubmitInfo> commandBufferInfos;
		commandBufferInfos.reserve(submitInfo.commandLists.size());

		std::vector<CommandList*> trackedLists;
		trackedLists.reserve(submitInfo.commandLists.size());

		std::unordered_set<CommandList*> duplicateListTracker;
		duplicateListTracker.reserve(submitInfo.commandLists.size());

		for (CommandList* commandList : submitInfo.commandLists) {
			auto* vkCommandList = dynamic_cast<VulkanCommandList*>(commandList);
			if (vkCommandList == nullptr || vkCommandList->GetQueueId() != GetQueueId()) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Command list was created for a different queue family or index.");
				return {};
			}

			if (commandList->GetState() != CommandListState::Executable) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Cannot submit a command list that is not in the Executable state.");
				return {};
			}

			if (!duplicateListTracker.insert(commandList).second) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Duplicate command lists are not allowed in a single submission.");
				return {};
			}

			trackedLists.push_back(commandList);
			commandBufferInfos.push_back({
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
				.commandBuffer = vkCommandList->GetCommandBuffer(),
			});
		}

		std::vector<VkSemaphoreSubmitInfo> waitSemaphoreInfos;
		waitSemaphoreInfos.reserve(submitInfo.waitFor.size());
		for (const SubmissionToken& waitToken : submitInfo.waitFor) {
			if (!waitToken.IsValid()) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Invalid submission token for queue wait.");
				return {};
			}

			VulkanCommandQueue* queuedDependency = m_Device->GetCommandQueue(waitToken.queue);
			if (queuedDependency == nullptr) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Cannot wait on a token from a different device or unknown queue.");
				return {};
			}

			waitSemaphoreInfos.push_back({
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = queuedDependency->GetTimelineSemaphore(),
				.value = waitToken.value,
				.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			});
		}

		return SubmitTracked(commandBufferInfos, waitSemaphoreInfos, {}, trackedLists);
	}

	SubmissionToken VulkanCommandQueue::SubmitTracked(
		std::span<const VkCommandBufferSubmitInfo> commandBuffers,
		std::span<const VkSemaphoreSubmitInfo> waitSemaphores,
		std::span<const VkSemaphoreSubmitInfo> signalSemaphores,
		std::span<CommandList*> trackedLists,
		VkFence fence) {
		// Retains references to the command lists until the submission is completed then releases them
		std::vector<Ref<CommandList>> retainedLists;
		retainedLists.reserve(trackedLists.size());

		std::vector<VkSemaphoreSubmitInfo> signalInfos(signalSemaphores.begin(), signalSemaphores.end());

		std::unordered_set<CommandList*> duplicateListTracker;
		duplicateListTracker.reserve(trackedLists.size());
		for (CommandList* commandList : trackedLists) {
			auto* vkCommandList = dynamic_cast<VulkanCommandList*>(commandList);
			if (vkCommandList == nullptr || vkCommandList->GetQueueId() != GetQueueId()) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Tried to submit a command list to the wrong queue.");
				return {};
			}

			if (commandList->GetState() != CommandListState::Executable) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Tracked command list was not executable before submission.");
				return {};
			}

			if (!duplicateListTracker.insert(commandList).second) {
				m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Duplicate command lists are not allowed in a single submission.");
				return {};
			}

			retainedLists.emplace_back(commandList);
		}

		std::unique_lock submitLock(m_SubmissionMutex);
        // Check the complete batch before submitting. No global state changes
        // are published unless vkQueueSubmit2 succeeds.
        std::scoped_lock stateLock(m_Device->GetStateTrackingMutex());
		VulkanCommandList::LayoutMap predictedLayouts;
        for (CommandList* commandList : trackedLists) {
            if (!static_cast<VulkanCommandList*>(commandList)->ValidateSubmissionLayouts(predictedLayouts)) {
                m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General,
                    "Submission rejected: texture layout differs from the recorded initial layout.");
                return {};
            }
        }
		const uint64_t submissionValue = m_NextFrameValue;
		signalInfos.push_back({
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = m_TimelineSemaphore,
			.value = submissionValue,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		});

		VkSubmitInfo2 submitInfo2{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.waitSemaphoreInfoCount = static_cast<uint32_t>(waitSemaphores.size()),
			.pWaitSemaphoreInfos = waitSemaphores.empty() ? nullptr : waitSemaphores.data(),
			.commandBufferInfoCount = static_cast<uint32_t>(commandBuffers.size()),
			.pCommandBufferInfos = commandBuffers.empty() ? nullptr : commandBuffers.data(),
			.signalSemaphoreInfoCount = static_cast<uint32_t>(signalInfos.size()),
			.pSignalSemaphoreInfos = signalInfos.empty() ? nullptr : signalInfos.data(),
		};

		if (vkQueueSubmit2(m_Queue, 1, &submitInfo2, fence) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to submit Vulkan command buffers to the queue.");
			// Caller owns recovery of a failed frame fence.
			return {};
		}

		for (CommandList* commandList : trackedLists) {
			static_cast<VulkanCommandList*>(commandList)->CommitSubmissionLayouts();
		}
		++m_NextFrameValue;
		m_PendingSubmissions[submissionValue] = std::move(retainedLists);

		for (CommandList* commandList : trackedLists) {
			if (commandList != nullptr) {
				static_cast<VulkanCommandList*>(commandList)->SetState(CommandListState::Pending);
			}
		}

		submitLock.unlock();

		return SubmissionToken{ .queue = GetQueueId(), .value = submissionValue };
	}

	bool VulkanCommandQueue::IsComplete(const SubmissionToken& token) const {
		if (!token.IsValid() || token.queue != GetQueueId()) {
			return false;
		}

		{
			std::scoped_lock lock(m_SubmissionMutex);
			if (token.value >= m_NextFrameValue) {
				return false;
			}
		}
		uint64_t completed = 0;
		if (vkGetSemaphoreCounterValue(m_Device->GetDevice(), m_TimelineSemaphore, &completed) != VK_SUCCESS) {
			return false;
		}

		return completed >= token.value;
	}

	void VulkanCommandQueue::ProcessCompletedSubmissions() {
		uint64_t completed = 0;
		if (vkGetSemaphoreCounterValue(m_Device->GetDevice(), m_TimelineSemaphore, &completed) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to query queue timeline completion.");
			return;
		}

		FinalizeCompleted(completed);
	}

	void VulkanCommandQueue::Wait(const SubmissionToken& token) {
		if (!token.IsValid() || token.queue != GetQueueId()) {
			return;
		}

		{
			std::scoped_lock lock(m_SubmissionMutex);
			// If the token value is greater than or equal to the next frame value, 
			// it means that the submission has not yet been made, so we can return early.
			if (token.value >= m_NextFrameValue) {
				return;
			}
		}

		const uint64_t value = token.value;
		VkSemaphoreWaitInfo waitInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.semaphoreCount = 1,
			.pSemaphores = &m_TimelineSemaphore,
			.pValues = &value,
		};
		if (vkWaitSemaphores(m_Device->GetDevice(), &waitInfo, UINT64_MAX) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to wait for queue timeline completion.");
			return;
		}

		FinalizeCompleted(value);
	}

	void VulkanCommandQueue::WaitIdle() {
		uint64_t lastSubmitted = 0;
		{
			std::scoped_lock lock(m_SubmissionMutex);
			vkQueueWaitIdle(m_Queue);
			lastSubmitted = m_NextFrameValue - 1;
		}

		if (lastSubmitted != 0) {
			FinalizeCompleted(lastSubmitted);
		}
	}

	void VulkanCommandQueue::FinalizeCompleted(uint64_t completedValue) {
		std::vector<Ref<CommandList>> completedLists;
		{
			std::scoped_lock lock(m_SubmissionMutex);

			// Move completed command lists to a separate vector and remove them from the pending submissions
			auto it = m_PendingSubmissions.begin();
			while (it != m_PendingSubmissions.end()) {
				if (it->first <= completedValue) {
					// make_move_iterator is used to move the Ref<CommandList> objects instead of copying them,
					// which is more efficient and avoids unnecessary reference count increments/decrements.
					completedLists.insert(
						completedLists.end(),
						std::make_move_iterator(it->second.begin()),
						std::make_move_iterator(it->second.end()));

					it = m_PendingSubmissions.erase(it);
				} else {
					++it;
				}
			}

			if (completedValue > m_LastCompletedValue) {
				m_LastCompletedValue = completedValue;
			}
		}

		// Update the state of the completed command lists to Executable
		for (auto& commandList : completedLists) {
			if (commandList) {
				auto* vkCommandList = dynamic_cast<VulkanCommandList*>(commandList.Raw());

				if (vkCommandList != nullptr && vkCommandList->GetState() == CommandListState::Pending) {
					vkCommandList->SetState(CommandListState::Executable);
				}
			}
		}
	}
}
