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
    desc.enableValidationLayers = true;
    desc.enableDebug = true;

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


    // Invalid barrier ranges must fail without recording a Vulkan command.
    {
        RealRHI::Ref<RealRHI::Buffer> buffer;
        RealRHI::BufferDesc desc{};
        desc.size = 64;
        desc.usage = RealRHI::BufferUsage::TransferDst;
        Expect(device->CreateBuffer(desc, buffer) == RealRHI::Result::Success,
               "Create invalid-range barrier test buffer");

        RealRHI::Ref<RealRHI::CommandList> list;
        Expect(device->CreateCommandList(allocator.Raw(), list) == RealRHI::Result::Success,
               "Create invalid-range barrier test list");
        Expect(list->Begin() == RealRHI::Result::Success, "Begin invalid-range barrier test list");

        RealRHI::BarrierDesc barriers;
        barriers.buffers.push_back({
            .buffer = buffer.Raw(),
            .before = { RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferWrite },
            .after = { RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferRead },
            .offset = 60,
            .size = 16,
        });
        Expect(list->Barrier(barriers) != RealRHI::Result::Success,
               "Reject out-of-bounds buffer barrier");

        RealRHI::TextureDesc textureDesc{};
        textureDesc.width = 4;
        textureDesc.height = 4;
        textureDesc.mipLevels = 1;
        textureDesc.arrayLayers = 1;
        textureDesc.format = RealRHI::TextureFormat::RGBA8_UNorm;
        textureDesc.usage = RealRHI::TextureUsage::TransferDst;

        RealRHI::Ref<RealRHI::Texture> texture;
        Expect(device->CreateTexture(textureDesc, texture) == RealRHI::Result::Success,
               "Create invalid-range barrier test texture");
        barriers.buffers.clear();
        barriers.textures.push_back({
            .texture = texture.Raw(),
            .oldLayout = RealRHI::TextureLayout::Undefined,
            .newLayout = RealRHI::TextureLayout::TransferDst,
            .subresources = { .baseMipLevel = 1, .mipLevelCount = 1, .baseArrayLayer = 0, .arrayLayerCount = 1 },
        });
        Expect(list->Barrier(barriers) != RealRHI::Result::Success,
               "Reject out-of-bounds texture mip barrier");
        Expect(list->End() == RealRHI::Result::Success, "End invalid-range barrier test list");
    }

    // Verify texture-to-texture copies and GPU readback against CPU data.
    {
        constexpr uint32_t width = 8;
        constexpr uint32_t height = 8;
        std::array<uint8_t, width * height * 4> pixels{};
        for (size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<uint8_t>((i * 17) & 0xff);
        }

        RealRHI::TextureDesc textureDesc{};
        textureDesc.width = width;
        textureDesc.height = height;
        textureDesc.format = RealRHI::TextureFormat::RGBA8_UNorm;
        textureDesc.usage = RealRHI::TextureUsage::ShaderResource |
            RealRHI::TextureUsage::TransferSrc | RealRHI::TextureUsage::TransferDst;

        RealRHI::Ref<RealRHI::Texture> source;
        RealRHI::Ref<RealRHI::Texture> destination;
        Expect(device->CreateTexture(textureDesc, source) == RealRHI::Result::Success,
               "Create source transfer texture");
        Expect(device->CreateTexture(textureDesc, destination) == RealRHI::Result::Success,
               "Create destination transfer texture");
        Expect(source->UploadData(pixels.data(), static_cast<uint32_t>(pixels.size())) == RealRHI::Result::Success,
               "Upload source texture data");

        RealRHI::BufferDesc readbackDesc{};
        readbackDesc.size = pixels.size();
        readbackDesc.usage = RealRHI::BufferUsage::TransferDst;
        readbackDesc.memoryUsage = RealRHI::MemoryUsage::GPUToCPU;
        RealRHI::Ref<RealRHI::Buffer> readback;
        Expect(device->CreateBuffer(readbackDesc, readback) == RealRHI::Result::Success,
               "Create texture readback buffer");

        RealRHI::Ref<RealRHI::CommandList> list;
        Expect(device->CreateCommandList(allocator.Raw(), list) == RealRHI::Result::Success,
               "Create texture transfer list");
        Expect(list->Begin() == RealRHI::Result::Success, "Begin texture transfer list");

        RealRHI::BarrierDesc barriers;
        barriers.textures.push_back({
            .texture = source.Raw(),
            .before = { RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferWrite },
            .after = { RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferRead },
            .oldLayout = RealRHI::TextureLayout::ShaderRead,
            .newLayout = RealRHI::TextureLayout::TransferSrc,
        });
        barriers.textures.push_back({
            .texture = destination.Raw(),
            .before = { RealRHI::PipelineStage::None, RealRHI::ResourceAccess::None },
            .after = { RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferWrite },
            .oldLayout = RealRHI::TextureLayout::Undefined,
            .newLayout = RealRHI::TextureLayout::TransferDst,
        });
        Expect(list->Barrier(barriers) == RealRHI::Result::Success,
               "Prepare source and destination image layouts");

        RealRHI::TextureCopyRegion copyRegion{};
        copyRegion.width = width;
        copyRegion.height = height;
        Expect(list->CopyTexture(source.Raw(), destination.Raw(), copyRegion) == RealRHI::Result::Success,
               "Copy texture to texture");

        barriers.textures.clear();
        barriers.textures.push_back({
            .texture = destination.Raw(),
            .before = { RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferWrite },
            .after = { RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferRead },
            .oldLayout = RealRHI::TextureLayout::TransferDst,
            .newLayout = RealRHI::TextureLayout::TransferSrc,
        });
        Expect(list->Barrier(barriers) == RealRHI::Result::Success,
               "Prepare destination texture for readback");

        RealRHI::BufferTextureCopyRegion readbackRegion{};
        readbackRegion.width = width;
        readbackRegion.height = height;
        Expect(list->CopyTextureToBuffer(destination.Raw(), readback.Raw(), readbackRegion) == RealRHI::Result::Success,
               "Copy texture to readback buffer");
        Expect(list->End() == RealRHI::Result::Success, "End texture transfer list");

        const auto token = graphicsQueue->Submit(*list);
        Expect(token.IsValid(), "Submit texture transfer commands");
        graphicsQueue->Wait(token);
        Expect(graphicsQueue->IsComplete(token), "Wait for texture transfer completion");

        std::array<uint8_t, width * height * 4> actual{};
        Expect(readback->ReadData(actual.data(), actual.size()) == RealRHI::Result::Success,
               "Read mapped GPU readback data");
        Expect(actual == pixels, "Texture copy and readback contents match original data");
        Expect(list->Reset() == RealRHI::Result::Success, "Reset completed texture transfer list");
    }

    // Layout tracking is scoped to the recording and to individual subresources.
    {
        RealRHI::TextureDesc desc{};
        desc.width = 8;
        desc.height = 8;
        desc.mipLevels = 2;
        desc.arrayLayers = 2;
        desc.format = RealRHI::TextureFormat::RGBA8_UNorm;
        desc.usage = RealRHI::TextureUsage::TransferSrc | RealRHI::TextureUsage::TransferDst;

        RealRHI::Ref<RealRHI::Texture> texture;
        Expect(device->CreateTexture(desc, texture) == RealRHI::Result::Success,
               "Create tracked-layout texture");

        RealRHI::Ref<RealRHI::CommandList> list;
        Expect(device->CreateCommandList(allocator.Raw(), list) == RealRHI::Result::Success,
               "Create tracked-layout command list");
        Expect(list->Begin() == RealRHI::Result::Success, "Begin tracked-layout list");

        RealRHI::BarrierDesc barriers;
        barriers.textures.push_back({
            .texture = texture.Raw(),
            .before = { RealRHI::PipelineStage::None, RealRHI::ResourceAccess::None },
            .after = { RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferWrite },
            .oldLayout = RealRHI::TextureLayout::Undefined,
            .newLayout = RealRHI::TextureLayout::TransferDst,
            .subresources = { .baseMipLevel = 1, .mipLevelCount = 1, .baseArrayLayer = 1, .arrayLayerCount = 1 },
        });
        Expect(list->Barrier(barriers) == RealRHI::Result::Success,
               "Record a single subresource transition");

        // An inconsistent second transition must be rejected.
        barriers.textures[0].oldLayout = RealRHI::TextureLayout::ShaderRead;
        barriers.textures[0].newLayout = RealRHI::TextureLayout::TransferSrc;
        Expect(list->Barrier(barriers) != RealRHI::Result::Success,
               "Reject mismatched recorded old layout");

        barriers.textures[0].oldLayout = RealRHI::TextureLayout::TransferDst;
        Expect(list->Barrier(barriers) == RealRHI::Result::Success,
               "Accept transition from the recorded layout");

        Expect(list->End() == RealRHI::Result::Success, "End tracked-layout list");
        const auto token = graphicsQueue->Submit(*list);
        Expect(token.IsValid(), "Submit tracked-layout list");
        graphicsQueue->Wait(token);
        Expect(list->Reset() == RealRHI::Result::Success, "Reset tracked-layout list");
    }

    // Submission state is committed in queue order, not during recording.
    {
        RealRHI::TextureDesc desc{};
        desc.width = 4;
        desc.height = 4;
        desc.format = RealRHI::TextureFormat::RGBA8_UNorm;
        desc.usage = RealRHI::TextureUsage::TransferSrc | RealRHI::TextureUsage::TransferDst;

        RealRHI::Ref<RealRHI::Texture> texture;
        Expect(device->CreateTexture(desc, texture) == RealRHI::Result::Success,
               "Create submission-state texture");

        RealRHI::Ref<RealRHI::CommandList> first;
        RealRHI::Ref<RealRHI::CommandList> stale;
        Expect(device->CreateCommandList(allocator.Raw(), first) == RealRHI::Result::Success,
               "Create first state list");
        Expect(device->CreateCommandList(allocator.Raw(), stale) == RealRHI::Result::Success,
               "Create stale state list");

        RealRHI::BarrierDesc transition;
        transition.textures.push_back({
            .texture = texture.Raw(),
            .before = { RealRHI::PipelineStage::None, RealRHI::ResourceAccess::None },
            .after = { RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferWrite },
            .oldLayout = RealRHI::TextureLayout::Undefined,
            .newLayout = RealRHI::TextureLayout::TransferDst,
        });

        // Establish a non-Undefined state before recording competing transitions.
        RealRHI::Ref<RealRHI::CommandList> prime;
        Expect(device->CreateCommandList(allocator.Raw(), prime) == RealRHI::Result::Success,
            "Create initial state list");
        Expect(prime->Begin() == RealRHI::Result::Success, "Begin initial state list");
        Expect(prime->Barrier(transition) == RealRHI::Result::Success, "Record initial state");
        Expect(prime->End() == RealRHI::Result::Success, "End initial state list");
        const auto primeToken = graphicsQueue->Submit(*prime);
        Expect(primeToken.IsValid(), "Submit initial state");
        graphicsQueue->Wait(primeToken);
        Expect(prime->Reset() == RealRHI::Result::Success, "Reset initial state list");

        transition.textures[0].oldLayout = RealRHI::TextureLayout::TransferDst;
        transition.textures[0].newLayout = RealRHI::TextureLayout::TransferSrc;
        transition.textures[0].before = {
            RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferWrite
        };
        transition.textures[0].after = {
            RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferRead
        };

        Expect(first->Begin() == RealRHI::Result::Success, "Begin first state list");
        Expect(first->Barrier(transition) == RealRHI::Result::Success, "Record first state transition");
        Expect(first->End() == RealRHI::Result::Success, "End first state list");

        Expect(stale->Begin() == RealRHI::Result::Success, "Begin stale state list");
        Expect(stale->Barrier(transition) == RealRHI::Result::Success, "Record stale state transition");
        Expect(stale->End() == RealRHI::Result::Success, "End stale state list");

        const auto firstToken = graphicsQueue->Submit(*first);
        Expect(firstToken.IsValid(), "Submit first state list");
        Expect(!graphicsQueue->Submit(*stale).IsValid(),
               "Reject stale command list without submitting it");
        graphicsQueue->Wait(firstToken);

        Expect(stale->Reset() == RealRHI::Result::Success, "Reset stale list");
        transition.textures[0].oldLayout = RealRHI::TextureLayout::TransferSrc;
        transition.textures[0].newLayout = RealRHI::TextureLayout::TransferDst;
        transition.textures[0].before = {
            RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferRead
        };
        transition.textures[0].after = {
            RealRHI::PipelineStage::Transfer, RealRHI::ResourceAccess::TransferWrite
        };
        Expect(stale->Begin() == RealRHI::Result::Success, "Begin corrected state list");
        Expect(stale->Barrier(transition) == RealRHI::Result::Success, "Record corrected transition");
        Expect(stale->End() == RealRHI::Result::Success, "End corrected state list");
        const auto secondToken = graphicsQueue->Submit(*stale);
        Expect(secondToken.IsValid(), "Submit corrected state list");
        graphicsQueue->Wait(secondToken);
        Expect(stale->Reset() == RealRHI::Result::Success, "Reset corrected state list");
        Expect(first->Reset() == RealRHI::Result::Success, "Reset first state list");
    }

    // Discard transitions may use Undefined even after a previous submission.
    {
        RealRHI::TextureDesc desc{};
        desc.width = 4;
        desc.height = 4;
        desc.format = RealRHI::TextureFormat::RGBA8_UNorm;
        desc.usage = RealRHI::TextureUsage::TransferDst | RealRHI::TextureUsage::TransferSrc;

        RealRHI::Ref<RealRHI::Texture> texture;
        Expect(device->CreateTexture(desc, texture) == RealRHI::Result::Success,
            "Create discard transition texture");
        RealRHI::Ref<RealRHI::CommandList> list;
        Expect(device->CreateCommandList(allocator.Raw(), list) == RealRHI::Result::Success,
            "Create discard transition list");
        RealRHI::BarrierDesc barriers;
        barriers.textures.push_back({
            .texture = texture.Raw(),
            .oldLayout = RealRHI::TextureLayout::Undefined,
            .newLayout = RealRHI::TextureLayout::TransferDst,
        });
        Expect(list->Begin() == RealRHI::Result::Success, "Begin discard transition list");
        Expect(list->Barrier(barriers) == RealRHI::Result::Success, "Record initial transition");
        Expect(list->End() == RealRHI::Result::Success, "End initial transition");
        auto token = graphicsQueue->Submit(*list);
        Expect(token.IsValid(), "Submit initial transition");
        graphicsQueue->Wait(token);
        Expect(list->Reset() == RealRHI::Result::Success, "Reset discard transition list");

        Expect(list->Begin() == RealRHI::Result::Success, "Begin discard rerecord");
        Expect(list->Barrier(barriers) == RealRHI::Result::Success,
            "Record discard transition from previously used image");
        Expect(list->End() == RealRHI::Result::Success, "End discard rerecord");
        token = graphicsQueue->Submit(*list);
        Expect(token.IsValid(), "Submit discard transition from used image");
        graphicsQueue->Wait(token);
        Expect(list->Reset() == RealRHI::Result::Success, "Reset discard rerecord");

        Expect(list->Begin() == RealRHI::Result::Success, "Begin overlap test");
        barriers.textures.push_back(barriers.textures.front());
        Expect(list->Barrier(barriers) != RealRHI::Result::Success,
            "Reject overlapping texture barriers");
        barriers.textures.pop_back();
        Expect(list->Barrier(barriers) == RealRHI::Result::Success,
            "Accept non-overlapping barrier after rejection");
        Expect(list->End() == RealRHI::Result::Success, "End overlap test");
    }

    // Compute dispatch requires an explicitly bound compute pipeline.
    {
        RealRHI::Ref<RealRHI::CommandList> computeList;
        Expect(device->CreateCommandList(allocator.Raw(), computeList) == RealRHI::Result::Success,
            "Create compute validation command list");
        Expect(computeList->Begin() == RealRHI::Result::Success,
            "Begin compute validation command list");
        Expect(computeList->Dispatch(1, 1, 1) != RealRHI::Result::Success,
            "Reject dispatch without a compute pipeline");
        Expect(computeList->End() == RealRHI::Result::Success,
            "End compute validation command list");
    }

    std::cout << "QueueSubmissionTests passed" << std::endl;
    return 0;
}
