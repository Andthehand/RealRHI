#include "Device.h"
#include "Vulkan/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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
			Expect(message.type == DebugMessageType::General, "No Vulkan validation errors");
			++g_Errors;
			g_LastError = message.message;
		}
	}

	template <typename Operation>
	void ExpectRejected(Operation operation, const char* cause) {
		const uint32_t before = g_Errors;
		operation();
		Expect(g_Errors == before + 1 && g_LastError.find(cause) != std::string::npos,
			"Invalid operation identifies its cause exactly once");
		++g_ExpectedErrors;
	}

	class TextureTestDevice final : public VulkanDevice {
	public:
		explicit TextureTestDevice(bool graphicsOnly)
			: m_GraphicsOnly(graphicsOnly) { }

		using VulkanDevice::GetCommandQueue;

		CommandQueue* GetCommandQueue(QueueType type) const override {
			if (m_GraphicsOnly &&
				(type == QueueType::Graphics || type == QueueType::Compute || type == QueueType::Transfer)) {
				return GetGraphicsCommandQueue();
			}

			return VulkanDevice::GetCommandQueue(type);
		}

	private:
		bool m_GraphicsOnly;
	};

	Ref<CommandList> MakeList(Device& device) {
		Ref<CommandAllocator> allocator;
		Ref<CommandList> list;
		Expect(device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success, "Create allocator");
		Expect(device.CreateCommandList(allocator.Raw(), list) == Result::Success, "Create command list");
		Expect(list->Begin() == Result::Success, "Begin command list");

		return list;
	}

	Ref<Buffer> MakeBuffer(Device& device, uint64_t size, BufferUsage usage, MemoryUsage memory) {
		Ref<Buffer> buffer;
		Expect(device.CreateBuffer({.size = size, .usage = usage, .memoryUsage = memory}, buffer) == Result::Success,
			"Create buffer");

		return buffer;
	}

	Ref<Texture> MakeTexture(Device& device,
		uint32_t width,
		uint32_t height,
		uint32_t depth,
		uint32_t mips,
		uint32_t layers,
		TextureUsage usage) {
		Ref<Texture> texture;
		Expect(device.CreateTexture(
				   {
					   .width = width,
					   .height = height,
					   .depth = depth,
					   .mipLevels = mips,
					   .arrayLayers = layers,
					   .format = TextureFormat::RGBA8_UNorm,
					   .usage = usage,
				   },
				   texture) == Result::Success,
			"Create texture");

		return texture;
	}

	void Transition(CommandList& list,
		Texture* texture,
		TextureLayout before,
		TextureLayout after,
		TextureSubresourceRange range) {
		BarrierDesc barriers;
		barriers.textures.push_back({
			.texture = texture,
			.before = {PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite},
			.after = {PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite},
			.oldLayout = before,
			.newLayout = after,
			.subresources = range,
		});

		list.Barrier(barriers);
	}

	void SubmitAndWait(Device& device, CommandList& list, std::span<const SubmissionToken> waits = {}) {
		Expect(list.End() == Result::Success, "End command list");

		CommandList* lists[] = {&list};
		auto* queue = device.GetGraphicsCommandQueue();
		const auto token = queue->Submit({.commandLists = lists, .waitFor = waits});
		Expect(token.IsValid(), "Submit valid texture work");
		queue->Wait(token);
	}

	void TestMipGeneration(Device& device, uint32_t width, uint32_t height, uint32_t depth, uint32_t layers) {
		const uint32_t mipCount = 3;
		auto texture = MakeTexture(device,
			width,
			height,
			depth,
			mipCount,
			layers,
			TextureUsage::ShaderResource | TextureUsage::TransferSrc | TextureUsage::TransferDst);
		std::vector<uint8_t> pixels(width * height * depth * layers * 4);
		for (uint32_t layer = 0; layer < layers; ++layer) {
			for (uint32_t pixel = 0; pixel < width * height * depth; ++pixel) {
				const uint32_t offset = (layer * width * height * depth + pixel) * 4;
				pixels[offset] = static_cast<uint8_t>(32 + layer * 96);
				pixels[offset + 1] = 64;
				pixels[offset + 2] = 192;
				pixels[offset + 3] = 255;
			}
		}

		std::unique_ptr<UploadContext> uploads;
		Expect(device.CreateUploadContext({}, uploads) == Result::Success, "Create transfer uploader");
		Expect(uploads->BeginBatch() == Result::Success, "Begin upload");
		Expect(uploads->UploadTexture(
				   {
					   .destination = texture.Raw(),
					   .layerCount = layers,
					   .finalLayout = TextureLayout::TransferDst,
					   .finalState = {PipelineStage::Transfer, ResourceAccess::TransferWrite},
				   },
				   pixels.data(),
				   pixels.size()) == Result::Success,
			"Upload base mip");

		UploadSubmission upload;
		Expect(uploads->Submit(upload) == Result::Success, "Submit base mip upload");

		auto list = MakeList(device);
		list->Barrier(upload.acquireBarriers);
		list->GenerateMipmaps(texture.Raw(),
			{
				.finalLayout = TextureLayout::TransferSrc,
				.finalState = {PipelineStage::Transfer, ResourceAccess::TransferRead},
			});

		uint64_t byteCount = 0;
		for (uint32_t mip = 0; mip < mipCount; ++mip) {
			byteCount +=
				std::max(1u, width >> mip) * std::max(1u, height >> mip) * std::max(1u, depth >> mip) * layers * 4;
		}

		auto output = MakeBuffer(device, byteCount, BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		uint64_t offset = 0;
		for (uint32_t mip = 0; mip < mipCount; ++mip) {
			const uint32_t mipWidth = std::max(1u, width >> mip);
			const uint32_t mipHeight = std::max(1u, height >> mip);
			const uint32_t mipDepth = std::max(1u, depth >> mip);
			list->CopyTextureToBuffer(texture.Raw(),
				output.Raw(),
				{
					.width = mipWidth,
					.height = mipHeight,
					.depth = mipDepth,
					.bufferOffset = offset,
					.mipLevel = mip,
					.layerCount = layers,
				});
			offset += mipWidth * mipHeight * mipDepth * layers * 4;
		}

		SubmitAndWait(device, *list, {&upload.completion, 1});

		std::vector<uint8_t> actual(byteCount);
		Expect(output->ReadData(actual.data(), byteCount) == Result::Success, "Read generated mip chain");

		offset = 0;
		for (uint32_t mip = 0; mip < mipCount; ++mip) {
			const uint32_t pixelCount =
				std::max(1u, width >> mip) * std::max(1u, height >> mip) * std::max(1u, depth >> mip);
			for (uint32_t layer = 0; layer < layers; ++layer) {
				for (uint32_t pixel = 0; pixel < pixelCount; ++pixel) {
					Expect(actual[offset] == 32 + layer * 96 && actual[offset + 1] == 64 && actual[offset + 2] == 192 &&
							actual[offset + 3] == 255,
						"Every mip and layer preserves its source color");
					offset += 4;
				}
			}
		}
	}

	void TestFilteredMipRange(Device& device) {
		auto texture = MakeTexture(device, 8, 8, 1, 4, 2, TextureUsage::TransferSrc | TextureUsage::TransferDst);
		std::array<uint8_t, 64> gradient{};
		for (uint32_t y = 0; y < 4; ++y) {
			for (uint32_t x = 0; x < 4; ++x) {
				gradient[(y * 4 + x) * 4] = static_cast<uint8_t>(x * 64);
			}
		}

		std::array<uint8_t, 16> untouched;
		untouched.fill(230);

		std::unique_ptr<UploadContext> uploads;
		Expect(device.CreateUploadContext({.queueType = QueueType::Graphics}, uploads) == Result::Success,
			"Create range uploader");
		Expect(uploads->BeginBatch() == Result::Success, "Begin range upload");
		Expect(uploads->UploadTexture(
				   {
					   .destination = texture.Raw(),
					   .mipLevel = 1,
					   .baseArrayLayer = 1,
					   .finalLayout = TextureLayout::TransferDst,
					   .finalState = {PipelineStage::Transfer, ResourceAccess::TransferWrite},
				   },
				   gradient.data(),
				   gradient.size()) == Result::Success,
			"Upload gradient to source mip/layer");
		Expect(uploads->UploadTexture(
				   {
					   .destination = texture.Raw(),
					   .mipLevel = 2,
					   .finalLayout = TextureLayout::TransferDst,
					   .finalState = {PipelineStage::Transfer, ResourceAccess::TransferWrite},
				   },
				   untouched.data(),
				   untouched.size()) == Result::Success,
			"Upload a neighboring layer");

		UploadSubmission upload;
		Expect(uploads->Submit(upload) == Result::Success, "Submit range upload");

		auto list = MakeList(device);
		list->GenerateMipmaps(texture.Raw(),
			{
				.baseMipLevel = 1,
				.mipLevelCount = 2,
				.baseArrayLayer = 1,
				.arrayLayerCount = 1,
				.finalLayout = TextureLayout::TransferSrc,
				.finalState = {PipelineStage::Transfer, ResourceAccess::TransferRead},
			});

		Transition(*list, texture.Raw(), TextureLayout::TransferDst, TextureLayout::TransferSrc, {2, 1, 0, 1});

		auto output = MakeBuffer(device, 32, BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		list->CopyTextureToBuffer(texture.Raw(),
			output.Raw(),
			{
				.width = 2,
				.height = 2,
				.mipLevel = 2,
				.layerCount = 2,
			});
		SubmitAndWait(device, *list, {&upload.completion, 1});

		std::array<uint8_t, 32> pixels{};
		Expect(output->ReadData(pixels.data(), pixels.size()) == Result::Success, "Read filtered mip and its neighbor");
		Expect(std::equal(untouched.begin(), untouched.end(), pixels.begin()),
			"Neighboring layer data and layout remain intact");
		Expect(pixels[16] == 32 && pixels[20] == 160 && pixels[24] == 32 && pixels[28] == 160,
			"Linear blits average source texels within the selected mip range");
	}

	void TestStorageView(Device& device) {
		auto texture = MakeTexture(device,
			8,
			8,
			1,
			3,
			2,
			TextureUsage::Storage | TextureUsage::ShaderResource | TextureUsage::TransferSrc |
				TextureUsage::TransferDst);
		Ref<TextureView> view;
		Expect(device.CreateTextureView(texture.Raw(),
				   {
					   .baseMipLevel = 1,
					   .baseArrayLayer = 1,
					   .usage = TextureUsage::Storage,
				   },
				   view) == Result::Success,
			"Create a storage view of one mip/layer");
		Expect(view->GetStorageImageDescriptorIndex() != UINT32_MAX &&
				view->GetSampledImageDescriptorIndex() == UINT32_MAX,
			"View usage selects its descriptors");

		std::array<uint8_t, 64> initial{};
		std::unique_ptr<UploadContext> uploads;
		Expect(device.CreateUploadContext({.queueType = QueueType::Graphics}, uploads) == Result::Success,
			"Create storage uploader");
		Expect(uploads->BeginBatch() == Result::Success, "Begin storage upload");
		Expect(uploads->UploadTexture(
				   {
					   .destination = texture.Raw(),
					   .mipLevel = 1,
					   .baseArrayLayer = 1,
					   .finalLayout = TextureLayout::General,
					   .finalState = {PipelineStage::ComputeShader, ResourceAccess::ShaderWrite},
				   },
				   initial.data(),
				   initial.size()) == Result::Success,
			"Initialize selected storage mip/layer");

		UploadSubmission upload;
		Expect(uploads->Submit(upload) == Result::Success, "Submit storage upload");

		Ref<Shader> shader;
		Ref<Pipeline> pipeline;
		Expect(device.CreateShader(
				   {.moduleName = "ownership_texture", .entryPoints = {{"computeMain", RealRHI::ShaderStage::Compute}}},
				   shader) == Result::Success,
			"Compile storage shader");
		Expect(device.CreateComputePipeline({.shader = shader.Raw()}, pipeline) == Result::Success,
			"Create storage pipeline");

		auto list = MakeList(device);
		list->BindPipeline(pipeline.Raw());
		list->UseResource(view.Raw());
		const uint32_t index = view->GetStorageImageDescriptorIndex();
		list->SetPushConstants(&index, sizeof(index));
		list->DispatchCompute(1, 1, 1);

		Transition(*list, texture.Raw(), TextureLayout::General, TextureLayout::TransferSrc, {1, 1, 1, 1});
		auto output = MakeBuffer(device, 64, BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		list->CopyTextureToBuffer(texture.Raw(),
			output.Raw(),
			{
				.width = 4,
				.height = 4,
				.mipLevel = 1,
				.baseArrayLayer = 1,
			});
		SubmitAndWait(device, *list, {&upload.completion, 1});

		std::array<uint8_t, 64> actual{};
		Expect(output->ReadData(actual.data(), actual.size()) == Result::Success, "Read storage-view writes");
		Expect(std::all_of(actual.begin(),
				   actual.end(),
				   [](uint8_t value) {
					   return value == 255;
				   }),
			"Storage descriptors write the selected mip/layer");
	}

	void TestViewSamplingAndLifetime(Device& device) {
		auto texture = MakeTexture(device, 4, 4, 1, 3, 2, TextureUsage::ShaderResource | TextureUsage::TransferDst);
		std::unique_ptr<UploadContext> uploads;
		Expect(device.CreateUploadContext({.queueType = QueueType::Graphics}, uploads) == Result::Success,
			"Create uploader");
		Expect(uploads->BeginBatch() == Result::Success, "Begin selected subresource upload");

		// Leave unused subresources undefined: declaring a view must check only its selected range.
		for (uint32_t mip = 0; mip < 3; ++mip) {
			const uint32_t side = 4u >> mip;
			std::vector<uint8_t> pixels(side * side * 4, static_cast<uint8_t>(64 + mip * 64));
			uint32_t layer = 1;
			if (mip == 0) {
				layer = 0;
			}

			Expect(uploads->UploadTexture(
					   {
						   .destination = texture.Raw(),
						   .mipLevel = mip,
						   .baseArrayLayer = layer,
						   .finalState = {PipelineStage::ComputeShader, ResourceAccess::ShaderRead},
					   },
					   pixels.data(),
					   pixels.size()) == Result::Success,
				"Upload selected mip/layer");
		}

		UploadSubmission upload;
		Expect(uploads->Submit(upload) == Result::Success, "Submit sampling data");

		Ref<TextureView> firstView;
		Ref<TextureView> secondView;
		Expect(device.CreateTextureView(texture.Raw(),
				   {
					   .baseMipLevel = 1,
					   .mipLevelCount = 2,
					   .baseArrayLayer = 1,
				   },
				   firstView) == Result::Success,
			"Create mip/layer view");
		Expect(device.CreateTextureView(texture.Raw(), {}, secondView) == Result::Success, "Create base mip view");
		Expect(firstView->GetSampledImageDescriptorIndex() != secondView->GetSampledImageDescriptorIndex(),
			"Views have independent descriptors");

		Ref<Sampler> repeatSampler;
		Ref<Sampler> borderSampler;
		Expect(device.CreateSampler(
				   {
					   .minFilter = Filter::Nearest,
					   .magFilter = Filter::Nearest,
					   .mipFilter = Filter::Nearest,
					   .maxLod = 0.0f,
				   },
				   repeatSampler) == Result::Success,
			"Create repeat sampler with fixed LOD");
		Expect(device.CreateSampler(
				   {
					   .addressU = SamplerAddressMode::ClampToBorder,
					   .borderColor = SamplerBorderColor::OpaqueWhite,
				   },
				   borderSampler) == Result::Success,
			"Create linear sampler with white border");

		Ref<Shader> shader;
		Ref<Pipeline> pipeline;
		Expect(device.CreateShader(
				   {.moduleName = "texture_sampling", .entryPoints = {{"computeMain", RealRHI::ShaderStage::Compute}}},
				   shader) == Result::Success,
			"Compile sampling shader");
		Expect(device.CreateComputePipeline({.shader = shader.Raw()}, pipeline) == Result::Success,
			"Create sampling pipeline");

		auto result = MakeBuffer(
			device, 5 * sizeof(uint32_t), BufferUsage::Storage | BufferUsage::TransferSrc, MemoryUsage::GPUOnly);
		auto output = MakeBuffer(device, 5 * sizeof(uint32_t), BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		auto list = MakeList(device);
		list->BindPipeline(pipeline.Raw());
		list->UseResource(firstView.Raw());
		list->UseResource(secondView.Raw());
		list->UseResource(repeatSampler.Raw());
		list->UseResource(borderSampler.Raw());
		list->UseResource(result.Raw());

		const std::array<uint32_t, 5> parameters{
			firstView->GetSampledImageDescriptorIndex(),
			secondView->GetSampledImageDescriptorIndex(),
			repeatSampler->GetDescriptorIndex(),
			borderSampler->GetDescriptorIndex(),
			result->GetDescriptorIndex(),
		};
		list->SetPushConstants(parameters.data(), sizeof(parameters));
		list->DispatchCompute(1, 1, 1);

		BarrierDesc barriers;
		barriers.buffers.push_back({
			.buffer = result.Raw(),
			.before = {PipelineStage::ComputeShader, ResourceAccess::ShaderWrite},
			.after = {PipelineStage::Transfer, ResourceAccess::TransferRead},
			.size = 5 * sizeof(uint32_t),
		});
		list->Barrier(barriers);
		list->CopyBuffer(result.Raw(), output.Raw(), {.size = 5 * sizeof(uint32_t)});

		// The recorded list must keep both descriptor slots and their texture alive.
		texture.Release();
		firstView.Release();
		secondView.Release();
		repeatSampler.Release();
		borderSampler.Release();

		Ref<Sampler> extraSampler;
		Expect(device.CreateSampler({}, extraSampler) == Result::Success, "Allocate another sampler before submission");
		Expect(
			extraSampler->GetDescriptorIndex() != parameters[2] && extraSampler->GetDescriptorIndex() != parameters[3],
			"Recorded sampler descriptors cannot be recycled");

		SubmitAndWait(device, *list, {&upload.completion, 1});

		std::array<uint32_t, 5> actual{};
		Expect(output->ReadData(actual.data(), sizeof(actual)) == Result::Success, "Read sampled values");
		const std::array<uint32_t, 5> expected{128, 160, 64, 255, 128};
		Expect(
			actual == expected, "View selection, mip filtering, LOD clamping and address modes affect actual sampling");
		Expect(list->Reset() == Result::Success, "Reset releases declared views and samplers");
	}

	void TestAttachmentView(Device& device) {
		auto texture = MakeTexture(device, 4, 4, 1, 3, 2, TextureUsage::RenderTarget | TextureUsage::TransferSrc);
		Ref<TextureView> view;
		Expect(
			device.CreateTextureView(texture.Raw(), {.baseMipLevel = 1, .baseArrayLayer = 1}, view) == Result::Success,
			"Create attachment for one mip and layer");

		auto output = MakeBuffer(device, 16, BufferUsage::TransferDst, MemoryUsage::GPUToCPU);
		auto list = MakeList(device);
		Transition(*list, texture.Raw(), TextureLayout::Undefined, TextureLayout::ColorAttachment, {1, 1, 1, 1});
		ExpectRejected(
			[&] {
				list->BeginRendering({
					.colorAttachments = {{.target = view.Raw()}},
					.renderArea = {.width = 4, .height = 4},
				});
			},
			"render area");
		list->BeginRendering({
			.colorAttachments = {{.target = view.Raw(), .clearColor = {0, 1, 0, 1}}},
			.renderArea = {.width = 2, .height = 2},
		});
		list->EndRendering();

		Transition(*list, texture.Raw(), TextureLayout::ColorAttachment, TextureLayout::TransferSrc, {1, 1, 1, 1});
		list->CopyTextureToBuffer(texture.Raw(),
			output.Raw(),
			{
				.width = 2,
				.height = 2,
				.mipLevel = 1,
				.baseArrayLayer = 1,
			});

		texture.Release();
		view.Release();
		SubmitAndWait(device, *list);

		std::array<uint8_t, 16> pixels{};
		Expect(output->ReadData(pixels.data(), pixels.size()) == Result::Success, "Read attachment mip/layer");
		for (uint32_t pixel = 0; pixel < 4; ++pixel) {
			Expect(pixels[pixel * 4] == 0 && pixels[pixel * 4 + 1] == 255 && pixels[pixel * 4 + 2] == 0 &&
					pixels[pixel * 4 + 3] == 255,
				"Custom attachment view clears the selected subresource");
		}
	}

	void TestValidation(Device& device) {
		auto texture = MakeTexture(device,
			4,
			4,
			1,
			3,
			1,
			TextureUsage::ShaderResource | TextureUsage::TransferSrc | TextureUsage::TransferDst);
		Ref<TextureView> view;
		for (const auto& desc : std::array<TextureViewDesc, 5>{
				 TextureViewDesc{.baseMipLevel = UINT32_MAX},
				 TextureViewDesc{.mipLevelCount = 0},
				 TextureViewDesc{.baseArrayLayer = UINT32_MAX},
				 TextureViewDesc{.type = TextureViewType::View3D},
				 TextureViewDesc{.usage = TextureUsage::Storage},
			 }) {
			ExpectRejected(
				[&] {
					Expect(device.CreateTextureView(texture.Raw(), desc, view) == Result::InvalidParameter,
						"Reject invalid view");
				},
				"CreateTextureView");
		}
		Expect(!view, "Failed view creation publishes no object");

		Ref<Sampler> sampler;
		for (const auto& desc : std::array<SamplerDesc, 3>{
				 SamplerDesc{.minFilter = static_cast<Filter>(999)},
				 SamplerDesc{.minLod = 3, .maxLod = 1},
				 SamplerDesc{.mipLodBias = std::numeric_limits<float>::quiet_NaN()},
			 }) {
			ExpectRejected(
				[&] {
					Expect(device.CreateSampler(desc, sampler) == Result::InvalidParameter, "Reject invalid sampler");
				},
				"CreateSampler");
		}
		Expect(!sampler, "Failed sampler creation publishes no object");

		auto list = MakeList(device);
		ExpectRejected(
			[&] {
				list->GenerateMipmaps(texture.Raw());
			},
			"Initialize the source mip");
		ExpectRejected(
			[&] {
				list->GenerateMipmaps(texture.Raw(), {.baseMipLevel = 999});
			},
			"in-bounds");
		ExpectRejected(
			[&] {
				list->GenerateMipmaps(texture.Raw(), {.mipLevelCount = 1});
			},
			"at least two");
		ExpectRejected(
			[&] {
				list->GenerateMipmaps(texture.Raw(), {.filter = static_cast<Filter>(999)});
			},
			"supported filter");
		SubmitAndWait(device, *list);
	}

	VKAPI_ATTR VkResult VKAPI_CALL FailImageDescriptor(VkDevice,
		uint32_t,
		const VkResourceDescriptorInfoEXT*,
		const VkHostAddressRangeEXT*) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	VKAPI_ATTR VkResult VKAPI_CALL FailSamplerDescriptor(VkDevice,
		uint32_t,
		const VkSamplerCreateInfo*,
		const VkHostAddressRangeEXT*) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	VKAPI_ATTR VkResult VKAPI_CALL FailNativeView(VkDevice,
		const VkImageViewCreateInfo*,
		const VkAllocationCallbacks*,
		VkImageView*) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	void TestCreationFailures(Device& device) {
		auto texture = MakeTexture(device, 4, 4, 1, 1, 1, TextureUsage::ShaderResource);
		Ref<TextureView> view;
		Expect(
			device.CreateTextureView(texture.Raw(), {}, view) == Result::Success, "Create reusable image descriptor");
		const uint32_t viewIndex = view->GetSampledImageDescriptorIndex();
		view.Release();
		const uint32_t references = texture->GetRefCount();

		const auto writeImage = vkWriteResourceDescriptorsEXT;
		vkWriteResourceDescriptorsEXT = FailImageDescriptor;
		ExpectRejected(
			[&] {
				Expect(device.CreateTextureView(texture.Raw(), {}, view) == Result::Failed,
					"Propagate descriptor write failure");
			},
			"Failed to write Vulkan image descriptor");
		vkWriteResourceDescriptorsEXT = writeImage;

		Expect(!view && texture->GetRefCount() == references, "Failed views release their texture reference");
		Expect(device.CreateTextureView(texture.Raw(), {}, view) == Result::Success, "Retry image view allocation");
		Expect(view->GetSampledImageDescriptorIndex() == viewIndex, "Failed image descriptor slot is reclaimed");

		Ref<Sampler> sampler;
		Expect(device.CreateSampler({}, sampler) == Result::Success, "Create reusable sampler descriptor");
		const uint32_t samplerIndex = sampler->GetDescriptorIndex();
		sampler.Release();

		const auto writeSampler = vkWriteSamplerDescriptorsEXT;
		vkWriteSamplerDescriptorsEXT = FailSamplerDescriptor;
		ExpectRejected(
			[&] {
				Expect(
					device.CreateSampler({}, sampler) == Result::Failed && !sampler, "Failed sampler is not published");
			},
			"Failed to write Vulkan sampler descriptor");
		vkWriteSamplerDescriptorsEXT = writeSampler;

		Expect(device.CreateSampler({}, sampler) == Result::Success, "Retry sampler allocation");
		Expect(sampler->GetDescriptorIndex() == samplerIndex, "Failed sampler descriptor slot is reclaimed");

		auto target = MakeTexture(device, 4, 4, 1, 1, 1, TextureUsage::RenderTarget);
		Ref<TextureView> attachment;
		const auto createView = vkCreateImageView;
		vkCreateImageView = FailNativeView;
		ExpectRejected(
			[&] {
				Expect(device.CreateTextureView(target.Raw(), {}, attachment) == Result::Failed,
					"Propagate native view failure");
			},
			"Failed to create Vulkan texture view");
		vkCreateImageView = createView;

		Expect(!attachment && target->GetRefCount() == 1, "Failed native view releases its parent");
		Expect(device.CreateTextureView(target.Raw(), {}, attachment) == Result::Success, "Retry native view creation");
	}

	void TestViewTypes(Device& device) {
		auto array = MakeTexture(device, 4, 4, 1, 1, 12, TextureUsage::ShaderResource);
		Ref<TextureView> view;
		Expect(device.CreateTextureView(array.Raw(),
				   {
					   .type = TextureViewType::View2DArray,
					   .arrayLayerCount = 12,
				   },
				   view) == Result::Success,
			"Create ordinary array view");

		for (TextureViewType type : {TextureViewType::Cube, TextureViewType::CubeArray}) {
			ExpectRejected(
				[&] {
					Expect(device.CreateTextureView(array.Raw(),
							   {
								   .type = type,
								   .arrayLayerCount = 6,
							   },
							   view) == Result::InvalidParameter,
						"Reject unsupported cube views");
				},
				"supported 2D");
		}

		Ref<Texture> depth;
		Expect(device.CreateTexture(
				   {
					   .width = 4,
					   .height = 4,
					   .format = TextureFormat::D32_Float_S8_UInt,
					   .usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource,
				   },
				   depth) == Result::Success,
			"Create sampled depth/stencil texture");
		Expect(device.CreateTextureView(depth.Raw(),
				   {
					   .usage = TextureUsage::ShaderResource,
				   },
				   view) == Result::Success,
			"Sample depth independently of stencil");

		auto volume = MakeTexture(device, 4, 4, 4, 3, 1, TextureUsage::ShaderResource);
		Expect(device.CreateTextureView(volume.Raw(),
				   {
					   .type = TextureViewType::View3D,
					   .baseMipLevel = 1,
					   .mipLevelCount = 2,
				   },
				   view) == Result::Success,
			"Create volume view of selected mips");
	}

	void RunTests(bool graphicsOnly) {
		TextureTestDevice device(graphicsOnly);
		Expect(device.Init({
				   .applicationName = "Texture API tests",

				   .shaderDirectory = REALRHI_TEST_SHADER_DIRECTORY,
				   .enableDebug = true,
				   .enableProfiling = false,
				   .enableValidationLayers = true,
				   .debugCallback = CaptureDebugMessage,
			   }) == Result::Success,
			"Initialize test device");

		TestMipGeneration(device, 7, 5, 1, 2);
		TestMipGeneration(device, 4, 2, 2, 1);
		TestFilteredMipRange(device);
		TestStorageView(device);
		TestViewSamplingAndLifetime(device);
		TestAttachmentView(device);
		TestValidation(device);
		TestCreationFailures(device);
		TestViewTypes(device);
		device.WaitIdle();

		Expect(g_Errors == g_ExpectedErrors, "No unexpected errors");
	}
}

int main() {
	RunTests(false);
	RunTests(true);
	Expect(g_Errors == g_ExpectedErrors, "No cleanup errors");
	std::cout << "Texture API tests passed\n";
}
