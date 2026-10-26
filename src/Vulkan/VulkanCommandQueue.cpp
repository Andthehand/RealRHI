#include "VulkanCommandQueue.h"

#include "VulkanCommandList.h"
#include "VulkanCpuTimer.h"
#include "VulkanDevice.h"
#include "VulkanSwapchain.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace RealRHI {
	VulkanCommandQueue::VulkanCommandQueue(const VulkanDevice* device,
		uint32_t familyIndex,
		uint32_t queueIndex,
		VkQueue queue)
		: m_Device(device), m_Queue(queue), m_FamilyIndex(familyIndex), m_QueueIndex(queueIndex) { }

	Result VulkanCommandQueue::Create(const VulkanDevice* device,
		uint32_t familyIndex,
		uint32_t queueIndex,
		VkQueue queue,
		std::unique_ptr<VulkanCommandQueue>& outQueue) {
		std::unique_ptr<VulkanCommandQueue> commandQueue(
			new VulkanCommandQueue(device, familyIndex, queueIndex, queue));
		const Result result = commandQueue->Init();
		if (result != Result::Success) {
			return result;
		}

		outQueue = std::move(commandQueue);
		return Result::Success;
	}

	Result VulkanCommandQueue::Init() {
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
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Failed to create Vulkan timeline semaphore for command queue.");
			return Result::Failed;
		}

		return Result::Success;
	}

	VulkanCommandQueue::~VulkanCommandQueue() {
		if (m_Queue != VK_NULL_HANDLE && m_TimelineSemaphore != VK_NULL_HANDLE) {
			WaitIdle();
		}

		if (m_TimelineSemaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(m_Device->GetDevice(), m_TimelineSemaphore, nullptr);
		}

		m_PendingSubmissions.clear();
	}

	SubmissionToken VulkanCommandQueue::Submit(CommandList& cmd) {
		CommandList* commandLists[] = {&cmd};
		return Submit(std::span<CommandList*>(commandLists, 1));
	}

	SubmissionToken VulkanCommandQueue::Submit(std::span<CommandList*> commandLists) {
		return Submit(SubmitInfo{.commandLists = commandLists, .waitFor = {}});
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
			auto* vkCommandList = static_cast<VulkanCommandList*>(commandList);
			if (vkCommandList == nullptr || vkCommandList->GetQueueId() != GetQueueId()) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Command list was created for a different queue family or index.");
				return {};
			}

			if (commandList->GetState() != CommandListState::Executable) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Cannot submit a command list that is not in the Executable state.");
				return {};
			}

			if (!duplicateListTracker.insert(commandList).second) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Duplicate command lists are not allowed in a single submission.");
				return {};
			}

			trackedLists.push_back(commandList);
			commandBufferInfos.push_back({
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
				.commandBuffer = vkCommandList->GetCommandBuffer(),
			});
		}

		if (!submitInfo.swapchain) {
			return SubmitTracked(commandBufferInfos, {}, {}, trackedLists, VK_NULL_HANDLE, submitInfo.waitFor);
		}
		const auto& request = *submitInfo.swapchain;
		auto* swapchain = static_cast<VulkanSwapchain*>(request.swapchain);
		if (!swapchain || !swapchain->CanSubmit(request)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Submit: Swapchain endpoint requires a current frame and an unconsumed acquire or pending "
				"presentation.");
			return {};
		}
		std::vector<VkSemaphoreSubmitInfo> waits;
		std::vector<VkSemaphoreSubmitInfo> signals;
		if (request.waitForAcquire) {
			waits.push_back(
				{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = swapchain->GetImageAvailableSemaphore(request.frame.frameIndex),
					.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
		}
		if (request.signalForPresent) {
			signals.push_back(
				{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					.semaphore = swapchain->GetRenderFinishedSemaphore(request.frame.imageIndex),
					.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
		}
		const auto token = SubmitTracked(
			commandBufferInfos, waits, signals, trackedLists, VK_NULL_HANDLE, submitInfo.waitFor, swapchain);
		swapchain->FinishSubmission(request, token);
		return token;
	}

	SubmissionToken VulkanCommandQueue::SubmitTracked(std::span<const VkCommandBufferSubmitInfo> commandBuffers,
		std::span<const VkSemaphoreSubmitInfo> waitSemaphores,
		std::span<const VkSemaphoreSubmitInfo> signalSemaphores,
		std::span<CommandList*> trackedLists,
		VkFence fence,
		std::span<const SubmissionToken> waitFor,
		Swapchain* retainedSwapchain) {
		const VulkanCpuTimer timer(m_Device);

		std::vector<VkSemaphoreSubmitInfo> waitSemaphoreInfos(waitSemaphores.begin(), waitSemaphores.end());
		std::unordered_map<VulkanCommandQueue*, uint64_t> dependencyValues;
		for (const SubmissionToken& waitToken : waitFor) {
			if (!waitToken.IsValid()) {
				m_Device->SendDebugMessage(
					DebugSeverity::Error, DebugMessageType::General, "Invalid submission token for queue wait.");
				return {};
			}

			VulkanCommandQueue* queuedDependency = m_Device->GetCommandQueue(waitToken.queue);
			if (queuedDependency == nullptr || !queuedDependency->HasSubmitted(waitToken)) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Submit: Queue wait token must identify an existing submission on a known queue.");
				return {};
			}
			dependencyValues[queuedDependency] = std::max(dependencyValues[queuedDependency], waitToken.value);
		}
		for (const auto& [dependency, value] : dependencyValues) {
			waitSemaphoreInfos.push_back({
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = dependency->GetTimelineSemaphore(),
				.value = value,
				.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			});
		}

		std::vector<Ref<CommandList>> retainedLists;
		retainedLists.reserve(trackedLists.size());

		std::vector<VkSemaphoreSubmitInfo> signalInfos(signalSemaphores.begin(), signalSemaphores.end());

		std::unordered_set<CommandList*> duplicateListTracker;
		duplicateListTracker.reserve(trackedLists.size());
		for (CommandList* commandList : trackedLists) {
			auto* vkCommandList = static_cast<VulkanCommandList*>(commandList);
			if (vkCommandList == nullptr || vkCommandList->GetQueueId() != GetQueueId()) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Tried to submit a command list to the wrong queue.");
				return {};
			}

			if (commandList->GetState() != CommandListState::Executable) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Tracked command list was not executable before submission.");
				return {};
			}

			if (!duplicateListTracker.insert(commandList).second) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Duplicate command lists are not allowed in a single submission.");
				return {};
			}

			retainedLists.emplace_back(commandList);
		}

		std::unique_lock submitLock(m_SubmissionMutex);
		// Check the complete batch before submitting. No global state changes
		// are published unless vkQueueSubmit2 succeeds.
		std::scoped_lock stateLock(m_Device->GetStateTrackingMutex());
		const uint64_t submissionValue = m_NextFrameValue;
		const SubmissionToken submission{.queue = GetQueueId(), .value = submissionValue};
		VulkanCommandList::SubmissionState predicted;
		for (CommandList* commandList : trackedLists) {
			if (!static_cast<VulkanCommandList*>(commandList)
					->ValidateSubmissionState(predicted, submission, waitSemaphoreInfos)) {
				return {};
			}
		}
		signalInfos.push_back({
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = m_TimelineSemaphore,
			.value = submissionValue,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		});

		VkSubmitInfo2 submitInfo2{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.waitSemaphoreInfoCount = static_cast<uint32_t>(waitSemaphoreInfos.size()),
			.pWaitSemaphoreInfos = waitSemaphoreInfos.data(),
			.commandBufferInfoCount = static_cast<uint32_t>(commandBuffers.size()),
			.pCommandBufferInfos = commandBuffers.data(),
			.signalSemaphoreInfoCount = static_cast<uint32_t>(signalInfos.size()),
			.pSignalSemaphoreInfos = signalInfos.data(),
		};

		if (m_Device->IsProfilingEnabled()) {
			for (CommandList* commandList : trackedLists) {
				static_cast<VulkanCommandList*>(commandList)->PrepareTimestampSubmission();
			}
		}

		if (vkQueueSubmit2(m_Queue, 1, &submitInfo2, fence) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Failed to submit Vulkan command buffers to the queue.");
			// Caller owns recovery of a failed frame fence.
			return {};
		}

		VulkanCommandList::CommitSubmissionState(predicted);
		++m_NextFrameValue;
		m_PendingSubmissions.push_back({
			.value = submissionValue,
			.swapchain = Ref<Swapchain>(retainedSwapchain),
			.commandLists = std::move(retainedLists),
		});

		for (CommandList* commandList : trackedLists) {
			static_cast<VulkanCommandList*>(commandList)->SetState(CommandListState::Pending);
		}

		submitLock.unlock();
		timer.Finish(VulkanProfiledOperation::Submission);

		return SubmissionToken{.queue = GetQueueId(), .value = submissionValue};
	}

	bool VulkanCommandQueue::IsComplete(const SubmissionToken& token) {
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

		FinalizeCompleted(completed);
		return completed >= token.value;
	}

	bool VulkanCommandQueue::HasSubmitted(const SubmissionToken& token) const {
		std::scoped_lock lock(m_SubmissionMutex);
		return token.IsValid() && token.queue == GetQueueId() && token.value < m_NextFrameValue;
	}

	void VulkanCommandQueue::ProcessCompletedSubmissions() {
		uint64_t completed = 0;
		if (vkGetSemaphoreCounterValue(m_Device->GetDevice(), m_TimelineSemaphore, &completed) != VK_SUCCESS) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to query queue timeline completion.");
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
			// Only values returned by a successful submission can be waited on.
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
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to wait for queue timeline completion.");
			return;
		}

		FinalizeCompleted(value);
	}

	void VulkanCommandQueue::WaitIdle() {
		uint64_t lastSubmitted = 0;
		{
			std::scoped_lock lock(m_SubmissionMutex);
			if (vkQueueWaitIdle(m_Queue) != VK_SUCCESS) {
				m_Device->SendDebugMessage(
					DebugSeverity::Error, DebugMessageType::General, "WaitIdle: Failed to wait for the Vulkan queue.");
				return;
			}
			lastSubmitted = m_NextFrameValue - 1;
		}

		if (lastSubmitted != 0) {
			FinalizeCompleted(lastSubmitted);
		}
	}

	void VulkanCommandQueue::FinalizeCompleted(uint64_t completedValue) {
		// Release retained command lists after dropping the submission lock.
		std::vector<PendingSubmission> completedSubmissions;
		{
			std::scoped_lock lock(m_SubmissionMutex);

			while (!m_PendingSubmissions.empty() && m_PendingSubmissions.front().value <= completedValue) {
				auto& submission = m_PendingSubmissions.front();
				for (auto& commandList : submission.commandLists) {
					static_cast<VulkanCommandList*>(commandList.Raw())->SetState(CommandListState::Executable);
				}

				completedSubmissions.push_back(std::move(submission));
				m_PendingSubmissions.pop_front();
			}
		}
	}
}
