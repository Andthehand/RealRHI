#include "Device.h"
#include "Queue.h"
#include "FrameResources.h"
#include "DeferredDeletionQueue.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanCommandQueue.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>

namespace {
    void Expect(bool condition, const char* message) {
        if (!condition) {
            std::cerr << "Test failed: " << message << '\n';
            std::exit(1);
        }
    }
}

int main() {
    RealRHI::DeviceDesc desc{};
    desc.applicationName = "RealRHI Submission Tests";
    desc.shaderDirectory = std::filesystem::current_path() / "assets" / "shaders";
    desc.enableValidationLayers = false;
    desc.enableDebug = false;

    std::unique_ptr<RealRHI::Device> device;
    const RealRHI::Result createResult = RealRHI::Device::Create(desc, device);
    Expect(createResult == RealRHI::Result::Success, "Device creation failed");

    auto* graphicsQueue = static_cast<RealRHI::VulkanDevice*>(device.get())->GetGraphicsCommandQueue();
    Expect(graphicsQueue != nullptr, "Graphics queue was not created");

    RealRHI::Ref<RealRHI::CommandAllocator> allocator;
    Expect(device->CreateCommandAllocator(allocator) == RealRHI::Result::Success, "Create command allocator");

    RealRHI::Ref<RealRHI::CommandList> firstList;
    Expect(device->CreateCommandList(allocator.Raw(), firstList) == RealRHI::Result::Success, "Create first command list");
    Expect(firstList->Begin() == RealRHI::Result::Success, "Begin first command list");
    Expect(firstList->End() == RealRHI::Result::Success, "End first command list");

    const RealRHI::SubmissionToken firstToken = graphicsQueue->Submit(*firstList);
    Expect(firstToken.value == 1, "First submission should start at timeline value 1");
    graphicsQueue->Wait(firstToken);
    Expect(graphicsQueue->IsComplete(firstToken), "First submission should be complete after waiting");
    Expect(firstList->GetState() == RealRHI::CommandListState::Executable, "Command list returned to executable state");

    RealRHI::Ref<RealRHI::CommandList> secondList;
    Expect(device->CreateCommandList(allocator.Raw(), secondList) == RealRHI::Result::Success, "Create second command list");
    Expect(secondList->Begin() == RealRHI::Result::Success, "Begin second command list");
    Expect(secondList->End() == RealRHI::Result::Success, "End second command list");

    const RealRHI::SubmissionToken secondToken = graphicsQueue->Submit(*secondList);
    Expect(secondToken.value == 2, "Second submission must increment timeline value");
    graphicsQueue->Wait(secondToken);
    Expect(graphicsQueue->IsComplete(secondToken), "Wait should complete a submission");
    Expect(secondList->Reset() == RealRHI::Result::Success, "Reset command list after completion");

    RealRHI::Ref<RealRHI::CommandList> listA;
    RealRHI::Ref<RealRHI::CommandList> listB;
    Expect(device->CreateCommandList(allocator.Raw(), listA) == RealRHI::Result::Success, "Create list A");
    Expect(device->CreateCommandList(allocator.Raw(), listB) == RealRHI::Result::Success, "Create list B");
    Expect(listA->Begin() == RealRHI::Result::Success, "Begin list A");
    Expect(listA->End() == RealRHI::Result::Success, "End list A");
    Expect(listB->Begin() == RealRHI::Result::Success, "Begin list B");
    Expect(listB->End() == RealRHI::Result::Success, "End list B");

    std::array<RealRHI::CommandList*, 2> combined = { listA.Raw(), listB.Raw() };
    const RealRHI::SubmissionToken combinedToken = graphicsQueue->Submit(combined);
    Expect(combinedToken.value == 3, "Combined submission should continue timeline monotonicity");
    graphicsQueue->Wait(combinedToken);
    Expect(listA->Reset() == RealRHI::Result::Success, "Reset list A after combined completion");
    Expect(listB->Reset() == RealRHI::Result::Success, "Reset list B after combined completion");

    // Two independent execution slots, reused only after their GPU tokens complete.
    {
        RealRHI::FrameResources frames;
        Expect(frames.Initialize(*device, *graphicsQueue, 2) == RealRHI::Result::Success,
               "Create two frame resource slots");
        for (uint32_t i = 0; i < 8; ++i) {
            const uint32_t slot = i % frames.GetSlotCount();
            RealRHI::CommandList* cmd = nullptr;
            Expect(frames.BeginSlot(slot, cmd) == RealRHI::Result::Success,
                   "Recycle completed frame slot");
            Expect(cmd != nullptr, "Frame slot returned a command list");
            Expect(cmd->Begin() == RealRHI::Result::Success, "Begin recycled command list");
            Expect(cmd->End() == RealRHI::Result::Success, "End recycled command list");
            const auto token = graphicsQueue->Submit(*cmd);
            Expect(token.IsValid(), "Submit recycled command list");
            Expect(frames.TrackSubmission(slot, token) == RealRHI::Result::Success,
                   "Track frame slot submission");
        }
        frames.WaitAll();
    }

    // Recording retains a vertex buffer until the command list is reset.
    {
        RealRHI::Ref<RealRHI::Buffer> buffer;
        RealRHI::BufferDesc desc{};
        desc.size = 64;
        desc.usage = RealRHI::BufferUsage::Vertex;
        Expect(device->CreateBuffer(desc, buffer) == RealRHI::Result::Success,
               "Create lifetime-test vertex buffer");
        RealRHI::Ref<RealRHI::CommandList> list;
        Expect(device->CreateCommandList(allocator.Raw(), list) == RealRHI::Result::Success,
               "Create lifetime-test command list");
        Expect(list->Begin() == RealRHI::Result::Success, "Begin lifetime-test list");
        const auto beforeBind = buffer->GetRefCount();
        list->BindVertexBuffer(buffer.Raw());
        Expect(buffer->GetRefCount() > beforeBind, "Command list retains bound buffer");
        Expect(list->End() == RealRHI::Result::Success, "End lifetime-test list");
        const auto token = graphicsQueue->Submit(*list);
        Expect(token.IsValid(), "Submit lifetime-test list");
        graphicsQueue->Wait(token);
        Expect(list->Reset() == RealRHI::Result::Success, "Reset lifetime-test list");
        Expect(buffer->GetRefCount() == beforeBind, "Reset releases bound buffer");
    }

    // Deferred cleanup waits for all dependencies, not merely one queue value.
    {
        RealRHI::DeferredDeletionQueue deletions;
        int destroyed = 0;
        const RealRHI::SubmissionToken first{.queue = {0, 0}, .value = 4};
        const RealRHI::SubmissionToken second{.queue = {1, 0}, .value = 7};
        deletions.Enqueue({first, second}, [&] { ++destroyed; });
        deletions.Process([&](const auto& token) { return token == first; });
        Expect(destroyed == 0 && deletions.PendingCount() == 1,
               "Deletion must wait for all queue dependencies");
        deletions.Process([](const auto&) { return true; });
        Expect(destroyed == 1 && deletions.PendingCount() == 0,
               "Completed deletion runs exactly once");
    }

    // Explicit bindless registrations retain shader-visible resources even
    // when no CPU-side binding command identifies the descriptor index.
    {
        RealRHI::Ref<RealRHI::Buffer> bindless;
        RealRHI::BufferDesc desc{};
        desc.size = 64;
        desc.usage = RealRHI::BufferUsage::Storage;
        Expect(device->CreateBuffer(desc, bindless) == RealRHI::Result::Success,
               "Create bindless lifetime buffer");
        RealRHI::Ref<RealRHI::CommandList> list;
        Expect(device->CreateCommandList(allocator.Raw(), list) == RealRHI::Result::Success,
               "Create bindless lifetime command list");
        Expect(list->Begin() == RealRHI::Result::Success, "Begin bindless lifetime list");
        const auto initialReferences = bindless->GetRefCount();
        Expect(list->UseResource(bindless.Raw()) == RealRHI::Result::Success,
               "Register bindless resource");
        Expect(bindless->GetRefCount() > initialReferences,
               "Bindless registration retains resource");
        Expect(list->End() == RealRHI::Result::Success, "End bindless lifetime list");
        const auto token = graphicsQueue->Submit(*list);
        Expect(token.IsValid(), "Submit bindless lifetime list");
        graphicsQueue->Wait(token);
        Expect(list->Reset() == RealRHI::Result::Success, "Reset bindless lifetime list");
        Expect(bindless->GetRefCount() == initialReferences,
               "Reset releases bindless resource");
    }

    // A recorded command list owns its bindless resource even after the
    // application drops its reference, including across a second submission.
    {
        RealRHI::Ref<RealRHI::Buffer> resource;
        RealRHI::BufferDesc desc{};
        desc.size = 64;
        desc.usage = RealRHI::BufferUsage::Storage;
        Expect(device->CreateBuffer(desc, resource) == RealRHI::Result::Success,
               "Create retained bindless buffer");
        RealRHI::Ref<RealRHI::CommandList> list;
        Expect(device->CreateCommandList(allocator.Raw(), list) == RealRHI::Result::Success,
               "Create retained bindless list");
        Expect(list->Begin() == RealRHI::Result::Success, "Begin retained bindless list");
        Expect(list->UseResource(resource.Raw()) == RealRHI::Result::Success,
               "Register bindless buffer");
        const auto references = resource->GetRefCount();
        Expect(list->UseResource(resource.Raw()) == RealRHI::Result::Success,
               "Duplicate registration accepted");
        Expect(resource->GetRefCount() == references, "Duplicate registration does not retain twice");
        Expect(list->End() == RealRHI::Result::Success, "End retained bindless list");
        resource.Reset();
        const auto first = graphicsQueue->Submit(*list);
        Expect(first.IsValid(), "Submit retained list");
        graphicsQueue->Wait(first);
        const auto second = graphicsQueue->Submit(*list);
        Expect(second.IsValid(), "Resubmit retained list");
        graphicsQueue->Wait(second);
        Expect(list->Reset() == RealRHI::Result::Success,
               "Reset releases retained resources after resubmission");
    }

    std::cout << "QueueSubmissionTests passed" << std::endl;
    return 0;
}
