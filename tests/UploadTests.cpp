#include "Device.h"
#include "Vulkan/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
	using namespace RealRHI;
	uint32_t g_Errors = 0;
	uint32_t g_ExpectedErrors = 0;
	std::string g_LastError;

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void CaptureDebugMessage(const DebugMessage& message) {
		DefaultDebugCallback(message);
		if (message.severity == DebugSeverity::Error) {
			Expect(message.type == DebugMessageType::General, "Unexpected Vulkan validation or shader error");
			++g_Errors;
			g_LastError = message.message;
		}
	}

	template <typename Operation>
	void ExpectRejected(Operation operation, Result expected, const char* cause) {
		const auto before = g_Errors;
		Expect(operation() == expected, "Rejected operation returns the expected result");
		Expect(g_Errors == before + 1 && g_LastError.find(cause) != std::string::npos,
			"Rejected operation identifies its cause");
		++g_ExpectedErrors;
	}

	class UploadTestDevice final : public VulkanDevice {
	public:
		explicit UploadTestDevice(bool graphicsOnly)
			: m_GraphicsOnly(graphicsOnly) { }

		using VulkanDevice::GetCommandQueue;
		CommandQueue* GetCommandQueue(QueueType type) const override {
			if (m_GraphicsOnly &&
				(type == QueueType::Graphics || type == QueueType::Compute || type == QueueType::Transfer)) {
				return GetGraphicsCommandQueue();
			}
			return VulkanDevice::GetCommandQueue(type);
		}

		Result CreateBuffer(const BufferDesc& desc, Ref<Buffer>& out) const override {
			const bool staging = desc.memoryUsage == MemoryUsage::CPUToGPU && desc.usage == BufferUsage::TransferSrc;
			if (staging && m_FailStaging) {
				m_FailStaging = false;
				SendDebugMessage(
					DebugSeverity::Error, DebugMessageType::General, "Injected staging allocation failure.");
				return Result::OutOfMemory;
			}
			const auto result = VulkanDevice::CreateBuffer(desc, out);
			if (staging && result == Result::Success) {
				++m_StagingAllocations;
			}
			return result;
		}

		Result CreateCommandList(CommandAllocator* allocator, Ref<CommandList>& out) const override {
			const auto result = VulkanDevice::CreateCommandList(allocator, out);
			if (result == Result::Success) {
				++m_CommandLists;
			}
			return result;
		}

		uint32_t GetStagingAllocations() const { return m_StagingAllocations; }
		uint32_t GetCommandLists() const { return m_CommandLists; }
		void FailNextStagingAllocation() { m_FailStaging = true; }

	private:
		bool m_GraphicsOnly;
		mutable bool m_FailStaging = false;
		mutable uint32_t m_StagingAllocations = 0;
		mutable uint32_t m_CommandLists = 0;
	};

	Ref<Buffer> MakeBuffer(Device& device,
		uint64_t size,
		BufferUsage usage = BufferUsage::TransferDst | BufferUsage::TransferSrc,
		MemoryUsage memory = MemoryUsage::GPUOnly) {
		Ref<Buffer> buffer;
		Expect(device.CreateBuffer({.size = size, .usage = usage, .memoryUsage = memory}, buffer) == Result::Success,
			"Create buffer");
		return buffer;
	}

	Ref<Texture> MakeTexture(Device& device,
		uint32_t width,
		uint32_t height,
		uint32_t depth = 1,
		uint32_t mips = 1,
		uint32_t layers = 1,
		TextureFormat format = TextureFormat::RGBA8_UNorm) {
		Ref<Texture> texture;
		Expect(device.CreateTexture({.width = width,
										.height = height,
										.depth = depth,
										.mipLevels = mips,
										.arrayLayers = layers,
										.format = format,
										.usage = TextureUsage::TransferDst | TextureUsage::TransferSrc},
				   texture) == Result::Success,
			"Create texture");
		return texture;
	}

	std::unique_ptr<UploadContext> MakeContext(Device& device,
		uint64_t pageSize = 128,
		QueueType queue = QueueType::Transfer) {
		std::unique_ptr<UploadContext> context;
		Expect(
			device.CreateUploadContext({.queueType = queue, .stagingPageSize = pageSize}, context) == Result::Success,
			"Create upload context");
		return context;
	}

	Ref<CommandList> MakeList(Device& device) {
		Ref<CommandAllocator> allocator;
		Ref<CommandList> list;
		Expect(device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success,
			"Create graphics allocator");
		Expect(device.CreateCommandList(allocator.Raw(), list) == Result::Success && list->Begin() == Result::Success,
			"Begin consumer");
		return list;
	}

	constexpr ResourceState transferRead{PipelineStage::Transfer, ResourceAccess::TransferRead};
	constexpr ResourceState transferWrite{PipelineStage::Transfer, ResourceAccess::TransferWrite};

	BufferUploadDesc BufferDestination(Buffer* buffer, uint64_t offset = 0) {
		return {.destination = buffer, .destinationOffset = offset, .finalState = transferRead};
	}

	TextureUploadDesc TextureDestination(Texture* texture, uint32_t mip = 0, uint32_t layer = 0, uint32_t count = 1) {
		return {.destination = texture,
			.mipLevel = mip,
			.baseArrayLayer = layer,
			.layerCount = count,
			.finalLayout = TextureLayout::TransferSrc,
			.finalState = transferRead};
	}

	SubmissionToken SubmitConsumer(Device& device, CommandList& list, SubmissionToken dependency) {
		Expect(list.End() == Result::Success, "End consumer");
		CommandList* lists[] = {&list};
		const auto token =
			device.GetCommandQueue(QueueType::Graphics)->Submit({.commandLists = lists, .waitFor = {&dependency, 1}});
		Expect(token.IsValid(), "Submit consumer dependency");
		device.GetCommandQueue(QueueType::Graphics)->Wait(token);
		return token;
	}

	void CheckBuffer(Device& device,
		Buffer* source,
		const UploadSubmission& upload,
		const std::vector<uint8_t>& expected,
		uint64_t offset = 0) {
		auto output = MakeBuffer(device, expected.size(), BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		auto list = MakeList(device);
		list->Barrier(upload.acquireBarriers);
		list->CopyBuffer(source, output.Raw(), {.sourceOffset = offset, .size = expected.size()});
		SubmitConsumer(device, *list, upload.completion);
		std::vector<uint8_t> actual(expected.size());
		Expect(output->ReadData(actual.data(), actual.size()) == Result::Success && actual == expected,
			"Uploaded buffer bytes match");
	}

	void TestMixedBatch(UploadTestDevice& device) {
		auto context = MakeContext(device, 64);
		auto first = MakeBuffer(device, 13);
		auto second = MakeBuffer(device, 32);
		auto texture = MakeTexture(device, 8, 4, 1, 4, 2);
		auto volume = MakeTexture(device, 4, 2, 2);
		auto smallTexture = MakeTexture(device, 3, 1, 1, 1, 1, TextureFormat::R8_UNorm);
		std::vector<uint8_t> firstBytes{1, 2, 3, 4, 5, 6, 7};
		const auto firstExpected = firstBytes;
		std::vector<uint8_t> secondBytes(32, 117);
		std::vector<std::vector<uint8_t>> mipBytes;
		for (uint32_t mip = 0; mip < 4; ++mip) {
			const uint32_t width = std::max(1u, 8u >> mip);
			const uint32_t height = std::max(1u, 4u >> mip);
			std::vector<uint8_t> bytes(width * height * 4 * 2);
			for (size_t i = 0; i < bytes.size(); ++i) {
				bytes[i] = static_cast<uint8_t>((i + mip * 37) % 251);
			}
			mipBytes.push_back(std::move(bytes));
		}
		std::vector<uint8_t> volumeBytes(64, 193);
		const std::array<uint8_t, 3> smallBytes{11, 22, 33};
		Expect(context->BeginBatch() == Result::Success, "Begin mixed upload batch");
		Expect(context->UploadBuffer(BufferDestination(first.Raw(), 3), firstBytes.data(), firstBytes.size()) ==
				Result::Success,
			"Upload an unaligned, odd-sized buffer range");
		Expect(context->UploadTexture(TextureDestination(smallTexture.Raw()), smallBytes.data(), smallBytes.size()) ==
				Result::Success,
			"Suballocate aligned R8 texture data after an odd-sized buffer");
		Expect(context->UploadBuffer(BufferDestination(second.Raw()), secondBytes.data(), secondBytes.size()) ==
				Result::Success,
			"Upload second buffer");
		for (uint32_t mip = 0; mip < 4; ++mip) {
			Expect(context->UploadTexture(TextureDestination(texture.Raw(), mip, 0, 2),
					   mipBytes[mip].data(),
					   mipBytes[mip].size()) == Result::Success,
				"Upload mip and two array layers");
		}
		Expect(context->UploadTexture(TextureDestination(volume.Raw()), volumeBytes.data(), volumeBytes.size()) ==
				Result::Success,
			"Upload volume texture");
		firstBytes.assign(firstBytes.size(), 0);
		UploadSubmission upload;
		Expect(context->Submit(upload) == Result::Success && upload.completion.IsValid(),
			"Submit all uploads as one batch");
		const bool different = device.GetCommandQueue(QueueType::Transfer)->GetQueueId().familyIndex !=
			device.GetCommandQueue(QueueType::Graphics)->GetQueueId().familyIndex;
		Expect(upload.acquireBarriers.buffers.size() == (different ? 2u : 0u) &&
				upload.acquireBarriers.textures.size() == (different ? 6u : 0u),
			"Acquire barriers only exist for family transfers");
		auto output = MakeBuffer(device, 448, BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		auto list = MakeList(device);
		list->Barrier(upload.acquireBarriers);
		list->CopyBuffer(first.Raw(), output.Raw(), {.sourceOffset = 3, .size = 7});
		list->CopyBuffer(second.Raw(), output.Raw(), {.destinationOffset = 8, .size = 32});
		uint64_t offset = 40;
		for (uint32_t mip = 0; mip < 4; ++mip) {
			list->CopyTextureToBuffer(texture.Raw(),
				output.Raw(),
				{.width = std::max(1u, 8u >> mip),
					.height = std::max(1u, 4u >> mip),
					.bufferOffset = offset,
					.mipLevel = mip,
					.layerCount = 2});
			offset += mipBytes[mip].size();
		}
		list->CopyTextureToBuffer(
			volume.Raw(), output.Raw(), {.width = 4, .height = 2, .depth = 2, .bufferOffset = offset});
		SubmitConsumer(device, *list, upload.completion);
		std::vector<uint8_t> actual(448);
		Expect(output->ReadData(actual.data(), actual.size()) == Result::Success, "Read mixed upload bytes");
		Expect(std::equal(firstExpected.begin(), firstExpected.end(), actual.begin()),
			"Source data was captured before modification");
		Expect(std::equal(secondBytes.begin(), secondBytes.end(), actual.begin() + 8), "Second buffer matches");
		offset = 40;
		for (const auto& bytes : mipBytes) {
			Expect(
				std::equal(bytes.begin(), bytes.end(), actual.begin() + offset), "Every mip and array layer matches");
			offset += bytes.size();
		}
		Expect(std::equal(volumeBytes.begin(), volumeBytes.end(), actual.begin() + offset), "Volume texture matches");
		Expect(context->ProcessCompleted() == Result::Success, "Retire completed upload batches");
	}

	VKAPI_ATTR VkResult VKAPI_CALL ReportIncomplete(VkDevice, VkSemaphore, uint64_t* value) {
		*value = 0;
		return VK_SUCCESS;
	}

	VKAPI_ATTR VkResult VKAPI_CALL FailSubmit(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	VKAPI_ATTR VkResult VKAPI_CALL FailEnd(VkCommandBuffer) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	VKAPI_ATTR VkResult VKAPI_CALL FailReset(VkCommandBuffer, VkCommandBufferResetFlags) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	void TestRetirementAndReuse(UploadTestDevice& device) {
		auto context = MakeContext(device, 128);
		std::vector<uint8_t> bytes(64, 81);
		auto first = MakeBuffer(device, bytes.size());
		auto second = MakeBuffer(device, bytes.size());
		const auto startAllocations = device.GetStagingAllocations();
		const auto counterFunction = vkGetSemaphoreCounterValue;
		vkGetSemaphoreCounterValue = ReportIncomplete;
		Expect(context->BeginBatch() == Result::Success, "Begin first pending batch");
		Expect(context->UploadBuffer(BufferDestination(first.Raw()), bytes.data(), bytes.size()) == Result::Success,
			"Stage first pending upload");
		UploadSubmission firstUpload;
		Expect(context->Submit(firstUpload) == Result::Success, "Submit first pending batch");
		Expect(context->BeginBatch() == Result::Success, "Begin second batch without waiting");
		Expect(context->UploadBuffer(BufferDestination(second.Raw()), bytes.data(), bytes.size()) == Result::Success,
			"Stage second pending upload");
		UploadSubmission secondUpload;
		Expect(context->Submit(secondUpload) == Result::Success, "Submit second pending batch");
		Expect(device.GetStagingAllocations() == startAllocations + 2,
			"Incomplete submissions cannot share a staging page");
		vkGetSemaphoreCounterValue = counterFunction;
		CheckBuffer(device, first.Raw(), firstUpload, bytes);
		CheckBuffer(device, second.Raw(), secondUpload, bytes);
		const auto resetFunction = vkResetCommandBuffer;
		vkResetCommandBuffer = FailReset;
		ExpectRejected(
			[&] {
				return context->ProcessCompleted();
			},
			Result::Failed,
			"Failed to reset Vulkan");
		vkResetCommandBuffer = resetFunction;
		Expect(context->ProcessCompleted() == Result::Success, "Retry failed retirement before reusing pages");
		const auto commandLists = device.GetCommandLists();
		auto third = MakeBuffer(device, bytes.size());
		Expect(context->BeginBatch() == Result::Success, "Reuse retired command list");
		Expect(context->UploadBuffer(BufferDestination(third.Raw()), bytes.data(), bytes.size()) == Result::Success,
			"Reuse retired staging page");
		Expect(device.GetStagingAllocations() == startAllocations + 2 && device.GetCommandLists() == commandLists,
			"Completed batches reuse both staging pages and command resources");
		UploadSubmission thirdUpload;
		Expect(context->Submit(thirdUpload) == Result::Success, "Submit reused batch");
		// Destroying the context must not wait or release GPU-referenced staging buffers.
		context.reset();
		CheckBuffer(device, third.Raw(), thirdUpload, bytes);
	}

	void TestUpdateExistingResources(UploadTestDevice& device) {
		auto context = MakeContext(device, 128);
		auto buffer = MakeBuffer(device, 64);
		auto texture = MakeTexture(device, 4, 4);
		auto output = MakeBuffer(device, 128, BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		std::vector<uint8_t> original(64, 17);
		Expect(context->BeginBatch() == Result::Success, "Begin initial upload");
		Expect(
			context->UploadBuffer(BufferDestination(buffer.Raw()), original.data(), original.size()) == Result::Success,
			"Upload initial buffer");
		Expect(context->UploadTexture(TextureDestination(texture.Raw()), original.data(), original.size()) ==
				Result::Success,
			"Upload initial texture");
		UploadSubmission initial;
		Expect(context->Submit(initial) == Result::Success, "Submit initial upload");
		const bool different = device.GetCommandQueue(QueueType::Transfer)->GetQueueId().familyIndex !=
			device.GetCommandQueue(QueueType::Graphics)->GetQueueId().familyIndex;
		auto consumer = MakeList(device);
		consumer->Barrier(initial.acquireBarriers);
		consumer->CopyBuffer(buffer.Raw(), output.Raw(), {.size = 64});
		consumer->CopyTextureToBuffer(texture.Raw(), output.Raw(), {.width = 4, .height = 4, .bufferOffset = 64});
		if (different) {
			BarrierDesc release;
			release.buffers.push_back({.buffer = buffer.Raw(),
				.before = transferRead,
				.after = transferWrite,
				.size = 64,
				.sourceQueue = QueueType::Graphics,
				.destinationQueue = QueueType::Transfer});
			release.textures.push_back({.texture = texture.Raw(),
				.before = transferRead,
				.after = transferWrite,
				.oldLayout = TextureLayout::TransferSrc,
				.newLayout = TextureLayout::TransferDst,
				.sourceQueue = QueueType::Graphics,
				.destinationQueue = QueueType::Transfer});
			consumer->Barrier(release);
		}
		const auto released = SubmitConsumer(device, *consumer, initial.completion);
		std::array<uint8_t, 128> actual{};
		Expect(output->ReadData(actual.data(), actual.size()) == Result::Success &&
				std::all_of(actual.begin(),
					actual.end(),
					[](uint8_t value) {
						return value == 17;
					}),
			"Original upload matches");
		std::vector<uint8_t> updated(64, 219);
		Expect(context->BeginBatch() == Result::Success, "Begin update batch");
		auto bufferDesc = BufferDestination(buffer.Raw());
		auto textureDesc = TextureDestination(texture.Raw());
		textureDesc.oldLayout = TextureLayout::TransferSrc;
		if (different) {
			bufferDesc.sourceQueue = QueueType::Graphics;
			textureDesc.sourceQueue = QueueType::Graphics;
		}
		Expect(context->UploadBuffer(bufferDesc, updated.data(), updated.size()) == Result::Success,
			"Acquire and update buffer");
		Expect(context->UploadTexture(textureDesc, updated.data(), updated.size()) == Result::Success,
			"Acquire and update texture");
		UploadSubmission update;
		if (different) {
			ExpectRejected(
				[&] {
					return context->Submit(update);
				},
				Result::Failed,
				"waitFor");
		}
		Expect(context->Submit(update, {&released, 1}) == Result::Success,
			"Retry update with graphics release dependency");
		auto updatedOutput = MakeBuffer(device, 128, BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		auto verify = MakeList(device);
		verify->Barrier(update.acquireBarriers);
		verify->CopyBuffer(buffer.Raw(), updatedOutput.Raw(), {.size = 64});
		verify->CopyTextureToBuffer(texture.Raw(), updatedOutput.Raw(), {.width = 4, .height = 4, .bufferOffset = 64});
		SubmitConsumer(device, *verify, update.completion);
		Expect(updatedOutput->ReadData(actual.data(), actual.size()) == Result::Success &&
				std::all_of(actual.begin(),
					actual.end(),
					[](uint8_t value) {
						return value == 219;
					}),
			"Updated buffer and texture match");
	}

	void TestValidationAndRecovery(UploadTestDevice& device) {
		std::unique_ptr<UploadContext> invalidContext;
		ExpectRejected(
			[&] {
				return device.CreateUploadContext({.stagingPageSize = 0}, invalidContext);
			},
			Result::InvalidParameter,
			"nonzero staging page");
		ExpectRejected(
			[&] {
				return device.CreateUploadContext({.queueType = static_cast<QueueType>(999)}, invalidContext);
			},
			Result::InvalidParameter,
			"available queue");
		Expect(!invalidContext, "Invalid factory inputs publish no context");
		auto context = MakeContext(device, 128);
		auto buffer = MakeBuffer(device, 64);
		auto invalidUsage = MakeBuffer(device, 64, BufferUsage::TransferSrc);
		auto texture = MakeTexture(device, 4, 4);
		std::vector<uint8_t> bytes(64, 73);
		UploadSubmission upload;
		ExpectRejected(
			[&] {
				return context->Submit(upload);
			},
			Result::InvalidParameter,
			"Begin a batch");
		Expect(context->BeginBatch() == Result::Success, "Begin validation batch");
		ExpectRejected(
			[&] {
				return context->BeginBatch();
			},
			Result::InvalidParameter,
			"Submit or cancel");
		ExpectRejected(
			[&] {
				return context->Submit(upload);
			},
			Result::InvalidParameter,
			"at least one");
		ExpectRejected(
			[&] {
				return context->UploadBuffer(BufferDestination(invalidUsage.Raw()), bytes.data(), bytes.size());
			},
			Result::InvalidParameter,
			"TransferDst");
		ExpectRejected(
			[&] {
				return context->UploadBuffer(BufferDestination(buffer.Raw(), 1), bytes.data(), UINT64_MAX);
			},
			Result::InvalidParameter,
			"in-bounds");
		ExpectRejected(
			[&] {
				return context->UploadBuffer(BufferDestination(buffer.Raw()), nullptr, bytes.size());
			},
			Result::InvalidParameter,
			"non-null");
		ExpectRejected(
			[&] {
				return context->UploadTexture(
					TextureDestination(texture.Raw(), UINT32_MAX), bytes.data(), bytes.size());
			},
			Result::InvalidParameter,
			"mip/layers");
		ExpectRejected(
			[&] {
				return context->UploadTexture(TextureDestination(texture.Raw(), 0, 0, 2), bytes.data(), bytes.size());
			},
			Result::InvalidParameter,
			"mip/layers");
		ExpectRejected(
			[&] {
				return context->UploadTexture(TextureDestination(texture.Raw()), bytes.data(), 1);
			},
			Result::InvalidParameter,
			"complete tightly packed");
		auto invalidLayout = TextureDestination(texture.Raw());
		invalidLayout.finalLayout = TextureLayout::ShaderRead;
		ExpectRejected(
			[&] {
				return context->UploadTexture(invalidLayout, bytes.data(), bytes.size());
			},
			Result::InvalidParameter,
			"final layout");
		device.FailNextStagingAllocation();
		ExpectRejected(
			[&] {
				return context->UploadBuffer(BufferDestination(buffer.Raw()), bytes.data(), bytes.size());
			},
			Result::OutOfMemory,
			"staging allocation");
		Expect(context->UploadBuffer(BufferDestination(buffer.Raw()), bytes.data(), bytes.size()) == Result::Success,
			"Allocation failure does not poison batch");
		ExpectRejected(
			[&] {
				return context->UploadBuffer(BufferDestination(buffer.Raw(), 4), bytes.data(), 4);
			},
			Result::InvalidParameter,
			"overlap");
		Expect(context->UploadTexture(TextureDestination(texture.Raw()), bytes.data(), bytes.size()) == Result::Success,
			"Stage valid texture after rejected operations");
		ExpectRejected(
			[&] {
				return context->UploadTexture(TextureDestination(texture.Raw()), bytes.data(), bytes.size());
			},
			Result::InvalidParameter,
			"overlap");
		const auto endFunction = vkEndCommandBuffer;
		vkEndCommandBuffer = FailEnd;
		ExpectRejected(
			[&] {
				return context->Submit(upload);
			},
			Result::Failed,
			"Failed to end Vulkan");
		vkEndCommandBuffer = endFunction;
		const auto submitFunction = vkQueueSubmit2;
		vkQueueSubmit2 = FailSubmit;
		ExpectRejected(
			[&] {
				return context->Submit(upload);
			},
			Result::Failed,
			"Failed to submit Vulkan");
		vkQueueSubmit2 = submitFunction;
		Expect(!upload.completion.IsValid() && upload.acquireBarriers.buffers.empty(), "Failed submit clears output");
		ExpectRejected(
			[&] {
				return context->UploadBuffer(BufferDestination(buffer.Raw()), bytes.data(), bytes.size());
			},
			Result::InvalidParameter,
			"recording batch");
		Expect(context->Submit(upload) == Result::Success, "Retry failed native submission");
		CheckBuffer(device, buffer.Raw(), upload, bytes);
		Expect(context->ProcessCompleted() == Result::Success, "Retire completed upload batches");
		const auto pages = device.GetStagingAllocations();
		auto cancelled = MakeBuffer(device, 64);
		Expect(context->BeginBatch() == Result::Success &&
				context->UploadBuffer(BufferDestination(cancelled.Raw()), bytes.data(), bytes.size()) ==
					Result::Success,
			"Stage cancelled batch");
		const auto cancelEndFunction = vkEndCommandBuffer;
		vkEndCommandBuffer = FailEnd;
		ExpectRejected(
			[&] {
				return context->CancelBatch();
			},
			Result::Failed,
			"Failed to end Vulkan");
		vkEndCommandBuffer = cancelEndFunction;
		Expect(context->CancelBatch() == Result::Success, "Retry cancellation after End failure");
		Expect(context->BeginBatch() == Result::Success &&
				context->UploadBuffer(BufferDestination(cancelled.Raw()), bytes.data(), bytes.size()) ==
					Result::Success,
			"Reuse cancelled batch resources");
		Expect(device.GetStagingAllocations() == pages, "Cancellation returns staging pages to pool");
		Expect(context->Submit(upload) == Result::Success, "Cancelled commands were not published");
		CheckBuffer(device, cancelled.Raw(), upload, bytes);
	}

	void TestExplicitQueues(UploadTestDevice& device) {
		for (QueueType queue : {QueueType::Graphics, QueueType::Compute}) {
			auto context = MakeContext(device, 128, queue);
			auto buffer = MakeBuffer(device, 64);
			std::vector<uint8_t> bytes(64, 132);
			Expect(context->BeginBatch() == Result::Success &&
					context->UploadBuffer(BufferDestination(buffer.Raw()), bytes.data(), bytes.size()) ==
						Result::Success,
				"Stage upload on explicitly selected queue");
			UploadSubmission upload;
			Expect(context->Submit(upload) == Result::Success &&
					upload.completion.queue == device.GetCommandQueue(queue)->GetQueueId(),
				"Upload token identifies the selected queue");
			CheckBuffer(device, buffer.Raw(), upload, bytes);
		}
	}
}

int main() {
	for (bool graphicsOnly : {false, true}) {
		UploadTestDevice device(graphicsOnly);
		DeviceDesc desc{};
		desc.enableDebug = true;
		desc.enableValidationLayers = true;
		desc.debugCallback = CaptureDebugMessage;
		Expect(device.Init(desc) == Result::Success, "Initialize upload test device");
		std::cout << "Upload tests (graphics override=" << graphicsOnly << ")\n";
		TestMixedBatch(device);
		TestRetirementAndReuse(device);
		TestUpdateExistingResources(device);
		TestValidationAndRecovery(device);
		TestExplicitQueues(device);
		device.WaitIdle();
		Expect(g_Errors == g_ExpectedErrors, "No unexpected programmer errors");
	}
	Expect(g_Errors == g_ExpectedErrors, "No errors during device cleanup");
	std::cout << "UploadTests passed\n";
}
