#include "Device.h"
#include "FrameResources.h"
#include "Vulkan/VulkanCommandQueue.h"
#include "Vulkan/VulkanConvertions.h"
#include "Vulkan/VulkanDescriptorHeaps.h"
#include "Vulkan/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>

static_assert(RealRHI::Utils::ScalarTypeToVkFormat(RealRHI::Float32) == VK_FORMAT_R32_SFLOAT);

namespace {
	VKAPI_ATTR VkResult VKAPI_CALL FailSemaphoreCreation(VkDevice,
		const VkSemaphoreCreateInfo*,
		const VkAllocationCallbacks*,
		VkSemaphore*) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	uint32_t g_ProgrammerErrors = 0;
	uint32_t g_ShaderErrors = 0;

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void CaptureDebugMessage(const RealRHI::DebugMessage& message) {
		if (message.severity == RealRHI::DebugSeverity::Error) {
			Expect(message.type != RealRHI::DebugMessageType::Validation, message.message);
			if (message.type == RealRHI::DebugMessageType::General) {
				++g_ProgrammerErrors;
			}
			if (message.type == RealRHI::DebugMessageType::ShaderCompilation) {
				++g_ShaderErrors;
			}
		}
		RealRHI::DefaultDebugCallback(message);
	}

	template <typename Operation>
	void ExpectRejected(Operation operation, const char* message) {
		const uint32_t before = g_ProgrammerErrors;
		operation();
		Expect(g_ProgrammerErrors == before + 1, message);
	}
}

int main() {
	using namespace RealRHI;
	DeviceDesc desc{};
	desc.enableValidationLayers = true;
	desc.enableDebug = true;
	desc.debugCallback = CaptureDebugMessage;
	desc.shaderDirectory = REALRHI_TEST_SHADER_DIRECTORY;
	std::unique_ptr<Device> device;
	Expect(Device::Create(desc, device) == Result::Success, "Create validation-enabled device");
	auto* queue = device->GetGraphicsCommandQueue();
	{
		auto* vulkanDevice = static_cast<VulkanDevice*>(device.get());
		auto* vulkanQueue = static_cast<VulkanCommandQueue*>(queue);
		std::unique_ptr<VulkanCommandQueue> failedQueue;
		ExpectRejected(
			[&] {
				const auto createSemaphore = vkCreateSemaphore;
				vkCreateSemaphore = FailSemaphoreCreation;
				const Result result = VulkanCommandQueue::Create(vulkanDevice,
					vulkanQueue->GetQueueId().familyIndex,
					vulkanQueue->GetQueueId().queueIndex,
					vulkanQueue->GetQueue(),
					failedQueue);
				vkCreateSemaphore = createSemaphore;

				Expect(result == Result::Failed && !failedQueue,
					"Queue factory reports semaphore failure without publishing an invalid queue");
			},
			"Queue initialization failure emits an actionable diagnostic");
	}

	{
		std::array<uint8_t, 64> initial{};
		Ref<Buffer> buffer;
		BufferDesc bufferDesc{
			.size = initial.size(),
			.usage = BufferUsage::TransferSrc,
			.memoryUsage = MemoryUsage::CPUToGPU,
			.initialData = initial.data(),
		};
		Expect(device->CreateBuffer(bufferDesc, buffer) == Result::Success, "Create mapped buffer");
		Expect(buffer->GetSize() == initial.size(), "GetSize returns logical size rather than allocation padding");
		const uint8_t value = 73;
		buffer->WriteData(&value, 1, 63);
		ExpectRejected(
			[&] {
				buffer->WriteData(&value, 1, 64);
			},
			"Reject write beyond logical end");
		ExpectRejected(
			[&] {
				buffer->WriteData(&value, UINT64_MAX, 1);
			},
			"Reject overflowing write range");
		ExpectRejected(
			[&] {
				buffer->WriteData(nullptr, 1);
			},
			"Reject null write data");
		std::array<uint8_t, 64> actual{};
		Expect(buffer->ReadData(actual.data(), actual.size()) == Result::Success, "Read mapped data");
		initial[63] = value;
		Expect(actual == initial, "Rejected writes preserve buffer contents");
		uint8_t unused = 0;
		Expect(buffer->ReadData(&unused, 1, 64) == Result::InvalidParameter, "Read respects logical end");

		Ref<Buffer> gpuOnly;
		bufferDesc.memoryUsage = MemoryUsage::GPUOnly;
		bufferDesc.initialData = nullptr;
		Expect(device->CreateBuffer(bufferDesc, gpuOnly) == Result::Success, "Create GPU-only buffer");
		// GPU-only buffers may reside in host-visible heaps, but are never mapped by this API.
		ExpectRejected(
			[&] {
				gpuOnly->WriteData(&value, 1);
			},
			"Reject write to an unmapped buffer");
		bufferDesc.initialData = initial.data();
		Ref<Buffer> rejected;
		ExpectRejected(
			[&] {
				Expect(device->CreateBuffer(bufferDesc, rejected) == Result::InvalidParameter,
					"GPUOnly initial data requires an explicit staging copy");
			},
			"Reject unsupported GPUOnly initialization before accessing a mapped pointer");

		TextureDesc textureDesc{};
		textureDesc.width = 4;
		textureDesc.height = 4;
		textureDesc.format = TextureFormat::RGBA8_UNorm;
		textureDesc.usage = TextureUsage::TransferDst;
		Ref<Texture> texture;
		Expect(device->CreateTexture(textureDesc, texture) == Result::Success, "Create transfer texture");
		ExpectRejected(
			[&] {
				Expect(texture->UploadData(initial.data(), 1) == Result::InvalidParameter, "Reject undersized upload");
			},
			"Upload validates byte count before submission");
		Expect(texture->UploadData(initial.data(), initial.size()) == Result::Success, "Upload transfer-only texture");

		Ref<CommandAllocator> allocator;
		Ref<CommandList> list;
		Expect(
			device->CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success, "Create copy allocator");
		Expect(device->CreateCommandList(allocator.Raw(), list) == Result::Success, "Create copy list");
		Expect(list->Begin() == Result::Success, "Begin copy list");
		BufferTextureCopyRegion region{.width = 4, .height = 4};
		region.mipLevel = UINT32_MAX;
		ExpectRejected(
			[&] {
				list->CopyBufferToTexture(buffer.Raw(), texture.Raw(), region);
			},
			"Reject invalid mip before layout lookup");
		region.mipLevel = 0;
		region.baseArrayLayer = UINT32_MAX;
		ExpectRejected(
			[&] {
				list->CopyBufferToTexture(buffer.Raw(), texture.Raw(), region);
			},
			"Reject invalid layer before layout lookup");
		region.baseArrayLayer = 0;
		region.bufferOffset = 4;
		ExpectRejected(
			[&] {
				list->CopyBufferToTexture(buffer.Raw(), texture.Raw(), region);
			},
			"Reject source buffer too small for the full copy");
		region.bufferOffset = 0;
		list->CopyBufferToTexture(buffer.Raw(), texture.Raw(), region);
		Expect(list->End() == Result::Success, "End list after rejected and valid copies");
		const auto token = queue->Submit(*list);
		Expect(token.IsValid(), "Submit valid copy after rejected commands");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!queue->IsComplete(token) && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::yield();
		}
		Expect(queue->IsComplete(token), "Polling observes GPU completion");
		Expect(list->Reset() == Result::Success, "Polling retires the list without a CPU Wait call");

		Expect(list->Begin() == Result::Success && list->End() == Result::Success, "Record empty dependency list");
		CommandList* lists[] = {list.Raw()};
		SubmissionToken future = token;
		future.value = UINT64_MAX;
		ExpectRejected(
			[&] {
				Expect(!queue->Submit(SubmitInfo{.commandLists = lists, .waitFor = {&future, 1}}).IsValid(),
					"Reject unsubmitted wait value");
			},
			"Invalid future wait cannot stall the queue");
		const SubmissionToken waits[] = {token, token};
		const auto duplicateWaitToken = queue->Submit(SubmitInfo{.commandLists = lists, .waitFor = waits});
		Expect(duplicateWaitToken.IsValid(), "Coalesce duplicate semaphore waits");
		queue->Wait(duplicateWaitToken);

		std::array<Ref<CommandList>, 3> pendingLists;
		std::array<SubmissionToken, 3> pendingTokens;
		for (size_t i = 0; i < pendingLists.size(); ++i) {
			Expect(device->CreateCommandList(allocator.Raw(), pendingLists[i]) == Result::Success,
				"Create retirement test list");
			Expect(pendingLists[i]->Begin() == Result::Success && pendingLists[i]->End() == Result::Success,
				"Record retirement test list");
			pendingTokens[i] = queue->Submit(*pendingLists[i]);
			Expect(pendingTokens[i].IsValid(), "Queue retirement test submission");
			Expect(pendingLists[i]->GetRefCount() == 2, "Pending submission retains its command list");
		}

		queue->Wait(pendingTokens[1]);
		for (size_t i = 0; i < 2; ++i) {
			Expect(pendingLists[i]->GetState() == CommandListState::Executable && pendingLists[i]->GetRefCount() == 1,
				"Waiting for a later token retires every preceding submission");
		}
		Expect(pendingLists[2]->GetState() == CommandListState::Pending && pendingLists[2]->GetRefCount() == 2,
			"Retirement preserves submissions beyond the requested completion value");

		queue->WaitIdle();
		Expect(pendingLists[2]->GetState() == CommandListState::Executable && pendingLists[2]->GetRefCount() == 1,
			"WaitIdle retires the remaining submission");
		for (auto& pendingList : pendingLists) {
			Expect(pendingList->Reset() == Result::Success, "Retired command lists can be reused");
		}
	}

	for (QueueType type : {QueueType::Compute, QueueType::Transfer}) {
		auto* selectedQueue = device->GetCommandQueue(type);
		FrameResources frames;
		Expect(frames.Initialize(*device, type, 1) == Result::Success, "Create frame resources for selected queue");
		CommandList* list = nullptr;
		Expect(frames.BeginSlot(0, list) == Result::Success, "Begin selected queue slot");
		Expect(list->Begin() == Result::Success && list->End() == Result::Success, "Record selected queue list");
		const auto token = selectedQueue->Submit(*list);
		Expect(token.IsValid(), "Frame allocator matches selected queue family");
		auto wrongQueue = token;
		wrongQueue.queue.familyIndex = UINT32_MAX;
		Expect(frames.TrackSubmission(0, wrongQueue) != Result::Success, "Reject foreign queue completion token");
		Expect(frames.TrackSubmission(0, token) == Result::Success, "Track selected queue submission");
	}

	{
		auto* vulkanDevice = static_cast<VulkanDevice*>(device.get());
		auto properties = vulkanDevice->GetDescriptorHeapProperties();
		properties.minResourceHeapReservedRange = std::max<VkDeviceSize>(properties.minResourceHeapReservedRange, 256);
		properties.minSamplerHeapReservedRange = std::max<VkDeviceSize>(properties.minSamplerHeapReservedRange, 256);
		properties.maxResourceHeapSize = std::min(properties.maxResourceHeapSize,
			properties.minResourceHeapReservedRange +
				std::max(properties.resourceHeapAlignment * 2,
					std::max(properties.bufferDescriptorSize, properties.imageDescriptorSize) * 8) +
				1);
		properties.maxSamplerHeapSize = std::min(properties.maxSamplerHeapSize,
			properties.minSamplerHeapReservedRange +
				std::max(properties.samplerHeapAlignment * 2, properties.samplerDescriptorSize * 8) + 1);
		VulkanDescriptorHeaps heaps(vulkanDevice);
		Expect(heaps.Init(properties) == Result::Success, "Create heaps with nonzero reserved ranges and capped sizes");
		Expect(heaps.GetResourceHeap().heapSize <= properties.maxResourceHeapSize,
			"Heap alignment respects resource maximum");
		Expect(heaps.GetSamplerHeap().heapSize <= properties.maxSamplerHeapSize,
			"Heap alignment respects sampler maximum");
		Expect(heaps.GetResourceHeap().usableSize + properties.minResourceHeapReservedRange <=
				heaps.GetResourceHeap().heapSize,
			"Resource descriptors exclude reserved bytes");
		Expect(heaps.GetSamplerHeap().usableSize + properties.minSamplerHeapReservedRange <=
				heaps.GetSamplerHeap().heapSize,
			"Sampler descriptors exclude reserved bytes");
	}

	{
		Ref<Shader> shader;
		Expect(device->CreateShader({.moduleName = "shader"}, shader) == Result::Success, "Compile graphics shader");
		Expect(g_ShaderErrors == 0, "Successful reflection is not logged as a shader error");
		PipelineDesc pipelineDesc{};
		pipelineDesc.shader = shader.Raw();
		pipelineDesc.depthState.depthTestEnable = false;
		pipelineDesc.depthState.depthWriteEnable = false;
		pipelineDesc.renderTargetFormats.colorFormats = {TextureFormat::RGBA8_UNorm, TextureFormat::RGBA8_UNorm};
		Ref<Pipeline> pipeline;
		Expect(device->CreateGraphicsPipeline(pipelineDesc, pipeline) == Result::Success,
			"Create multiple-color-attachment pipeline");
		pipelineDesc.rasterState.fillMode = FillMode::Wireframe;
		if (static_cast<VulkanDevice*>(device.get())->SupportsWireframe()) {
			Expect(device->CreateGraphicsPipeline(pipelineDesc, pipeline) == Result::Success,
				"Create supported wireframe pipeline");
		}
		pipelineDesc.rasterState.fillMode = FillMode::Solid;
		pipelineDesc.renderTargetFormats.colorFormats.clear();
		pipelineDesc.renderTargetFormats.depthFormat = TextureFormat::D24_UNorm_S8_UInt;
		pipelineDesc.depthState.depthTestEnable = true;
		pipelineDesc.depthState.depthWriteEnable = true;
		Expect(device->CreateGraphicsPipeline(pipelineDesc, pipeline) == Result::Success,
			"Create depth/stencil-only pipeline");

		TextureDesc textureDesc{};
		textureDesc.width = 4;
		textureDesc.height = 4;
		textureDesc.format = TextureFormat::RGBA8_UNorm;
		textureDesc.usage = TextureUsage::Storage;
		Ref<Texture> storage;
		Expect(device->CreateTexture(textureDesc, storage) == Result::Success, "Create storage texture");
		Expect(storage->GetStorageImageDescriptorIndex() != UINT32_MAX,
			"Expose storage descriptor through abstract texture");
		textureDesc.format = TextureFormat::D24_UNorm_S8_UInt;
		textureDesc.usage = TextureUsage::DepthStencil;
		Ref<Texture> depth;
		Expect(device->CreateTexture(textureDesc, depth) == Result::Success, "Create depth/stencil texture");
		Ref<CommandAllocator> allocator;
		Ref<CommandList> list;
		Expect(device->CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success,
			"Create layout allocator");
		Expect(device->CreateCommandList(allocator.Raw(), list) == Result::Success, "Create layout list");
		ExpectRejected(
			[&] {
				list->BindVertexBuffer(nullptr);
			},
			"Reject null binding outside recording");
		ExpectRejected(
			[&] {
				list->Draw(3);
			},
			"Reject drawing outside recording");
		Expect(list->Begin() == Result::Success, "Begin layout list");
		ExpectRejected(
			[&] {
				list->EndRendering();
			},
			"Reject ending a missing rendering scope");
		BarrierDesc barriers;
		barriers.textures.push_back({
			.texture = storage.Raw(),
			.after = {PipelineStage::ComputeShader, ResourceAccess::ShaderWrite},
			.newLayout = TextureLayout::General,
		});
		barriers.textures.push_back({
			.texture = depth.Raw(),
			.after = {PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests,
				ResourceAccess::DepthStencilWrite},
			.newLayout = TextureLayout::DepthStencilAttachment,
		});
		list->Barrier(barriers);
		RenderingInfo rendering;
		rendering.hasDepth = true;
		rendering.depthAttachment.target = depth->GetTextureView();
		rendering.renderArea = {.width = 4, .height = 4};
		list->BeginRendering(rendering);
		list->EndRendering();
		Expect(list->End() == Result::Success, "End depth/storage layout list");
		Expect(device->ImmediateSubmit(list.Raw()) == Result::Success, "Submit depth rendering and storage transition");
	}
	{
		std::array<uint8_t, 32> pixels{};
		TextureDesc textureDesc{};
		textureDesc.width = 4;
		textureDesc.height = 1;
		textureDesc.arrayLayers = 2;
		textureDesc.format = TextureFormat::RGBA8_UNorm;
		textureDesc.usage = TextureUsage::ShaderResource | TextureUsage::TransferDst;
		Ref<Texture> texture;
		Expect(device->CreateTexture(textureDesc, texture) == Result::Success,
			"Single-row 2D array uses a matching image view type");
		Expect(
			texture->UploadData(pixels.data(), pixels.size()) == Result::Success, "Upload a single-row texture array");
	}
	device->WaitIdle();
	std::cout << "ResourceSafetyTests passed\n";
}
