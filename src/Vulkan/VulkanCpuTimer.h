#pragma once
#include "VulkanDevice.h"

#include <chrono>

namespace RealRHI {
	class VulkanCpuTimer {
	public:
		explicit VulkanCpuTimer(const VulkanDevice* device)
			: m_Device(device) {
			if (m_Device->IsProfilingEnabled()) {
				m_Start = std::chrono::steady_clock::now();
			}
		}

		void Finish(VulkanProfiledOperation operation, uint64_t bytes = 0) const {
			if (m_Device->IsProfilingEnabled()) {
				const auto elapsed = std::chrono::steady_clock::now() - m_Start;
				m_Device->RecordCpuTiming(operation, std::chrono::duration<double, std::milli>(elapsed).count(), bytes);
			}
		}

	private:
		const VulkanDevice* m_Device;
		std::chrono::steady_clock::time_point m_Start;
	};
}
