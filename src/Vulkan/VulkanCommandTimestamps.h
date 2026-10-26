#pragma once
#include "Profiling.h"
#include "Queue.h"
#include "Result.h"
#include "VulkanDevice.h"

#include <volk.h>

#include <string>
#include <vector>

namespace RealRHI {
	// The command list owns synchronization and validates recording lifecycle.
	class VulkanCommandTimestamps {
	private:
		struct Interval {
			uint32_t begin;
			uint32_t end = UINT32_MAX;
		};

	public:
		VulkanCommandTimestamps(const VulkanDevice* device, QueueId queue);
		~VulkanCommandTimestamps();
		VulkanCommandTimestamps(const VulkanCommandTimestamps&) = delete;
		VulkanCommandTimestamps& operator=(const VulkanCommandTimestamps&) = delete;

		Result Init();
		bool IsSupported() const { return m_TimestampPool != VK_NULL_HANDLE; }
		void WriteTimestamp(VkCommandBuffer commands, const char* name);
		uint32_t BeginGpuTiming(VkCommandBuffer commands, const char* name);
		void EndGpuTiming(VkCommandBuffer commands, uint32_t timing);
		void PrepareSubmission();
		void OnSubmitted();
		void OnCompleted();
		void Reset();
		Result GetResults(std::vector<GpuTimestamp>& outTimestamps) const;

	private:
		Result ReadTimestamps();
		void RecordTimestampTimings() const;

	private:
		const VulkanDevice* m_Device;
		QueueId m_QueueId;
		static constexpr uint32_t s_MaxTimestampCount = 128;
		VkQueryPool m_TimestampPool = VK_NULL_HANDLE;
		uint32_t m_TimestampValidBits = 0;
		float m_TimestampPeriod = 0;
		std::vector<std::string> m_TimestampNames;
		std::vector<Interval> m_TimestampIntervals;
		uint32_t m_ReservedTimestamps = 0;
		std::vector<GpuTimestamp> m_TimestampResults;
		Result m_TimestampReadResult = Result::NotReady;
	};
}
