#include "VulkanCommandList.h"

namespace RealRHI {
	Result VulkanCommandList::InitTimestamps() {
		if (!m_Device->IsProfilingEnabled() || !m_Device->SupportsHostQueryReset()) {
			return Result::Success;
		}

		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(m_Device->GetPhysicalDevice(), &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(m_Device->GetPhysicalDevice(), &familyCount, families.data());
		m_TimestampValidBits = families[m_QueueId.familyIndex].timestampValidBits;
		if (m_TimestampValidBits == 0) {
			return Result::Success;
		}

		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(m_Device->GetPhysicalDevice(), &properties);
		m_TimestampPeriod = properties.limits.timestampPeriod;

		const VkQueryPoolCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_TIMESTAMP,
			.queryCount = s_MaxTimestampCount,
		};
		if (vkCreateQueryPool(m_Device->GetDevice(), &info, nullptr, &m_TimestampPool) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateCommandList: Failed to allocate the profiling timestamp query pool.");
			return Result::Failed;
		}
		return Result::Success;
	}

	void VulkanCommandList::WriteTimestamp(const char* name) {
		if (!SupportsTimestamps()) {
			return;
		}
		if (GetState() != CommandListState::Recording || name == nullptr || name[0] == '\0' ||
			m_TimestampNames.size() == s_MaxTimestampCount) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"WriteTimestamp: Requires recording, a nonempty name, and fewer than 128 recorded markers.");
			return;
		}

		const uint32_t query = static_cast<uint32_t>(m_TimestampNames.size());
		m_TimestampNames.emplace_back(name);
		vkCmdWriteTimestamp2(m_CommandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_TimestampPool, query);
	}

	void VulkanCommandList::PrepareTimestampSubmission() {
		std::scoped_lock lock(m_StateMutex);
		if (!m_TimestampNames.empty()) {
			// Command-buffer query resets are unavailable on transfer-only queues. Reset on the host
			// immediately before every submission, including replays of an executable recording.
			vkResetQueryPool(m_Device->GetDevice(), m_TimestampPool, 0, static_cast<uint32_t>(m_TimestampNames.size()));
		}
	}

	Result VulkanCommandList::ReadTimestamps() {
		if (m_TimestampNames.empty()) {
			return Result::Success;
		}

		std::vector<uint64_t> ticks(m_TimestampNames.size());
		const VkResult result = vkGetQueryPoolResults(m_Device->GetDevice(),
			m_TimestampPool,
			0,
			static_cast<uint32_t>(ticks.size()),
			ticks.size() * sizeof(uint64_t),
			ticks.data(),
			sizeof(uint64_t),
			VK_QUERY_RESULT_64_BIT);
		if (result == VK_NOT_READY) {
			return Result::NotReady;
		}
		if (result != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"GetTimestampResults: Failed to read completed GPU timestamp queries.");
			return Result::Failed;
		}

		uint64_t mask = UINT64_MAX;
		if (m_TimestampValidBits < 64) {
			mask = (uint64_t{1} << m_TimestampValidBits) - 1;
		}
		uint64_t elapsedTicks = 0;
		for (size_t index = 0; index < ticks.size(); ++index) {
			if (index != 0) {
				elapsedTicks += (ticks[index] - ticks[index - 1]) & mask;
			}
			m_TimestampResults.push_back({
				.name = m_TimestampNames[index],
				.milliseconds = static_cast<double>(elapsedTicks) * m_TimestampPeriod / 1'000'000.0,
			});
		}
		return Result::Success;
	}

	void VulkanCommandList::SetState(CommandListState state) {
		std::scoped_lock lock(m_StateMutex);
		if (state == CommandListState::Pending) {
			m_TimestampResults.clear();
			m_TimestampReadResult = Result::NotReady;
		}
		if (m_Device->IsProfilingEnabled() && m_State == CommandListState::Pending &&
			state == CommandListState::Executable) {
			// Collect before publishing Executable: another thread may immediately recycle this recording.
			m_TimestampReadResult = ReadTimestamps();
			if (m_TimestampReadResult == Result::Success) {
				m_Device->RecordGpuTimings(m_QueueId, m_TimestampResults);
			}
		}
		m_State = state;
	}

	Result VulkanCommandList::GetTimestampResults(std::vector<GpuTimestamp>& outTimestamps) const {
		std::scoped_lock lock(m_StateMutex);
		outTimestamps.clear();
		if (!SupportsTimestamps()) {
			return Result::Success;
		}
		if (m_State != CommandListState::Executable) {
			return Result::NotReady;
		}
		if (m_TimestampReadResult != Result::Success) {
			return m_TimestampReadResult;
		}
		outTimestamps = m_TimestampResults;
		return Result::Success;
	}

	bool VulkanCommandList::ValidateDebugLabel(const char* operation, const char* name) const {
		if (GetState() == CommandListState::Recording && name != nullptr && name[0] != '\0') {
			return true;
		}
		const std::string message = std::string(operation) + ": Requires recording and a nonempty label name.";
		m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, message.c_str());
		return false;
	}

	void VulkanCommandList::BeginDebugLabel(const char* name) {
		if (!m_Device->SupportsDebugLabels() || !ValidateDebugLabel("BeginDebugLabel", name)) {
			return;
		}
		const VkDebugUtilsLabelEXT label{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
			.pLabelName = name,
			.color = {0.2f, 0.6f, 0.9f, 1.0f},
		};
		vkCmdBeginDebugUtilsLabelEXT(m_CommandBuffer, &label);
	}

	void VulkanCommandList::EndDebugLabel() {
		if (!m_Device->SupportsDebugLabels()) {
			return;
		}
		if (GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"EndDebugLabel: Requires a recording command list and an outstanding label in queue submission order.");
			return;
		}
		vkCmdEndDebugUtilsLabelEXT(m_CommandBuffer);
	}

	void VulkanCommandList::InsertDebugLabel(const char* name) {
		if (!m_Device->SupportsDebugLabels() || !ValidateDebugLabel("InsertDebugLabel", name)) {
			return;
		}
		const VkDebugUtilsLabelEXT label{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
			.pLabelName = name,
			.color = {0.2f, 0.6f, 0.9f, 1.0f},
		};
		vkCmdInsertDebugUtilsLabelEXT(m_CommandBuffer, &label);
	}
}
