#include "VulkanDescriptorManager.h"
#include "VulkanDevice.h"

#include <algorithm>

namespace RealRHI {
	namespace {
		ResourceStatistics& SelectResource(DeviceStatistics& statistics, VulkanProfiledResource type) {
			switch (type) {
				case VulkanProfiledResource::Buffer:
					return statistics.buffers;
				case VulkanProfiledResource::Texture:
					return statistics.textures;
				case VulkanProfiledResource::Pipeline:
					return statistics.pipelines;
			}
			return statistics.buffers;
		}

		void AddTiming(TimingStatistics& timing, double milliseconds) {
			++timing.sampleCount;
			timing.totalMilliseconds += milliseconds;
			timing.maximumMilliseconds = std::max(timing.maximumMilliseconds, milliseconds);
		}
	}

	DeviceStatistics VulkanDevice::GetStatistics() const {
		DeviceStatistics snapshot;
		{
			std::scoped_lock lock(m_StatisticsMutex);
			snapshot = m_Statistics;
		}

		if (m_DescriptorManager) {
			snapshot.descriptors = m_DescriptorManager->GetStatistics();
		}
		
		return snapshot;
	}

	void VulkanDevice::RecordResourceCreated(VulkanProfiledResource type, uint64_t bytes) const {
		std::scoped_lock lock(m_StatisticsMutex);
		auto& usage = SelectResource(m_Statistics, type);
		++usage.liveCount;
		usage.liveBytes += bytes;
		usage.peakCount = std::max(usage.peakCount, usage.liveCount);
		usage.peakBytes = std::max(usage.peakBytes, usage.liveBytes);
	}

	void VulkanDevice::RecordResourceDestroyed(VulkanProfiledResource type, uint64_t bytes) const {
		std::scoped_lock lock(m_StatisticsMutex);
		auto& usage = SelectResource(m_Statistics, type);
		--usage.liveCount;
		usage.liveBytes -= bytes;
	}

	void VulkanDevice::RecordCpuTiming(VulkanProfiledOperation operation, double milliseconds, uint64_t bytes) const {
		std::scoped_lock lock(m_StatisticsMutex);
		switch (operation) {
			case VulkanProfiledOperation::UploadStaging:
				AddTiming(m_Statistics.uploadStaging, milliseconds);
				m_Statistics.stagedUploadBytes += bytes;
				break;
			case VulkanProfiledOperation::Submission:
				AddTiming(m_Statistics.submissions, milliseconds);
				break;
			case VulkanProfiledOperation::GraphicsPipeline:
				AddTiming(m_Statistics.graphicsPipelineCreation, milliseconds);
				break;
			case VulkanProfiledOperation::ComputePipeline:
				AddTiming(m_Statistics.computePipelineCreation, milliseconds);
				break;
		}
	}

	void VulkanDevice::RecordUploadBatch(uint64_t bytes) const {
		if (!m_EnableProfiling) {
			return;
		}

		std::scoped_lock lock(m_StatisticsMutex);
		++m_Statistics.uploadBatchCount;
		m_Statistics.submittedUploadBytes += bytes;
	}

	void VulkanDevice::RecordGpuTimings(QueueId queue, std::span<const GpuTimestamp> timestamps) const {
		std::scoped_lock lock(m_StatisticsMutex);
		for (size_t index = 1; index < timestamps.size(); ++index) {
			const auto& begin = timestamps[index - 1];
			const double milliseconds = timestamps[index].milliseconds - begin.milliseconds;
			auto found =
				std::find_if(m_Statistics.gpuTimings.begin(), m_Statistics.gpuTimings.end(), [&](const auto& timing) {
					return timing.name == begin.name && timing.queue == queue;
				});
			if (found == m_Statistics.gpuTimings.end()) {
				m_Statistics.gpuTimings.push_back({.name = begin.name, .queue = queue});
				found = m_Statistics.gpuTimings.end() - 1;
			}

			AddTiming(found->timing, milliseconds);
		}
	}
}
