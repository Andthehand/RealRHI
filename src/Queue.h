#pragma once

#include <cstdint>
#include <functional>
#include <span>

namespace RealRHI {
    class CommandList;

    enum class QueueType {
        Graphics,
        Compute,
        Transfer
    };

    struct QueueId {
        uint32_t familyIndex = 0;
        uint32_t queueIndex = 0;


        bool operator==(const QueueId& other) const = default;
    };

    struct SubmissionToken {
        QueueId queue{};
        uint64_t value = 0;

		constexpr bool IsValid() const noexcept { return value != 0; }
        bool operator==(const SubmissionToken& other) const = default;
    };

    struct SubmitInfo {
        std::span<CommandList*> commandLists{};
        std::span<const SubmissionToken> waitFor{};
    };

    class CommandQueue {
    public:
        virtual ~CommandQueue() = default;

        virtual SubmissionToken Submit(CommandList& cmd) = 0;
        virtual SubmissionToken Submit(std::span<CommandList*> commandLists) = 0;
        virtual SubmissionToken Submit(const SubmitInfo& submitInfo) = 0;
        virtual bool IsComplete(const SubmissionToken& token) const = 0;
        virtual void Wait(const SubmissionToken& token) = 0;
        virtual void WaitIdle() = 0;
    };
}

namespace std {
    template<>
    struct hash<RealRHI::QueueId> {
        std::size_t operator()(const RealRHI::QueueId& value) const noexcept {
            const std::size_t h1 = std::hash<uint32_t>{}(value.familyIndex);
            const std::size_t h2 = std::hash<uint32_t>{}(value.queueIndex);
            return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2));
        }
    };

    template<>
    struct hash<RealRHI::SubmissionToken> {
        std::size_t operator()(const RealRHI::SubmissionToken& value) const noexcept {
            const std::size_t h1 = std::hash<RealRHI::QueueId>{}(value.queue);
            const std::size_t h2 = std::hash<uint64_t>{}(value.value);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }
    };
}
