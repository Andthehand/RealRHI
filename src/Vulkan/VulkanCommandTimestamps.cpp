#include "VulkanCommandTimestamps.h"

#include <array>

namespace RealRHI {

	VulkanCommandTimestamps::VulkanCommandTimestamps(const VulkanDevice* device, QueueId queue)
		: m_Device(device), m_QueueId(queue) { }

	VulkanCommandTimestamps::~VulkanCommandTimestamps() {
		if (m_TimestampPool != VK_NULL_HANDLE) {
			vkDestroyQueryPool(m_Device->GetDevice(), m_TimestampPool, nullptr);
		}
	}

	void VulkanCommandTimestamps::OnSubmitted() {
		m_TimestampResults.clear();
		m_TimestampReadResult = Result::NotReady;
	}

	void VulkanCommandTimestamps::OnCompleted() {
		m_TimestampReadResult = ReadTimestamps();
		if (m_TimestampReadResult == Result::Success) {
			RecordTimestampTimings();
		}
	}

	void VulkanCommandTimestamps::Reset() {
		m_TimestampNames.clear();
		m_TimestampIntervals.clear();
		m_ReservedTimestamps = 0;
		OnSubmitted();
	}

	Result VulkanCommandTimestamps::GetResults(std::vector<GpuTimestamp>& outTimestamps) const {
		if (m_TimestampReadResult != Result::Success) {
			return m_TimestampReadResult;
		}
		outTimestamps = m_TimestampResults;
		return Result::Success;
	}

	Result VulkanCommandTimestamps::Init() {
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

	void VulkanCommandTimestamps::WriteTimestamp(VkCommandBuffer commands, const char* name) {
		if (!IsSupported()) {
			return;
		}
		if (name == nullptr || name[0] == '\0' ||
			m_TimestampNames.size() + m_ReservedTimestamps >= s_MaxTimestampCount) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"WriteTimestamp: Requires recording, a nonempty name, and fewer than 128 recorded markers.");
			return;
		}

		const uint32_t query = static_cast<uint32_t>(m_TimestampNames.size());
		m_TimestampNames.emplace_back(name);
		vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_TimestampPool, query);
	}

	uint32_t VulkanCommandTimestamps::BeginGpuTiming(VkCommandBuffer commands, const char* name) {
		if (!IsSupported()) {
			return UINT32_MAX;
		}
		if (!name || !name[0]) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"BeginGpuTiming: Requires an active recording and a nonempty name.");
			return UINT32_MAX;
		}
		if (m_TimestampNames.size() + m_ReservedTimestamps + 2 > s_MaxTimestampCount) {
			return UINT32_MAX;
		}
		const auto timing = static_cast<uint32_t>(m_TimestampIntervals.size());
		m_TimestampIntervals.push_back({static_cast<uint32_t>(m_TimestampNames.size())});
		WriteTimestamp(commands, name);
		++m_ReservedTimestamps;
		return timing;
	}

	void VulkanCommandTimestamps::EndGpuTiming(VkCommandBuffer commands, uint32_t timing) {
		if (timing == UINT32_MAX) {
			return;
		}
		if (timing >= m_TimestampIntervals.size() || m_TimestampIntervals[timing].end != UINT32_MAX) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"EndGpuTiming: Requires an active recording and an open interval from this recording.");
			return;
		}
		--m_ReservedTimestamps;
		m_TimestampIntervals[timing].end = static_cast<uint32_t>(m_TimestampNames.size());
		WriteTimestamp(commands, "GPU interval end");
	}

	void VulkanCommandTimestamps::RecordTimestampTimings() const {
		std::vector<bool> paired(m_TimestampResults.size());
		for (const auto& interval : m_TimestampIntervals) {
			paired[interval.begin] = true;
			if (interval.end == UINT32_MAX) {
				continue;
			}
			paired[interval.end] = true;
			const std::array<GpuTimestamp, 2> endpoints{
				m_TimestampResults[interval.begin], m_TimestampResults[interval.end]
			};
			m_Device->RecordGpuTimings(m_QueueId, endpoints);
		}
		// Keep the existing adjacent-marker contract for explicit WriteTimestamp calls.
		std::vector<GpuTimestamp> markers;
		for (size_t i = 0; i < m_TimestampResults.size(); ++i) {
			if (!paired[i]) {
				markers.push_back(m_TimestampResults[i]);
			}
		}
		m_Device->RecordGpuTimings(m_QueueId, markers);
	}

	void VulkanCommandTimestamps::PrepareSubmission() {
		if (!m_TimestampNames.empty()) {
			// Command-buffer query resets are unavailable on transfer-only queues. Reset on the host
			// immediately before every submission, including replays of an executable recording.
			vkResetQueryPool(m_Device->GetDevice(), m_TimestampPool, 0, static_cast<uint32_t>(m_TimestampNames.size()));
		}
	}

	Result VulkanCommandTimestamps::ReadTimestamps() {
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
}
