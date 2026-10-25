#include "Device.h"
#include "Vulkan/VulkanDevice.h"

#include <volk.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
	using namespace RealRHI;
	uint32_t s_Errors = 0;
	uint32_t s_ExpectedErrors = 0;
	std::string s_LastError;
	PFN_vkCmdBeginDebugUtilsLabelEXT s_BeginLabel = nullptr;
	PFN_vkCmdEndDebugUtilsLabelEXT s_EndLabel = nullptr;
	PFN_vkCmdInsertDebugUtilsLabelEXT s_InsertLabel = nullptr;
	uint32_t s_BeginLabels = 0;
	uint32_t s_EndLabels = 0;
	uint32_t s_InsertLabels = 0;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties s_GetQueueProperties = nullptr;
	uint32_t s_TestTimestampBits = 0;

	struct NamedObject {
		VkObjectType type;
		std::string name;
	};

	struct DescriptorName {
		const char* pointer;
		std::string expected;
	};

	PFN_vkSetDebugUtilsObjectNameEXT s_SetObjectName = nullptr;
	PFN_vkWriteResourceDescriptorsEXT s_WriteResourceDescriptors = nullptr;
	PFN_vkWriteSamplerDescriptorsEXT s_WriteSamplerDescriptors = nullptr;
	std::vector<NamedObject> s_NamedObjects;
	std::vector<DescriptorName> s_DescriptorNames;
	bool s_FailObjectName = false;

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void CaptureDebugMessage(const DebugMessage& message) {
		DefaultDebugCallback(message);
		if (message.severity == DebugSeverity::Error) {
			Expect(message.type == DebugMessageType::General, "No Vulkan validation or shader errors");
			++s_Errors;
			s_LastError = message.message;
		}
	}

	template <typename Operation>
	void ExpectRejected(Operation operation, const char* cause) {
		const uint32_t before = s_Errors;
		operation();
		Expect(s_Errors == before + 1 && s_LastError.find(cause) != std::string::npos,
			"Invalid operation reports its cause exactly once");
		++s_ExpectedErrors;
	}

	VKAPI_ATTR void VKAPI_CALL BeginLabel(VkCommandBuffer commands, const VkDebugUtilsLabelEXT* label) {
		++s_BeginLabels;
		s_BeginLabel(commands, label);
	}

	VKAPI_ATTR void VKAPI_CALL EndLabel(VkCommandBuffer commands) {
		++s_EndLabels;
		s_EndLabel(commands);
	}

	VKAPI_ATTR void VKAPI_CALL InsertLabel(VkCommandBuffer commands, const VkDebugUtilsLabelEXT* label) {
		++s_InsertLabels;
		s_InsertLabel(commands, label);
	}

	VKAPI_ATTR VkResult VKAPI_CALL FailSubmit(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	VKAPI_ATTR VkResult VKAPI_CALL FailDescriptorWrite(VkDevice,
		uint32_t,
		const VkResourceDescriptorInfoEXT*,
		const VkHostAddressRangeEXT*) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	VKAPI_ATTR void VKAPI_CALL OverrideTimestampBits(VkPhysicalDevice device,
		uint32_t* count,
		VkQueueFamilyProperties* properties) {
		s_GetQueueProperties(device, count, properties);
		if (properties != nullptr) {
			for (uint32_t index = 0; index < *count; ++index) {
				properties[index].timestampValidBits = s_TestTimestampBits;
			}
		}
	}

	VKAPI_ATTR VkResult VKAPI_CALL WrappedQueryResults(VkDevice,
		VkQueryPool,
		uint32_t,
		uint32_t count,
		size_t,
		void* data,
		VkDeviceSize stride,
		VkQueryResultFlags flags) {
		Expect(count == 2 && flags == VK_QUERY_RESULT_64_BIT, "Read two completed 64-bit timestamps without waiting");
		uint64_t maximum = UINT64_MAX;
		if (s_TestTimestampBits < 64) {
			maximum = (uint64_t{1} << s_TestTimestampBits) - 1;
		}
		*static_cast<uint64_t*>(data) = maximum - 5;
		*reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(data) + stride) = 5;
		return VK_SUCCESS;
	}

	void TestTimestampWraparound(Device& device) {
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(static_cast<VulkanDevice&>(device).GetPhysicalDevice(), &properties);
		for (uint32_t bits : {0u, 8u, 64u}) {
			Ref<CommandAllocator> allocator;
			Ref<CommandList> commands;
			Expect(device.CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success,
				"Create wraparound allocator");
			s_GetQueueProperties = vkGetPhysicalDeviceQueueFamilyProperties;
			s_TestTimestampBits = bits;
			vkGetPhysicalDeviceQueueFamilyProperties = OverrideTimestampBits;
			const Result creation = device.CreateCommandList(allocator.Raw(), commands);
			vkGetPhysicalDeviceQueueFamilyProperties = s_GetQueueProperties;
			Expect(creation == Result::Success, "Create query pool with simulated counter width");
			if (bits == 0) {
				Expect(
					!commands->SupportsTimestamps(), "Queue without timestamp support does not allocate a query pool");
				commands->WriteTimestamp(nullptr);
				std::vector<GpuTimestamp> unsupported;
				Expect(commands->GetTimestampResults(unsupported) == Result::Success && unsupported.empty(),
					"Unsupported timestamps are empty no-ops");
				continue;
			}
			Expect(commands->Begin() == Result::Success, "Begin wraparound recording");
			commands->WriteTimestamp("Wraparound");
			commands->WriteTimestamp("Wraparound end");
			Expect(commands->End() == Result::Success, "End wraparound recording");
			auto* queue = device.GetGraphicsCommandQueue();
			const auto completion = queue->Submit(*commands);
			Expect(completion.IsValid(), "Submit wraparound probe");
			const auto nativeResults = vkGetQueryPoolResults;
			vkGetQueryPoolResults = WrappedQueryResults;
			queue->Wait(completion);
			vkGetQueryPoolResults = nativeResults;
			std::vector<GpuTimestamp> timestamps;
			Expect(commands->GetTimestampResults(timestamps) == Result::Success && timestamps.size() == 2,
				"Read wrapped timestamp results");
			const double expected = 11.0 * properties.limits.timestampPeriod / 1'000'000.0;
			Expect(std::abs(timestamps[1].milliseconds - expected) < 1e-12,
				"Valid-bit and full-width counter wrap convert to eleven ticks");
		}
	}

	void CheckResourcesEqual(const DeviceStatistics& actual, const DeviceStatistics& expected) {
		Expect(actual.buffers.liveCount == expected.buffers.liveCount &&
				actual.buffers.liveBytes == expected.buffers.liveBytes,
			"Buffer allocations return to baseline");
		Expect(actual.textures.liveCount == expected.textures.liveCount &&
				actual.textures.liveBytes == expected.textures.liveBytes,
			"Texture allocations return to baseline");
		Expect(actual.pipelines.liveCount == expected.pipelines.liveCount, "Pipelines return to baseline");
		Expect(actual.descriptors.bufferCount == expected.descriptors.bufferCount &&
				actual.descriptors.imageCount == expected.descriptors.imageCount &&
				actual.descriptors.samplerCount == expected.descriptors.samplerCount,
			"Live descriptor slots return to baseline");
	}

	VKAPI_ATTR VkResult VKAPI_CALL CaptureObjectName(VkDevice device, const VkDebugUtilsObjectNameInfoEXT* info) {
		Expect(info->objectHandle != 0 && info->objectType != VK_OBJECT_TYPE_UNKNOWN,
			"Native names identify an actual object");
		if (s_FailObjectName) {
			s_FailObjectName = false;
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		s_NamedObjects.push_back({info->objectType, info->pObjectName});
		Expect(s_SetObjectName != nullptr, "Debug naming dispatch is available");
		return s_SetObjectName(device, info);
	}

	void CaptureDescriptorName(const void* chain) {
		auto* extension = static_cast<const VkBaseInStructure*>(chain);
		while (extension != nullptr) {
			if (extension->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT) {
				const auto* name = reinterpret_cast<const VkDebugUtilsObjectNameInfoEXT*>(extension);
				Expect(name->objectType == VK_OBJECT_TYPE_UNKNOWN && name->objectHandle == 0,
					"Descriptor names use the descriptor-heap naming contract");
				s_DescriptorNames.push_back({name->pObjectName, name->pObjectName});
			}
			extension = extension->pNext;
		}
	}

	VKAPI_ATTR VkResult VKAPI_CALL CaptureResourceNames(VkDevice device,
		uint32_t count,
		const VkResourceDescriptorInfoEXT* resources,
		const VkHostAddressRangeEXT* descriptors) {
		for (uint32_t index = 0; index < count; ++index) {
			CaptureDescriptorName(resources[index].pNext);
		}
		return s_WriteResourceDescriptors(device, count, resources, descriptors);
	}

	VKAPI_ATTR VkResult VKAPI_CALL CaptureSamplerNames(VkDevice device,
		uint32_t count,
		const VkSamplerCreateInfo* samplers,
		const VkHostAddressRangeEXT* descriptors) {
		for (uint32_t index = 0; index < count; ++index) {
			CaptureDescriptorName(samplers[index].pNext);
		}
		return s_WriteSamplerDescriptors(device, count, samplers, descriptors);
	}

	void TestResourceNames(Device& device) {
		const bool enabled = static_cast<VulkanDevice&>(device).SupportsDebugLabels();
		const auto baseline = device.GetStatistics();
		s_NamedObjects.clear();
		s_DescriptorNames.clear();
		s_SetObjectName = vkSetDebugUtilsObjectNameEXT;
		s_WriteResourceDescriptors = vkWriteResourceDescriptorsEXT;
		s_WriteSamplerDescriptors = vkWriteSamplerDescriptorsEXT;
		vkSetDebugUtilsObjectNameEXT = CaptureObjectName;
		vkWriteResourceDescriptorsEXT = CaptureResourceNames;
		vkWriteSamplerDescriptorsEXT = CaptureSamplerNames;
		{
			std::string bufferName = "Named storage buffer";
			std::string textureName = "Named texture";
			std::string viewName = "Named custom view";
			std::string samplerName = "Named sampler";
			Ref<Buffer> buffer;
			Ref<Texture> texture;
			Ref<TextureView> view;
			Ref<Sampler> sampler;
			Expect(device.CreateBuffer({.size = 256, .usage = BufferUsage::Storage, .debugName = bufferName.c_str()},
					   buffer) == Result::Success,
				"Create a named buffer and its descriptor");
			Expect(device.CreateTexture(
					   {
						   .width = 16,
						   .height = 16,
						   .format = TextureFormat::RGBA8_UNorm,
						   .usage = TextureUsage::ShaderResource | TextureUsage::Storage | TextureUsage::RenderTarget,
						   .debugName = textureName.c_str(),
					   },
					   texture) == Result::Success,
				"Create a named image, attachment view, descriptors and default sampler");
			Expect(device.CreateTextureView(texture.Raw(),
					   {
						   .usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget,
						   .debugName = viewName.c_str(),
					   },
					   view) == Result::Success,
				"Create a named custom view");
			Expect(device.CreateSampler({.debugName = samplerName.c_str()}, sampler) == Result::Success,
				"Create a named sampler descriptor");
			bufferName.assign("changed");
			textureName.assign("changed");
			viewName.assign("changed");
			samplerName.assign("changed");

			Ref<Shader> graphicsShader;
			Ref<Shader> computeShader;
			Ref<Pipeline> graphicsPipeline;
			Ref<Pipeline> computePipeline;
			Expect(device.CreateShader({.moduleName = "draw",
										   .entryPoints =
											   {{"vertexMain", RealRHI::ShaderStage::Vertex},
												   {"fragmentMain", RealRHI::ShaderStage::Fragment}},
										   .debugName = "Named graphics shader"},
					   graphicsShader) == Result::Success,
				"Name a graphics shader module");
			Expect(device.CreateShader({.moduleName = "ownership_compute",
										   .entryPoints = {{"computeMain", RealRHI::ShaderStage::Compute}},
										   .debugName = "Named compute shader"},
					   computeShader) == Result::Success,
				"Name a compute shader module");
			Expect(device.CreateGraphicsPipeline(
					   {
						   .shader = graphicsShader.Raw(),
						   .vertexLayout =
							   {.bindings = {{.stride = sizeof(float) * 2}},
								   .attributes = {{.format = RealRHI::VertexFormat::Float2}}},
						   .depthState = {.depthTestEnable = false, .depthWriteEnable = false},
						   .renderTargetFormats = {.colorFormats = {TextureFormat::RGBA8_UNorm}},
						   .debugName = "Named graphics pipeline",
					   },
					   graphicsPipeline) == Result::Success,
				"Name a graphics pipeline");
			Expect(device.CreateComputePipeline({.shader = computeShader.Raw(), .debugName = "Named compute pipeline"},
					   computePipeline) == Result::Success,
				"Name a compute pipeline");

			for (uint32_t index = 0; index < 64; ++index) {
				const std::string name = "Temporary sampler " + std::to_string(index);
				Ref<Sampler> temporary;
				Expect(device.CreateSampler({.debugName = name.c_str()}, temporary) == Result::Success,
					"Name identical descriptors while reusing a slot");
			}
		}
		CheckResourcesEqual(device.GetStatistics(), baseline);
		if (enabled) {
			for (const auto& expected : std::vector<NamedObject>{
					 {VK_OBJECT_TYPE_BUFFER, "Named storage buffer"},
					 {VK_OBJECT_TYPE_IMAGE, "Named texture"},
					 {VK_OBJECT_TYPE_IMAGE_VIEW, "Named texture"},
					 {VK_OBJECT_TYPE_IMAGE_VIEW, "Named custom view"},
					 {VK_OBJECT_TYPE_SHADER_MODULE, "Named graphics shader"},
					 {VK_OBJECT_TYPE_SHADER_MODULE, "Named compute shader"},
					 {VK_OBJECT_TYPE_PIPELINE, "Named graphics pipeline"},
					 {VK_OBJECT_TYPE_PIPELINE, "Named compute pipeline"},
				 }) {
				bool found = false;
				for (const auto& actual : s_NamedObjects) {
					found |= actual.type == expected.type && actual.name == expected.name;
				}
				Expect(found, "Requested resource names reach Vulkan with the correct object type");
			}
			Expect(s_DescriptorNames.size() >= 70, "Names reach buffer, image and sampler descriptor writes");
			for (const auto& name : s_DescriptorNames) {
				Expect(name.pointer != nullptr && std::string(name.pointer) == name.expected,
					"Interned descriptor labels survive caller mutation, slot reuse, interning rehash and alias "
					"destruction");
			}

			s_FailObjectName = true;
			Ref<Buffer> failed;
			ExpectRejected(
				[&] {
					Expect(
						device.CreateBuffer({.size = 64, .usage = BufferUsage::TransferDst, .debugName = "Failed name"},
							failed) == Result::Failed,
						"Native naming failure is returned to the caller");
				},
				"SetDebugName:");
			Expect(!failed, "Failed creation publishes no named resource");
			CheckResourcesEqual(device.GetStatistics(), baseline);
		}
		else {
			Expect(s_NamedObjects.empty() && s_DescriptorNames.empty(), "Names are no-ops without debug utils");
		}
		vkSetDebugUtilsObjectNameEXT = s_SetObjectName;
		vkWriteResourceDescriptorsEXT = s_WriteResourceDescriptors;
		vkWriteSamplerDescriptorsEXT = s_WriteSamplerDescriptors;
		s_DescriptorNames.clear();
	}

	void TestUsageCounters(Device& device) {
		const auto before = device.GetStatistics();
		{
			Ref<Buffer> buffer;
			Ref<Texture> texture;
			Ref<TextureView> view;
			Ref<Sampler> sampler;
			Expect(device.CreateBuffer({.size = 4096, .usage = BufferUsage::Storage}, buffer) == Result::Success,
				"Create tracked buffer");
			Expect(device.CreateTexture(
					   {
						   .width = 16,
						   .height = 16,
						   .format = TextureFormat::RGBA8_UNorm,
						   .usage = TextureUsage::ShaderResource | TextureUsage::Storage,
					   },
					   texture) == Result::Success,
				"Create tracked texture");
			Expect(device.CreateTextureView(texture.Raw(), {.usage = TextureUsage::ShaderResource}, view) ==
					Result::Success,
				"Create tracked custom view");
			Expect(device.CreateSampler({}, sampler) == Result::Success, "Create tracked sampler");

			Ref<Shader> graphicsShader;
			Ref<Shader> computeShader;
			Ref<Pipeline> graphics;
			Ref<Pipeline> compute;
			Expect(device.CreateShader({.moduleName = "draw",
										   .entryPoints =
											   {{"vertexMain", RealRHI::ShaderStage::Vertex},
												   {"fragmentMain", RealRHI::ShaderStage::Fragment}}},
					   graphicsShader) == Result::Success,
				"Create graphics shader");
			Expect(device.CreateGraphicsPipeline(
					   {
						   .shader = graphicsShader.Raw(),
						   .vertexLayout =
							   {.bindings = {{.stride = sizeof(float) * 2}},
								   .attributes = {{.format = RealRHI::VertexFormat::Float2}}},
						   .depthState = {.depthTestEnable = false, .depthWriteEnable = false},
						   .renderTargetFormats = {.colorFormats = {TextureFormat::RGBA8_UNorm}},
					   },
					   graphics) == Result::Success,
				"Create tracked graphics pipeline");
			Expect(device.CreateShader({.moduleName = "ownership_compute",
										   .entryPoints = {{"computeMain", RealRHI::ShaderStage::Compute}}},
					   computeShader) == Result::Success,
				"Create compute shader");
			Expect(device.CreateComputePipeline({.shader = computeShader.Raw()}, compute) == Result::Success,
				"Create tracked compute pipeline");

			const auto current = device.GetStatistics();
			Expect(current.buffers.liveCount == before.buffers.liveCount + 1 &&
					current.buffers.liveBytes >= before.buffers.liveBytes + 4096,
				"Buffer count and native allocation bytes increase");
			Expect(current.textures.liveCount == before.textures.liveCount + 1 &&
					current.textures.liveBytes > before.textures.liveBytes,
				"Owned texture allocation is counted");
			Expect(current.pipelines.liveCount == before.pipelines.liveCount + 2, "Both pipeline types are counted");
			Expect(current.descriptors.bufferCount == before.descriptors.bufferCount + 1 &&
					current.descriptors.imageCount == before.descriptors.imageCount + 3 &&
					current.descriptors.samplerCount == before.descriptors.samplerCount + 2,
				"Descriptors include custom views, storage images, and default and independent samplers");
			Expect(current.descriptors.resourceBytes > before.descriptors.resourceBytes &&
					current.descriptors.resourceCapacityBytes >= current.descriptors.resourceReservedBytes,
				"Live descriptor bytes and reserved capacity are consistent");
			if (device.IsProfilingEnabled()) {
				Expect(
					current.graphicsPipelineCreation.sampleCount == before.graphicsPipelineCreation.sampleCount + 1 &&
						current.computePipelineCreation.sampleCount == before.computePipelineCreation.sampleCount + 1,
					"Both pipeline creation paths record CPU timings");
			}

			const auto saved = current;
			Ref<Buffer> invalid;
			ExpectRejected(
				[&] {
					Expect(device.CreateBuffer({.size = 0, .usage = BufferUsage::Storage}, invalid) ==
							Result::InvalidParameter,
						"Reject zero buffer size");
				},
				"CreateBuffer:");
			CheckResourcesEqual(device.GetStatistics(), saved);

			const auto nativeWrite = vkWriteResourceDescriptorsEXT;
			vkWriteResourceDescriptorsEXT = FailDescriptorWrite;
			ExpectRejected(
				[&] {
					Expect(device.CreateBuffer({.size = 64, .usage = BufferUsage::Storage}, invalid) != Result::Success,
						"Descriptor failure rejects the new buffer");
				},
				"Failed to write Vulkan buffer descriptor");
			vkWriteResourceDescriptorsEXT = nativeWrite;
			CheckResourcesEqual(device.GetStatistics(), saved);
		}
		const auto after = device.GetStatistics();
		CheckResourcesEqual(after, before);
		Expect(
			after.buffers.peakCount > before.buffers.liveCount && after.textures.peakBytes > before.textures.liveBytes,
			"Resource high-water marks survive destruction");
		Expect(after.descriptors.peakResourceBytes >= after.descriptors.resourceBytes &&
				after.descriptors.peakSamplerCount >= 2,
			"Descriptor high-water marks survive slot reuse");
	}

	bool TestQueueTimestamps(Device& device, QueueType type) {
		const auto baseline = device.GetStatistics();
		Ref<CommandAllocator> allocator;
		Ref<CommandList> commands;
		Expect(device.CreateCommandAllocator(type, allocator) == Result::Success, "Create profiling allocator");
		Expect(device.CreateCommandList(allocator.Raw(), commands) == Result::Success, "Create profiling command list");
		std::vector<GpuTimestamp> timestamps;
		if (!commands->SupportsTimestamps()) {
			commands->WriteTimestamp(nullptr);
			Expect(commands->GetTimestampResults(timestamps) == Result::Success && timestamps.empty(),
				"Disabled timestamp operations are empty no-ops");
			return false;
		}

		Expect(commands->GetTimestampResults(timestamps) == Result::NotReady && timestamps.empty(),
			"Initial queries are not presented as completed results");
		ExpectRejected(
			[&] {
				commands->WriteTimestamp("Before begin");
			},
			"WriteTimestamp:");
		ExpectRejected(
			[&] {
				commands->EndDebugLabel();
			},
			"EndDebugLabel:");
		Expect(commands->Begin() == Result::Success, "Begin timestamped recording");
		ExpectRejected(
			[&] {
				commands->BeginDebugLabel(nullptr);
			},
			"BeginDebugLabel:");
		ExpectRejected(
			[&] {
				commands->InsertDebugLabel("");
			},
			"InsertDebugLabel:");
		ExpectRejected(
			[&] {
				commands->WriteTimestamp(nullptr);
			},
			"WriteTimestamp:");
		commands->BeginDebugLabel("Copy probe");
		commands->BeginDebugLabel("Nested probe");
		commands->InsertDebugLabel("Before copy");
		commands->WriteTimestamp("Copy probe");

		constexpr uint64_t byteCount = 2 * 1024 * 1024;
		std::vector<uint8_t> data(byteCount, 0x5a);
		Ref<Buffer> source;
		Ref<Buffer> destination;
		Expect(device.CreateBuffer({.size = byteCount,
									   .usage = BufferUsage::TransferSrc,
									   .memoryUsage = MemoryUsage::CPUToGPU,
									   .initialData = data.data()},
				   source) == Result::Success,
			"Create copy source");
		Expect(device.CreateBuffer(
				   {.size = byteCount, .usage = BufferUsage::TransferDst, .memoryUsage = MemoryUsage::GPUToCPU},
				   destination) == Result::Success,
			"Create copy destination");
		BarrierDesc barriers;
		barriers.buffers.push_back({
			.buffer = destination.Raw(),
			.before = {PipelineStage::AllCommands, ResourceAccess::MemoryWrite},
			.after = {PipelineStage::Transfer, ResourceAccess::TransferWrite},
			.size = byteCount,
		});
		commands->Barrier(barriers);
		commands->CopyBuffer(source.Raw(), destination.Raw(), {.size = byteCount});
		commands->WriteTimestamp("Copy end");
		commands->EndDebugLabel();
		commands->EndDebugLabel();
		Expect(commands->End() == Result::Success, "End timestamped recording after invalid markers were skipped");
		Expect(commands->GetTimestampResults(timestamps) == Result::NotReady,
			"Unsubmitted recording does not expose stale queries");

		auto* queue = device.GetCommandQueue(type);
		for (uint32_t replay = 0; replay < 2; ++replay) {
			const SubmissionToken completion = queue->Submit(*commands);
			Expect(completion.IsValid(), "Submit timestamped recording or replay");
			Expect(commands->GetTimestampResults(timestamps) == Result::NotReady && timestamps.empty(),
				"Pending results remain unavailable even when older query values existed");
			queue->Wait(completion);
			Expect(commands->GetTimestampResults(timestamps) == Result::Success && timestamps.size() == 2,
				"Completed recording has exactly two timestamp results");
			Expect(timestamps[0].name == "Copy probe" && timestamps[1].name == "Copy end" &&
					timestamps[0].milliseconds == 0 && std::isfinite(timestamps[1].milliseconds) &&
					timestamps[1].milliseconds >= 0,
				"Timestamps preserve names and convert to relative milliseconds");
		}
		Expect(destination->ReadData(data.data(), byteCount) == Result::Success && data.front() == 0x5a &&
				data.back() == 0x5a,
			"Profiling preserves copy results");

		const auto beforeFailure = device.GetStatistics();
		const auto nativeSubmit = vkQueueSubmit2;
		vkQueueSubmit2 = FailSubmit;
		ExpectRejected(
			[&] {
				Expect(!queue->Submit(*commands).IsValid(), "Native submit failure returns no token");
			},
			"Failed to submit");
		vkQueueSubmit2 = nativeSubmit;
		Expect(device.GetStatistics().submissions.sampleCount == beforeFailure.submissions.sampleCount,
			"Failed submits do not inflate successful submission timings");
		const SubmissionToken retry = queue->Submit(*commands);
		Expect(retry.IsValid(), "Retry timestamped submission after native failure");
		queue->Wait(retry);
		queue->Wait(retry);
		const auto after = device.GetStatistics();
		Expect(after.submissions.sampleCount == baseline.submissions.sampleCount + 3,
			"Only successful submissions contribute CPU samples");
		bool found = false;
		for (const auto& interval : after.gpuTimings) {
			if (interval.name == "Copy probe" && interval.queue == queue->GetQueueId()) {
				uint64_t previousSamples = 0;
				for (const auto& previous : baseline.gpuTimings) {
					if (previous.name == interval.name && previous.queue == interval.queue) {
						previousSamples = previous.timing.sampleCount;
					}
				}
				Expect(interval.timing.sampleCount == previousSamples + 3,
					"Completed intervals are collected exactly once and separated by queue");
				found = true;
			}
		}
		Expect(found, "GPU interval statistics contain the copy probe");

		source.Reset();
		destination.Reset();
		Expect(device.GetStatistics().buffers.liveCount == baseline.buffers.liveCount + 2,
			"Recorded buffers stay counted after application references are released");
		Expect(commands->Reset() == Result::Success, "Reset completed timestamped recording");
		Expect(commands->GetTimestampResults(timestamps) == Result::NotReady && timestamps.empty(),
			"Reset discards previous query results");
		CheckResourcesEqual(device.GetStatistics(), baseline);

		Expect(commands->Begin() == Result::Success, "Begin marker capacity test");
		for (uint32_t marker = 0; marker < 128; ++marker) {
			commands->WriteTimestamp("Capacity");
		}
		ExpectRejected(
			[&] {
				commands->WriteTimestamp("Overflow");
			},
			"fewer than 128");
		Expect(commands->End() == Result::Success, "End valid recording after rejecting excess marker");
		const auto capacity = queue->Submit(*commands);
		Expect(capacity.IsValid(), "Submit markers up to query pool capacity");
		queue->Wait(capacity);
		Expect(commands->GetTimestampResults(timestamps) == Result::Success && timestamps.size() == 128,
			"Full query pool reads only written markers");
		return true;
	}

	void TestUploads(Device& device, bool supportsTimestamps) {
		const auto before = device.GetStatistics();
		{
			std::unique_ptr<UploadContext> uploads;
			Expect(device.CreateUploadContext({}, uploads) == Result::Success, "Create measured uploader");
			Ref<Buffer> buffer;
			Expect(device.CreateBuffer({.size = 256, .usage = BufferUsage::TransferDst}, buffer) == Result::Success,
				"Create upload destination");
			std::array<uint8_t, 256> data{};
			Expect(uploads->BeginBatch() == Result::Success, "Begin cancelled upload");
			Expect(uploads->UploadBuffer({.destination = buffer.Raw(), .finalQueue = QueueType::Transfer},
					   data.data(),
					   data.size()) == Result::Success,
				"Stage cancelled upload");
			Expect(uploads->CancelBatch() == Result::Success, "Cancel measured upload");
			Expect(uploads->BeginBatch() == Result::Success, "Begin submitted upload");
			Expect(uploads->UploadBuffer({.destination = buffer.Raw(), .finalQueue = QueueType::Transfer},
					   data.data(),
					   data.size()) == Result::Success,
				"Stage submitted upload");
			UploadSubmission submission;
			Expect(uploads->Submit(submission) == Result::Success, "Submit measured upload");
			uploads.reset();
			device.GetCommandQueue(QueueType::Transfer)->Wait(submission.completion);
		}
		const auto after = device.GetStatistics();
		CheckResourcesEqual(after, before);
		if (device.IsProfilingEnabled()) {
			Expect(after.uploadStaging.sampleCount == before.uploadStaging.sampleCount + 2 &&
					after.stagedUploadBytes == before.stagedUploadBytes + 512,
				"Staging measurements include successfully staged bytes even for cancelled batches");
			Expect(after.uploadBatchCount == before.uploadBatchCount + 1 &&
					after.submittedUploadBytes == before.submittedUploadBytes + 256,
				"Only submitted batches count toward upload throughput");
			bool found = false;
			for (const auto& interval : after.gpuTimings) {
				if (interval.name == "Upload") {
					found = interval.timing.sampleCount == 1;
				}
			}
			Expect(found == supportsTimestamps,
				"Upload GPU timing survives uploader destruction when the queue supports timestamps");
		}
	}

	void RunTests(bool profiling, bool validation) {
		std::unique_ptr<Device> device;
		Expect(Device::Create(
				   {
					   .applicationName = "ProfilingTests",
					   .shaderDirectory = REALRHI_TEST_SHADER_DIRECTORY,
					   .enableDebug = true,
					   .enableValidationLayers = validation,
					   .debugCallback = CaptureDebugMessage,
					   .enableProfiling = profiling,
				   },
				   device) == Result::Success,
			"Create profiling test device");
		Expect(device->IsProfilingEnabled() == profiling, "Device preserves requested profiling mode");
		Expect(vkCmdBeginDebugUtilsLabelEXT != nullptr && vkCmdEndDebugUtilsLabelEXT != nullptr &&
				vkCmdInsertDebugUtilsLabelEXT != nullptr,
			"Debug labels are available independently of validation");
		s_BeginLabel = vkCmdBeginDebugUtilsLabelEXT;
		s_EndLabel = vkCmdEndDebugUtilsLabelEXT;
		s_InsertLabel = vkCmdInsertDebugUtilsLabelEXT;
		vkCmdBeginDebugUtilsLabelEXT = BeginLabel;
		vkCmdEndDebugUtilsLabelEXT = EndLabel;
		vkCmdInsertDebugUtilsLabelEXT = InsertLabel;

		TestResourceNames(*device);
		TestUsageCounters(*device);
		const bool graphicsTimestamps = TestQueueTimestamps(*device, QueueType::Graphics);
		TestQueueTimestamps(*device, QueueType::Compute);
		const bool transferTimestamps = TestQueueTimestamps(*device, QueueType::Transfer);
		TestUploads(*device, transferTimestamps);
		if (graphicsTimestamps) {
			TestTimestampWraparound(*device);
		}
		device->WaitIdle();
		if (!profiling) {
			const auto statistics = device->GetStatistics();
			Expect(statistics.gpuTimings.empty() && statistics.uploadStaging.sampleCount == 0 &&
					statistics.submissions.sampleCount == 0 && statistics.graphicsPipelineCreation.sampleCount == 0 &&
					statistics.computePipelineCreation.sampleCount == 0,
				"Disabled profiling records no timing samples");
		}
		vkCmdBeginDebugUtilsLabelEXT = s_BeginLabel;
		vkCmdEndDebugUtilsLabelEXT = s_EndLabel;
		vkCmdInsertDebugUtilsLabelEXT = s_InsertLabel;
		device.reset();
		Expect(s_Errors == s_ExpectedErrors, "No unexpected profiling or cleanup errors");
	}
}

int main() {
	RunTests(true, true);
	RunTests(true, false);
	RunTests(false, true);
	{
		std::unique_ptr<RealRHI::Device> device;
		Expect(RealRHI::Device::Create(
				   {
					   .applicationName = "Disabled debug names",
					   .shaderDirectory = REALRHI_TEST_SHADER_DIRECTORY,
					   .debugCallback = CaptureDebugMessage,
				   },
				   device) == RealRHI::Result::Success,
			"Create a device without debug utils");
		TestResourceNames(*device);
	}
	Expect(s_BeginLabels != 0 && s_BeginLabels == s_EndLabels && s_InsertLabels != 0,
		"Nested labels and point markers reach Vulkan and remain balanced");
	std::cout << "ProfilingTests passed\n";
}
