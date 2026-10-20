#include "Device.h"
#include "Queue.h"
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

    RealRHI::Ref<RealRHI::CommandList> firstList;
    Expect(device->CreateCommandList(firstList) == RealRHI::Result::Success, "Create first command list");
    Expect(firstList->Begin() == RealRHI::Result::Success, "Begin first command list");
    Expect(firstList->End() == RealRHI::Result::Success, "End first command list");

    const RealRHI::SubmissionToken firstToken = graphicsQueue->Submit(*firstList);
    Expect(firstToken.value == 1, "First submission should start at timeline value 1");
    graphicsQueue->Wait(firstToken);
    Expect(graphicsQueue->IsComplete(firstToken), "First submission should be complete after waiting");
    Expect(firstList->GetState() == RealRHI::CommandListState::Executable, "Command list returned to executable state");

    RealRHI::Ref<RealRHI::CommandList> secondList;
    Expect(device->CreateCommandList(secondList) == RealRHI::Result::Success, "Create second command list");
    Expect(secondList->Begin() == RealRHI::Result::Success, "Begin second command list");
    Expect(secondList->End() == RealRHI::Result::Success, "End second command list");

    const RealRHI::SubmissionToken secondToken = graphicsQueue->Submit(*secondList);
    Expect(secondToken.value == 2, "Second submission must increment timeline value");
    graphicsQueue->Wait(secondToken);
    Expect(graphicsQueue->IsComplete(secondToken), "Wait should complete a submission");
    Expect(secondList->Reset() == RealRHI::Result::Success, "Reset command list after completion");

    RealRHI::Ref<RealRHI::CommandList> listA;
    RealRHI::Ref<RealRHI::CommandList> listB;
    Expect(device->CreateCommandList(listA) == RealRHI::Result::Success, "Create list A");
    Expect(device->CreateCommandList(listB) == RealRHI::Result::Success, "Create list B");
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

    std::cout << "QueueSubmissionTests passed" << std::endl;
    return 0;
}
