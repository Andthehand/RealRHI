#include "Device.h"
#include "Queue.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>

namespace {
    struct Parameters {
        uint32_t outputIndex;
        uint32_t multiplier;
        uint32_t addend;
        uint32_t elementCount;
    };

    bool Check(RealRHI::Result result, const char* operation) {
        if (result == RealRHI::Result::Success) {
            return true;
        }
        std::cerr << operation << " failed\n";
        return false;
    }
}

int main() {
    using namespace RealRHI;
    constexpr uint32_t count = 64;
    constexpr uint64_t bytes = count * sizeof(uint32_t);

    DeviceDesc desc{};
    desc.applicationName = "RealRHI MultiQueue Example";
    desc.shaderDirectory = std::filesystem::current_path() / "shaders";
    desc.enableValidationLayers = true;
    desc.enableDebug = true;

    std::unique_ptr<Device> device;
    if (!Check(Device::Create(desc, device), "Create device")) {
        return 1;
    }

    auto* transferQueue = device->GetCommandQueue(QueueType::Transfer);
    auto* computeQueue = device->GetCommandQueue(QueueType::Compute);
    auto* graphicsQueue = device->GetCommandQueue(QueueType::Graphics);
    if (!transferQueue || !computeQueue || !graphicsQueue) {
        return 1;
    }

    Ref<Shader> shader;
    Ref<Pipeline> pipeline;
    if (!Check(device->CreateShader({ .moduleName = "compute" }, shader), "Compile shader") ||
        !Check(device->CreateComputePipeline({ .shader = shader.Raw() }, pipeline), "Create pipeline")) {
        return 1;
    }

    std::array<uint32_t, count> initial{};
    initial.fill(0);
    Ref<Buffer> upload;
    Ref<Buffer> output;
    Ref<Buffer> readback;
    if (!Check(device->CreateBuffer({
            .size = bytes,
            .usage = BufferUsage::TransferSrc,
            .memoryUsage = MemoryUsage::CPUToGPU,
        }, upload), "Create upload buffer") ||
        !Check(device->CreateBuffer({
            .size = bytes,
            .usage = BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst,
        }, output), "Create output buffer") ||
        !Check(device->CreateBuffer({
            .size = bytes,
            .usage = BufferUsage::TransferDst,
            .memoryUsage = MemoryUsage::GPUToCPU,
        }, readback), "Create readback buffer")) {
        return 1;
    }
    upload->WriteData(initial.data(), bytes);

    const QueueType roles[] = { QueueType::Transfer, QueueType::Compute, QueueType::Graphics };
    CommandQueue* queues[] = { transferQueue, computeQueue, graphicsQueue };
    std::array<Ref<CommandAllocator>, 3> allocators;
    std::array<Ref<CommandList>, 3> lists;
    for (uint32_t i = 0; i < 3; ++i) {
        if (!Check(device->CreateCommandAllocator(roles[i], allocators[i]), "Create allocator") ||
            !Check(device->CreateCommandList(allocators[i].Raw(), lists[i]), "Create list") ||
            !Check(lists[i]->Begin(), "Begin list")) {
            return 1;
        }
    }

    // Transfer: initialize the storage buffer, then release it to compute.
    lists[0]->CopyBuffer(upload.Raw(), output.Raw(), { .size = bytes });
    {
        BarrierDesc release;
        release.buffers.push_back({
            .buffer = output.Raw(),
            .before = { PipelineStage::Transfer, ResourceAccess::TransferWrite },
            .after = { PipelineStage::None, ResourceAccess::None },
            .offset = 0, .size = bytes,
            .sourceQueue = roles[0], .destinationQueue = roles[1],
        });
        lists[0]->Barrier(release);
    }

    // Compute: acquire, overwrite with a deterministic kernel, then release.
    {
        BarrierDesc acquire;
        acquire.buffers.push_back({
            .buffer = output.Raw(),
            .before = { PipelineStage::None, ResourceAccess::None },
            .after = { PipelineStage::ComputeShader, ResourceAccess::ShaderWrite },
            .offset = 0, .size = bytes,
            .sourceQueue = roles[0], .destinationQueue = roles[1],
        });
        lists[1]->Barrier(acquire);
    }
    const Parameters parameters{ output->GetDescriptorIndex(), 2, 1, count };
    lists[1]->BindPipeline(pipeline.Raw());
    lists[1]->UseResource(output.Raw());
    lists[1]->SetPushConstants(&parameters, sizeof(parameters));
    lists[1]->DispatchCompute(1, 1, 1);
    {
        BarrierDesc release;
        release.buffers.push_back({
            .buffer = output.Raw(),
            .before = { PipelineStage::ComputeShader, ResourceAccess::ShaderWrite },
            .after = { PipelineStage::None, ResourceAccess::None },
            .offset = 0, .size = bytes,
            .sourceQueue = roles[1], .destinationQueue = roles[2],
        });
        lists[1]->Barrier(release);
    }

    // Graphics queue consumes the compute result via transfer readback.
    {
        BarrierDesc acquire;
        acquire.buffers.push_back({
            .buffer = output.Raw(),
            .before = { PipelineStage::None, ResourceAccess::None },
            .after = { PipelineStage::Transfer, ResourceAccess::TransferRead },
            .offset = 0, .size = bytes,
            .sourceQueue = roles[1], .destinationQueue = roles[2],
        });
        lists[2]->Barrier(acquire);
    }
    lists[2]->CopyBuffer(output.Raw(), readback.Raw(), { .size = bytes });

    for (auto& list : lists) {
        if (!Check(list->End(), "End list")) {
            return 1;
        }
    }

    std::array<SubmissionToken, 3> tokens{};
    for (uint32_t i = 0; i < 3; ++i) {
        CommandList* commands[] = { lists[i].Raw() };
        const std::span<const SubmissionToken> waits =
            i == 0 ? std::span<const SubmissionToken>{} :
                std::span<const SubmissionToken>(&tokens[i - 1], 1);
        tokens[i] = queues[i]->Submit(SubmitInfo{
            .commandLists = commands,
            .waitFor = waits,
        });
        if (!tokens[i].IsValid()) {
            std::cerr << "Queue submission failed at stage " << i << '\n';
            return 1;
        }
    }

    // The final wait guarantees GPU completion of the dependency chain.
    queues[2]->Wait(tokens[2]);
    std::array<uint32_t, count> result{};
    if (!Check(readback->ReadData(result.data(), bytes), "Readback")) {
        return 1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (result[i] != 2 * i + 1) {
            std::cerr << "Incorrect GPU output at " << i << '\n';
            return 1;
        }
    }

    // Finalize the tracked lists on every queue before resetting.
    for (CommandQueue* queue : queues) {
        queue->WaitIdle();
    }
    for (auto& list : lists) {
        if (!Check(list->Reset(), "Reset list")) {
            return 1;
        }
    }
    device->WaitIdle();
    std::cout << "MultiQueueExample passed\n";
    return 0;
}
