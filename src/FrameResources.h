#pragma once

#include "Device.h"
#include "Queue.h"

#include <cstdint>
#include <vector>

namespace RealRHI {
    // Reusable execution slots; independent of swapchain image indices.
    // Device and queue must outlive this object. Not thread-safe: callers
    // must serialize access to a given slot.
    class FrameResources {
    public:
        FrameResources() = default;
        FrameResources(const FrameResources&) = delete;
        FrameResources& operator=(const FrameResources&) = delete;
        ~FrameResources() { WaitAll(); }

        Result Initialize(Device& device, QueueType queueType, uint32_t slotCount) {
            if (m_Queue != nullptr || slotCount == 0) {
                return Result::Failed;
            }

            CommandQueue* queue = device.GetCommandQueue(queueType);
            if (queue == nullptr) {
                return Result::InvalidParameter;
            }

            std::vector<Slot> slots;
            slots.reserve(slotCount);
            for (uint32_t i = 0; i < slotCount; ++i) {
                Slot slot;
                if (device.CreateCommandAllocator(queueType, slot.allocator) != Result::Success) {
                    return Result::Failed;
                }
                if (device.CreateCommandList(slot.allocator.Raw(), slot.commandList) != Result::Success) {
                    return Result::Failed;
                }
                slots.push_back(std::move(slot));
            }
            m_Slots = std::move(slots);
            m_Queue = queue;
            return Result::Success;
        }

        uint32_t GetSlotCount() const {
            return static_cast<uint32_t>(m_Slots.size());
        }

        // Wait for the previous use, finalize its command-list state, and
        // reset the existing command buffer for a new recording.
        Result BeginSlot(uint32_t index, CommandList*& out) {
            out = nullptr;
            if (index >= m_Slots.size() || m_Queue == nullptr) {
                return Result::Failed;
            }
            Slot& slot = m_Slots[index];
            if (slot.recordingAvailable == false) {
                return Result::Failed;
            }
            if (slot.completion.IsValid()) {
                m_Queue->Wait(slot.completion);
                if (!m_Queue->IsComplete(slot.completion)) {
                    return Result::Failed;
                }
                slot.completion = {};
            }
            if (slot.commandList->Reset() != Result::Success) {
                return Result::Failed;
            }
            slot.recordingAvailable = false;
            out = slot.commandList.Raw();
            return Result::Success;
        }

        // Track a successful submission of the slot's command list.
        // The token must come from this object's queue.
        Result TrackSubmission(uint32_t index, SubmissionToken token) {
            if (index >= m_Slots.size() || m_Queue == nullptr ||
                !token.IsValid() || token.queue != m_Queue->GetQueueId() || m_Slots[index].recordingAvailable) {
                return Result::Failed;
            }
            Slot& slot = m_Slots[index];
            if (slot.completion.IsValid()) {
                return Result::Failed;
            }
            slot.completion = token;
            slot.recordingAvailable = true;
            return Result::Success;
        }

        // If recording was not submitted, release the slot for another attempt.
        // Caller must end/reset any partially recorded command list first.
        Result CancelSlot(uint32_t index) {
            if (index >= m_Slots.size() || m_Slots[index].recordingAvailable) {
                return Result::Failed;
            }
            Slot& slot = m_Slots[index];
            if (slot.commandList->GetState() == CommandListState::Recording ||
                slot.commandList->GetState() == CommandListState::Pending) {
                return Result::Failed;
            }
            slot.recordingAvailable = true;
            return Result::Success;
        }

        void WaitAll() {
            if (m_Queue == nullptr) {
                return;
            }
            for (Slot& slot : m_Slots) {
                if (!slot.completion.IsValid()) {
                    continue;
                }
                m_Queue->Wait(slot.completion);
                if (m_Queue->IsComplete(slot.completion)) {
                    slot.completion = {};
                }
            }
        }

    private:
        struct Slot {
            Ref<CommandAllocator> allocator;
            Ref<CommandList> commandList;
            SubmissionToken completion{};
            bool recordingAvailable = true;
        };
        CommandQueue* m_Queue = nullptr;
        std::vector<Slot> m_Slots;
    };
}
