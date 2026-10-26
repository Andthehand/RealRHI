#pragma once
#include "FrameContext.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace RealRHI {
	class CommandList;
	class Swapchain;

	enum class QueueType {
		Graphics,
		Compute,
		Transfer
	};

	struct QueueId {
		uint32_t familyIndex = 0;
		uint32_t queueIndex = 0;

	public:
		bool operator==(const QueueId& other) const = default;
	};

	struct SubmissionToken {
		QueueId queue{};
		uint64_t value = 0;

	public:
		constexpr bool IsValid() const noexcept { return value != 0; }
		bool operator==(const SubmissionToken& other) const = default;
	};

	// Frame endpoints are serialized with BeginFrame/Present. Queue submissions retain
	// the swapchain through completion; final synchronization must join all frame work.
	struct SwapchainSubmitSync {
		Swapchain* swapchain = nullptr;
		FrameContext frame;
		bool waitForAcquire = false;
		bool signalForPresent = false;
	};

	struct SubmitInfo {
		std::span<CommandList* const> commandLists{};
		// Acquiring ownership from another family requires its release submission token
		// (or a later token on that queue). A CPU Wait does not replace this dependency.
		std::span<const SubmissionToken> waitFor{};
		// Endpoint synchronization only; command lists must record image layout/ownership barriers.
		std::optional<SwapchainSubmitSync> swapchain;
	};

	class CommandQueue {
	public:
		virtual ~CommandQueue() = default;

		// Backends implement SubmitInfo once; convenience forms share that path.
		virtual SubmissionToken Submit(CommandList& cmd) {
			CommandList* commandLists[] = {&cmd};
			return Submit(SubmitInfo{.commandLists = commandLists});
		}
		virtual SubmissionToken Submit(std::span<CommandList* const> commandLists) {
			return Submit(SubmitInfo{.commandLists = commandLists});
		}
		virtual SubmissionToken Submit(const SubmitInfo& submitInfo) = 0;
		virtual QueueId GetQueueId() const = 0;
		// A completed token also retires its command lists so they can be reset.
		virtual bool IsComplete(const SubmissionToken& token) = 0;
		virtual void Wait(const SubmissionToken& token) = 0;
		virtual void WaitIdle() = 0;
	};
}

namespace std {
	template <>
	struct hash<RealRHI::QueueId> {
		std::size_t operator()(const RealRHI::QueueId& value) const noexcept {
			const std::size_t h1 = std::hash<uint32_t>{}(value.familyIndex);
			const std::size_t h2 = std::hash<uint32_t>{}(value.queueIndex);
			return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2));
		}
	};

	template <>
	struct hash<RealRHI::SubmissionToken> {
		std::size_t operator()(const RealRHI::SubmissionToken& value) const noexcept {
			const std::size_t h1 = std::hash<RealRHI::QueueId>{}(value.queue);
			const std::size_t h2 = std::hash<uint64_t>{}(value.value);
			return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
		}
	};
}
