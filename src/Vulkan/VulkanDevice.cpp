#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION
#include "VulkanDevice.h"

#include "VulkanBuffer.h"
#include "VulkanCommandAllocator.h"
#include "VulkanCommandList.h"
#include "VulkanCommandQueue.h"
#include "VulkanConversions.h"
#include "VulkanDescriptorManager.h"
#include "VulkanPipeline.h"
#include "VulkanSampler.h"
#include "VulkanShader.h"
#include "VulkanSwapchain.h"
#include "VulkanUploadContext.h"
#include "VulkanWindow.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>

namespace RealRHI {
	namespace {
		template <typename T>
		void AppendCacheKey(std::string& key, T value) {
			static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
			// Serialize fields individually: struct padding and debug-name pointers are not identity.
			key.append(reinterpret_cast<const char*>(&value), sizeof(value));
		}

		void AppendBlendKey(std::string& key, const BlendState& blend) {
			AppendCacheKey(key, blend.enable);
			AppendCacheKey(key, blend.sourceColorFactor);
			AppendCacheKey(key, blend.destinationColorFactor);
			AppendCacheKey(key, blend.colorOperation);
			AppendCacheKey(key, blend.sourceAlphaFactor);
			AppendCacheKey(key, blend.destinationAlphaFactor);
			AppendCacheKey(key, blend.alphaOperation);
			AppendCacheKey(key, blend.writeMask);
		}

		std::string BuildGraphicsPipelineKey(const PipelineDesc& desc) {
			std::string key("graphics");
			AppendCacheKey(key, reinterpret_cast<uintptr_t>(desc.shader));

			const VertexLayout* layout = &desc.vertexLayout;
			if (desc.shader && layout->bindings.empty() && layout->attributes.empty()) {
				layout = &desc.shader->GetReflection().vertexLayout;
			}
			AppendCacheKey(key, layout->bindings.size());
			for (const auto& binding : layout->bindings) {
				AppendCacheKey(key, binding.binding);
				AppendCacheKey(key, binding.stride);
				AppendCacheKey(key, binding.inputRate);
			}
			AppendCacheKey(key, layout->attributes.size());
			for (const auto& attribute : layout->attributes) {
				AppendCacheKey(key, attribute.location);
				AppendCacheKey(key, attribute.binding);
				AppendCacheKey(key, attribute.format);
				AppendCacheKey(key, attribute.offset);
			}

			const auto& raster = desc.rasterState;
			AppendCacheKey(key, raster.cullMode);
			AppendCacheKey(key, raster.fillMode);
			AppendCacheKey(key, raster.frontCounterClockwise);
			AppendCacheKey(key, raster.depthBias.enable);
			AppendCacheKey(key, raster.depthBias.constantFactor);
			AppendCacheKey(key, raster.depthBias.clamp);
			AppendCacheKey(key, raster.depthBias.slopeFactor);
			AppendCacheKey(key, desc.depthState.depthTestEnable);
			AppendCacheKey(key, desc.depthState.depthWriteEnable);
			AppendCacheKey(key, desc.depthState.compareOp);
			AppendCacheKey(key, desc.stencilState.enable);
			for (const auto& face : {desc.stencilState.front, desc.stencilState.back}) {
				AppendCacheKey(key, face.failOp);
				AppendCacheKey(key, face.passOp);
				AppendCacheKey(key, face.depthFailOp);
				AppendCacheKey(key, face.compareOp);
				AppendCacheKey(key, face.compareMask);
				AppendCacheKey(key, face.writeMask);
				AppendCacheKey(key, face.reference);
			}
			AppendCacheKey(key, desc.multisampleState.sampleCount);
			AppendCacheKey(key, desc.multisampleState.sampleMask);
			AppendCacheKey(key, desc.multisampleState.alphaToCoverageEnable);
			AppendCacheKey(key, desc.topology);
			AppendCacheKey(key, desc.primitiveRestartEnable);

			AppendCacheKey(key, desc.renderTargetFormats.colorFormats.size());
			for (auto format : desc.renderTargetFormats.colorFormats) {
				AppendCacheKey(key, format);
			}
			AppendCacheKey(key, desc.renderTargetFormats.depthFormat);
			// Normalize an omitted per-target array to its effective attachment states.
			if (desc.colorBlendStates.empty()) {
				AppendCacheKey(key, desc.renderTargetFormats.colorFormats.size());
				for (size_t i = 0; i < desc.renderTargetFormats.colorFormats.size(); ++i) {
					AppendBlendKey(key, desc.blendState);
				}
			}
			else {
				AppendCacheKey(key, desc.colorBlendStates.size());
				for (const auto& blend : desc.colorBlendStates) {
					AppendBlendKey(key, blend);
				}
			}
			for (float constant : desc.blendConstants) {
				AppendCacheKey(key, constant);
			}
			return key;
		}
	}

	VulkanDevice::VulkanDevice() = default;

	VulkanDevice::~VulkanDevice() {
		if (m_Device != VK_NULL_HANDLE) {
			// Complete submissions before callbacks destroy Vulkan resources.
			vkDeviceWaitIdle(m_Device);
			ProcessDeferredDeletions();
		}
		ClearShaderAndPipelineCaches();
		m_Queues.clear();
		m_GraphicsCommandQueue = nullptr;

		if (m_DescriptorManager) {
			m_DescriptorManager->Cleanup();
		}

		if (m_Device != VK_NULL_HANDLE) {
			vkDeviceWaitIdle(m_Device);

			if (m_Allocator != VK_NULL_HANDLE) {
				vmaDestroyAllocator(m_Allocator);
			}
			vkDestroyDevice(m_Device, nullptr);
		}

		if (m_DebugMessenger != VK_NULL_HANDLE) {
			vkDestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
		}

		if (m_Instance != VK_NULL_HANDLE) {
			vkDestroyInstance(m_Instance, nullptr);
		}

		SDL_Vulkan_UnloadLibrary();
		SDL_Quit();
	}

	Result VulkanDevice::Init(const DeviceDesc& desc) {
		m_EnableDebug = desc.enableDebug;
		m_EnableProfiling = desc.enableProfiling;
		m_ShaderDirectory = desc.shaderDirectory;
		m_DebugCallback = DefaultDebugCallback;
		if (desc.debugCallback != nullptr) {
			m_DebugCallback = desc.debugCallback;
		}

		m_ShaderCompiler = std::make_unique<SlangShaderCompiler>(m_ShaderDirectory, m_EnableDebug, m_DebugCallback);

		if (!SDL_Init(SDL_INIT_VIDEO)) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to init SDL video.");
			return Result::Failed;
		}
		if (!SDL_Vulkan_LoadLibrary(nullptr)) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to load Vulkan library via SDL.");
			return Result::Failed;
		}

		if (volkInitialize() != VK_SUCCESS) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to initialize volk.");
			return Result::Failed;
		}

		if (!CreateInstance(desc.applicationName, desc.enableValidationLayers)) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan instance.");
			return Result::Failed;
		}

		if (desc.enableValidationLayers) {
			if (!SetupDebugMessenger()) {
				SendDebugMessage(
					DebugSeverity::Error, DebugMessageType::Validation, "Failed to setup Vulkan debug messenger.");
				return Result::Failed;
			}
		}

		if (!PickPhysicalDevice()) {
			SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to pick Vulkan physical device.");
			return Result::Failed;
		}

		m_Capabilities.limits.maxPushDataSize = GetDescriptorHeapProperties().maxPushDataSize;
		if (m_Capabilities.limits.maxPushDataSize == 0) {
			SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Selected device does not report descriptor heap push data support.");
			return Result::Failed;
		}

		if (!CreateLogicalDevice()) {
			SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan logical device.");
			return Result::Failed;
		}

		if (!CreateCommandQueues()) {
			SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan command queues.");
			return Result::Failed;
		}

		InitCapabilities();

		if (!CreateAllocator()) {
			SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan memory allocator.");
			return Result::Failed;
		}

		m_DescriptorManager = std::make_unique<VulkanDescriptorManager>(this);
		if (m_DescriptorManager->Init() != Result::Success) {
			SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to initialize Vulkan descriptor manager.");
			return Result::Failed;
		}

		return Result::Success;
	}

	void VulkanDevice::InitCapabilities() {
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(m_PhysicalDevice, &properties);

		m_Capabilities.name = properties.deviceName;
		m_Capabilities.vendorId = properties.vendorID;
		m_Capabilities.deviceId = properties.deviceID;
		m_Capabilities.features.debugLabels = m_DebugLabelsEnabled;

		const auto& source = properties.limits;
		auto& limits = m_Capabilities.limits;
		limits.maxTextureDimension2D = source.maxImageDimension2D;
		limits.maxTextureDimension3D = source.maxImageDimension3D;
		limits.maxTextureArrayLayers = source.maxImageArrayLayers;
		limits.maxColorAttachments = source.maxColorAttachments;
		limits.colorAttachmentSampleCounts = source.framebufferColorSampleCounts;
		limits.depthAttachmentSampleCounts = source.framebufferDepthSampleCounts;
		limits.stencilAttachmentSampleCounts = source.framebufferStencilSampleCounts;
		limits.noAttachmentSampleCounts = source.framebufferNoAttachmentsSampleCounts;
		VkPhysicalDeviceVulkan12Properties resolve{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
		VkPhysicalDeviceProperties2 extended{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &resolve
		};
		vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &extended);
		limits.integerColorAttachmentSampleCounts = resolve.framebufferIntegerColorSampleCounts;
		m_Capabilities.depthStencilResolve = {
			resolve.supportedDepthResolveModes,
			resolve.supportedStencilResolveModes,
			resolve.independentResolve != 0,
			resolve.independentResolveNone != 0
		};

		limits.maxDrawIndirectCount = source.maxDrawIndirectCount;
		limits.maxVertexInputBindings = source.maxVertexInputBindings;
		limits.maxVertexInputAttributes = source.maxVertexInputAttributes;
		limits.maxVertexInputBindingStride = source.maxVertexInputBindingStride;
		limits.maxVertexInputAttributeOffset = source.maxVertexInputAttributeOffset;

		for (uint32_t axis = 0; axis < 3; ++axis) {
			limits.maxComputeWorkGroupCount[axis] = source.maxComputeWorkGroupCount[axis];
			limits.maxComputeWorkGroupSize[axis] = source.maxComputeWorkGroupSize[axis];
		}
		limits.maxComputeWorkGroupInvocations = source.maxComputeWorkGroupInvocations;
		limits.maxComputeSharedMemorySize = source.maxComputeSharedMemorySize;

		limits.maxUniformBufferRange = source.maxUniformBufferRange;
		limits.maxStorageBufferRange = source.maxStorageBufferRange;
		limits.minUniformBufferOffsetAlignment = source.minUniformBufferOffsetAlignment;
		limits.minStorageBufferOffsetAlignment = source.minStorageBufferOffsetAlignment;
		limits.nonCoherentAtomSize = source.nonCoherentAtomSize;
		limits.maxSamplerLodBias = source.maxSamplerLodBias;
	}

	TextureFormatSupport VulkanDevice::GetTextureFormatSupport(TextureFormat format) const {
		const VkFormat nativeFormat = Utils::TextureFormatToVkFormat(format);
		if (nativeFormat == VK_FORMAT_UNDEFINED) {
			return {};
		}

		VkFormatProperties3 features{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
		VkFormatProperties2 properties{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &features};
		vkGetPhysicalDeviceFormatProperties2(m_PhysicalDevice, nativeFormat, &properties);
		const auto flags = features.optimalTilingFeatures;
		TextureFormatSupport support;

		if (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
			support.usages |= TextureUsage::ShaderResource;
		}
		if (flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
			support.usages |= TextureUsage::Storage;
		}
		if (flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) {
			support.usages |= TextureUsage::RenderTarget;
		}
		if (flags & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
			support.usages |= TextureUsage::DepthStencil;
		}
		if (flags & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) {
			support.usages |= TextureUsage::TransferSrc;
		}
		if (flags & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) {
			support.usages |= TextureUsage::TransferDst;
		}

		support.linearFiltering = (flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
		support.depthComparison = (flags & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT) != 0;
		support.colorBlending = (flags & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
		support.blitSource = (flags & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
		support.blitDestination = (flags & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
		support.storageAtomics = (flags & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) != 0;
		for (auto usage : {TextureUsage::RenderTarget, TextureUsage::DepthStencil}) {
			if (!Any(support.usages & usage)) {
				continue;
			}
			VkImageFormatProperties image{};
			if (vkGetPhysicalDeviceImageFormatProperties(m_PhysicalDevice,
					nativeFormat,
					VK_IMAGE_TYPE_2D,
					VK_IMAGE_TILING_OPTIMAL,
					Utils::TextureUsageToVkImageUsage(usage),
					0,
					&image) == VK_SUCCESS) {
				if (usage == TextureUsage::RenderTarget) {
					support.colorAttachmentSampleCounts =
						image.sampleCounts & m_Capabilities.limits.colorAttachmentSampleCounts;
					if (Utils::IsIntegerColorFormat(format)) {
						support.colorAttachmentSampleCounts =
							image.sampleCounts & m_Capabilities.limits.integerColorAttachmentSampleCounts;
					}
				}
				else {
					support.depthStencilAttachmentSampleCounts =
						image.sampleCounts & m_Capabilities.limits.depthAttachmentSampleCounts;
					if (Utils::TextureFormatToVkImageAspect(format) & VK_IMAGE_ASPECT_STENCIL_BIT) {
						support.depthStencilAttachmentSampleCounts &=
							m_Capabilities.limits.stencilAttachmentSampleCounts;
					}
				}
			}
		}
		return support;
	}

	bool VulkanDevice::SupportsVertexFormat(VertexFormat format) const {
		const auto info = Utils::GetVertexFormatInfo(format);
		if (info.format == VK_FORMAT_UNDEFINED) {
			return false;
		}

		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, info.format, &properties);
		return (properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
	}

	bool VulkanDevice::SupportsTexture(const TextureDesc& desc) const {
		constexpr auto knownUsages = TextureUsage::ShaderResource | TextureUsage::Storage | TextureUsage::RenderTarget |
			TextureUsage::DepthStencil | TextureUsage::TransferSrc | TextureUsage::TransferDst;
		if (!Utils::IsValidSampleCount(desc.sampleCount) ||
			(desc.sampleCount > 1 &&
				(desc.mipLevels != 1 || desc.depth != 1 || Any(desc.usage & TextureUsage::Storage))) ||
			!desc.width || !desc.height || !desc.depth || !desc.arrayLayers || !desc.mipLevels ||
			desc.mipLevels > std::bit_width(std::max({desc.width, desc.height, desc.depth})) ||
			desc.usage == TextureUsage::None ||
			(static_cast<uint32_t>(desc.usage) & ~static_cast<uint32_t>(knownUsages)) != 0) {
			return false;
		}

		const VkFormat format = Utils::TextureFormatToVkFormat(desc.format);
		if (format == VK_FORMAT_UNDEFINED) {
			return false;
		}

		const bool depthFormat = (Utils::TextureFormatToVkImageAspect(desc.format) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
		if ((depthFormat && Any(desc.usage & TextureUsage::RenderTarget)) ||
			(!depthFormat && Any(desc.usage & TextureUsage::DepthStencil)) ||
			(desc.depth > 1 &&
				(desc.arrayLayers != 1 ||
					Any(desc.usage & (TextureUsage::RenderTarget | TextureUsage::DepthStencil))))) {
			return false;
		}

		const auto support = GetTextureFormatSupport(desc.format);
		if ((support.usages & desc.usage) != desc.usage) {
			return false;
		}

		if ((Any(desc.usage & TextureUsage::RenderTarget) &&
				!(support.colorAttachmentSampleCounts & desc.sampleCount)) ||
			(Any(desc.usage & TextureUsage::DepthStencil) &&
				!(support.depthStencilAttachmentSampleCounts & desc.sampleCount))) {
			return false;
		}

		VkImageFormatProperties properties{};
		const auto result = vkGetPhysicalDeviceImageFormatProperties(m_PhysicalDevice,
			format,
			Utils::TextureDescToVkImageType(desc),
			VK_IMAGE_TILING_OPTIMAL,
			Utils::TextureUsageToVkImageUsage(desc.usage),
			0,
			&properties);
		if (result != VK_SUCCESS) {
			return false;
		}

		return desc.width <= properties.maxExtent.width && desc.height <= properties.maxExtent.height &&
			desc.depth <= properties.maxExtent.depth && desc.mipLevels <= properties.maxMipLevels &&
			desc.arrayLayers <= properties.maxArrayLayers && (properties.sampleCounts & desc.sampleCount) != 0;
	}

	Result VulkanDevice::SetDebugObjectName(VkObjectType type, uint64_t handle, const char* name) const {
		const VkDebugUtilsObjectNameInfoEXT info{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			.objectType = type,
			.objectHandle = handle,
			.pObjectName = name,
		};
		if (vkSetDebugUtilsObjectNameEXT(m_Device, &info) != VK_SUCCESS) {
			SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"SetDebugName: Failed to assign the Vulkan debug name.");
			return Result::Failed;
		}
		return Result::Success;
	}

	Result VulkanDevice::CreateWindow(const WindowDesc& desc, Ref<Window>& outWindow) const {
		Ref<VulkanWindow> vkWindow;
		Result res = VulkanWindow::Create(this, desc, vkWindow);
		if (res == Result::Success) {
			outWindow = vkWindow;
		}

		return res;
	}

	Result VulkanDevice::CreateShader(const ShaderDesc& desc, Ref<Shader>& outShader) const {
		std::scoped_lock lock(m_CacheMutex);
		CompiledShaderProgram program;
		const auto compiled = m_ShaderCompiler->Compile(desc, program);
		if (compiled != Result::Success) {
			return compiled;
		}
		const auto cached = m_Shaders.find(program.cacheId);
		if (cached != m_Shaders.end()) {
			outShader = cached->second;
			return Result::Success;
		}

		Ref<VulkanShader> shader;
		const auto result = VulkanShader::Create(this, program, desc.debugName, shader);
		if (result == Result::Success) {
			m_Shaders.emplace(program.cacheId, shader);
			outShader = shader;
		}
		return result;
	}

	Result VulkanDevice::CreateGraphicsPipeline(const PipelineDesc& desc, Ref<Pipeline>& outPipeline) const {
		std::scoped_lock lock(m_CacheMutex);
		const auto key = BuildGraphicsPipelineKey(desc);
		const auto cached = m_Pipelines.find(key);
		if (cached != m_Pipelines.end()) {
			outPipeline = cached->second.pipeline;
			return Result::Success;
		}

		Ref<VulkanPipeline> pipeline;
		const auto result = VulkanPipeline::CreateGraphics(this, desc, pipeline);
		if (result == Result::Success) {
			m_Pipelines.emplace(key, CachedPipeline{Ref<Shader>(desc.shader), pipeline});
			outPipeline = pipeline;
		}
		return result;
	}

	Result VulkanDevice::CreateComputePipeline(const ComputePipelineDesc& desc, Ref<Pipeline>& outPipeline) const {
		std::scoped_lock lock(m_CacheMutex);
		std::string key("compute");
		AppendCacheKey(key, reinterpret_cast<uintptr_t>(desc.shader));
		const auto cached = m_Pipelines.find(key);
		if (cached != m_Pipelines.end()) {
			outPipeline = cached->second.pipeline;
			return Result::Success;
		}

		Ref<VulkanPipeline> pipeline;
		const auto result = VulkanPipeline::CreateCompute(this, desc, pipeline);
		if (result == Result::Success) {
			m_Pipelines.emplace(key, CachedPipeline{Ref<Shader>(desc.shader), pipeline});
			outPipeline = pipeline;
		}
		return result;
	}

	void VulkanDevice::ClearShaderAndPipelineCaches() const {
		std::scoped_lock lock(m_CacheMutex);
		m_Pipelines.clear();
		m_Shaders.clear();
		if (m_ShaderCompiler) {
			m_ShaderCompiler->ClearCache();
		}
	}

	Result VulkanDevice::CreateSwapchain(const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain) const {
		Ref<VulkanSwapchain> vkSwapchain;
		Result res = VulkanSwapchain::Create(this, desc, vkSwapchain);
		if (res == Result::Success) {
			outSwapchain = vkSwapchain;
		}

		return res;
	}

	Result VulkanDevice::CreateBuffer(const BufferDesc& desc, Ref<Buffer>& outBuffer) const {
		Ref<VulkanBuffer> vkBuffer;
		Result res = VulkanBuffer::Create(this, desc, vkBuffer);
		if (res == Result::Success) {
			outBuffer = vkBuffer;
		}

		return res;
	}

	Result VulkanDevice::CreateTexture(const TextureDesc& desc, Ref<Texture>& outTexture) const {
		Ref<VulkanTexture> vkTexture;
		Result res = VulkanTexture::Create(this, desc, vkTexture);
		if (res == Result::Success) {
			outTexture = vkTexture;
		}

		return res;
	}

	Result VulkanDevice::CreateTextureView(Texture* texture,
		const TextureViewDesc& desc,
		Ref<TextureView>& outView) const {
		auto view = Ref<VulkanTextureView>::Create();
		const Result result = view->Init(this, static_cast<VulkanTexture*>(texture), desc, true);
		if (result == Result::Success) {
			outView = view;
		}

		return result;
	}

	Result VulkanDevice::CreateSampler(const SamplerDesc& desc, Ref<Sampler>& outSampler) const {
		auto sampler = Ref<VulkanSampler>::Create(this);
		const Result result = sampler->Init(desc);
		if (result == Result::Success) {
			outSampler = sampler;
		}

		return result;
	}

	Result VulkanDevice::CreateUploadContext(const UploadContextDesc& desc,
		std::unique_ptr<UploadContext>& outContext) const {
		auto context = std::make_unique<VulkanUploadContext>(this, desc);
		const Result result = context->Init();
		if (result == Result::Success) {
			outContext = std::move(context);
		}
		return result;
	}

	Result VulkanDevice::CreateCommandAllocator(QueueType type, Ref<CommandAllocator>& outAllocator) const {
		Ref<VulkanCommandAllocator> allocator;
		const auto* queue = static_cast<VulkanCommandQueue*>(GetCommandQueue(type));
		if (queue == nullptr) {
			return Result::Failed;
		}

		const Result result = VulkanCommandAllocator::Create(this, queue->GetQueueId().familyIndex, allocator);
		if (result == Result::Success) {
			outAllocator = allocator;
		}

		return result;
	}

	Result VulkanDevice::CreateCommandList(CommandAllocator* allocator, Ref<CommandList>& outCommandList) const {
		auto* vkAllocator = static_cast<VulkanCommandAllocator*>(allocator);
		if (vkAllocator == nullptr) {
			return Result::Failed;
		}
		Ref<VulkanCommandList> list;
		const Result result = VulkanCommandList::Create(this, Ref<VulkanCommandAllocator>(vkAllocator), list);

		if (result == Result::Success) {
			outCommandList = list;
		}
		return result;
	}

	CommandQueue* VulkanDevice::GetCommandQueue(QueueType type) const {
		uint32_t family = m_GraphicsQueueFamily;
		switch (type) {
			case QueueType::Graphics:
				break;
			case QueueType::Compute:
				family = m_ComputeQueueFamily;
				break;
			case QueueType::Transfer:
				family = m_TransferQueueFamily;
				break;
			default:
				return nullptr;
		}
		return GetCommandQueue(QueueId{family, 0});
	}

	CommandQueue* VulkanDevice::GetGraphicsCommandQueue() const {
		return m_GraphicsCommandQueue;
	}

	void VulkanDevice::DeferDeletion(std::vector<SubmissionToken> dependencies, std::function<void()> destroy) const {
		m_DeferredDeletions.Enqueue(std::move(dependencies), std::move(destroy));
	}

	void VulkanDevice::ProcessDeferredDeletions() {
		// Tokens identify their owning queue; queue index zero is valid.
		m_DeferredDeletions.Process([this](const SubmissionToken& token) {
			auto* queue = GetCommandQueue(token.queue);
			return queue != nullptr && queue->IsComplete(token);
		});
	}

	VulkanCommandQueue* VulkanDevice::GetCommandQueue(const QueueId& queueId) const {
		auto it = m_Queues.find(queueId);
		if (it == m_Queues.end()) {
			return nullptr;
		}

		return it->second.get();
	}

	void VulkanDevice::BindDescriptorHeaps(VkCommandBuffer commandBuffer) const {
		m_DescriptorManager->BindDescriptorHeaps(commandBuffer);
	}

	Result VulkanDevice::Submit(CommandList* commandList,
		Swapchain* swapchain,
		const FrameContext& frame,
		SubmissionToken* outToken,
		std::span<const SubmissionToken> waitFor) {
		if (outToken) {
			*outToken = {};
		}
		if (commandList == nullptr || swapchain == nullptr || m_GraphicsCommandQueue == nullptr) {
			return Result::Failed;
		}

		if (frame.imageIndex == std::numeric_limits<uint32_t>::max() || frame.backBufferView == nullptr) {
			return Result::Failed;
		}

		auto* vulkanSwapchain = static_cast<VulkanSwapchain*>(swapchain);
		if (!vulkanSwapchain->IsFrameValid(frame)) {
			SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Submit: Frame must match the swapchain.");
			return Result::Failed;
		}

		if (!vulkanSwapchain->PrepareFrameSubmission(frame.frameIndex)) {
			SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Submit: Frame is not ready for submission.");
			return Result::Failed;
		}

		VkCommandBuffer preTransition = vulkanSwapchain->RecordPreTransitionCmd(frame.frameIndex, frame.imageIndex);
		VkCommandBuffer postTransition = vulkanSwapchain->RecordPostTransitionCmd(frame.frameIndex, frame.imageIndex);

		VkSemaphoreSubmitInfo waitInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = vulkanSwapchain->GetImageAvailableSemaphore(frame.frameIndex),
			// Include the pre-render image layout transition in the acquire wait.
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};

		VkSemaphoreSubmitInfo signalInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = vulkanSwapchain->GetRenderFinishedSemaphore(frame.imageIndex),
			// The post-render transition is a pipeline barrier, not a graphics stage.
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};

		std::array<CommandList*, 1> tracked = {commandList};

		const SubmissionToken token =
			m_GraphicsCommandQueue->SubmitTracked(tracked,
				{&waitInfo, 1},
				{&signalInfo, 1},
				waitFor,
				swapchain,
				preTransition,
				postTransition);
		vulkanSwapchain->FinishFrameSubmission(frame.frameIndex, token);
		if (!token.IsValid()) {
			return Result::Failed;
		}
		if (outToken) {
			*outToken = token;
		}
		ProcessDeferredDeletions();

		return Result::Success;
	}

	Result VulkanDevice::ImmediateSubmit(CommandList* cmd) const {
		if (cmd == nullptr || m_GraphicsCommandQueue == nullptr) {
			return Result::Failed;
		}

		const SubmissionToken token = m_GraphicsCommandQueue->Submit(*cmd);
		if (!token.IsValid()) {
			return Result::Failed;
		}

		m_GraphicsCommandQueue->Wait(token);
		if (!m_GraphicsCommandQueue->IsComplete(token)) {
			return Result::Failed;
		}

		return Result::Success;
	}

	void VulkanDevice::WaitIdle() {
		for (const auto& [id, queue] : m_Queues) {
			queue->WaitIdle();
		}
		ProcessDeferredDeletions();
	}

	Result Device::Create(const DeviceDesc& desc, std::unique_ptr<Device>& outDevice) {
		std::unique_ptr<VulkanDevice> device = std::make_unique<VulkanDevice>();
		Result res = device->Init(desc);
		if (res != Result::Success) {
			return res;
		}

		outDevice = std::move(device);
		return Result::Success;
	}

	bool VulkanDevice::CreateInstance(const char* appName, bool enableValidationLayer) {
		VkApplicationInfo appInfo{
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.pApplicationName = appName,
			.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
			.pEngineName = "RealEngine",
			.engineVersion = VK_MAKE_VERSION(1, 0, 0),
			.apiVersion = VK_API_VERSION_1_3,
		};

		std::vector<const char*> extensions;
		std::vector<const char*> layers;

		uint32_t sdlExtensionsCount{0};
		char const* const* sdlExtensions{SDL_Vulkan_GetInstanceExtensions(&sdlExtensionsCount)};
		for (uint32_t i = 0; i < sdlExtensionsCount; ++i) {
			extensions.push_back(sdlExtensions[i]);
		}

		VkValidationFeatureEnableEXT syncValidation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
		VkValidationFeaturesEXT validationFeatures{
			.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
			.enabledValidationFeatureCount = 1,
			.pEnabledValidationFeatures = &syncValidation,
		};

		if (enableValidationLayer || m_EnableDebug || m_EnableProfiling) {
			uint32_t count = 0;
			if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
				SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"CreateInstance: Failed to enumerate available extensions for debug labels.");
				return false;
			}
			std::vector<VkExtensionProperties> available(count);
			if (vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()) != VK_SUCCESS) {
				SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"CreateInstance: Failed to read available extensions for debug labels.");
				return false;
			}
			m_DebugLabelsEnabled = std::any_of(available.begin(), available.end(), [](const auto& extension) {
				return std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
			});
			if (m_DebugLabelsEnabled || enableValidationLayer) {
				extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
				m_DebugLabelsEnabled = true;
			}
		}

		if (enableValidationLayer) {
			layers.push_back("VK_LAYER_KHRONOS_validation");

			uint32_t extensionCount = 0;
			constexpr const char* layerName = "VK_LAYER_KHRONOS_validation";
			if (vkEnumerateInstanceExtensionProperties(layerName, &extensionCount, nullptr) != VK_SUCCESS) {
				return false;
			}
			std::vector<VkExtensionProperties> layerExtensions(extensionCount);
			if (vkEnumerateInstanceExtensionProperties(layerName, &extensionCount, layerExtensions.data()) !=
				VK_SUCCESS) {
				return false;
			}
			bool supported = false;
			for (const auto& extension : layerExtensions) {
				if (std::strcmp(extension.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0) {
					supported = true;
					break;
				}
			}
			if (!supported) {
				SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"VK_EXT_validation_features is not advertised by the validation layer.");
				return false;
			}
			extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
		}

		VkInstanceCreateInfo createInfo{
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pNext = nullptr,
			.pApplicationInfo = &appInfo,
			.enabledLayerCount = static_cast<uint32_t>(layers.size()),
			.ppEnabledLayerNames = layers.data(),
			.enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
			.ppEnabledExtensionNames = extensions.data(),
		};

		if (enableValidationLayer) {
			createInfo.pNext = &validationFeatures;
		}

		if (vkCreateInstance(&createInfo, nullptr, &m_Instance) != VK_SUCCESS) {
			return false;
		}

		volkLoadInstance(m_Instance);

		return true;
	}

	VkPhysicalDeviceDescriptorHeapPropertiesEXT VulkanDevice::GetDescriptorHeapProperties() const {
		VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,
			.pNext = nullptr,
		};

		VkPhysicalDeviceProperties2 physicalDeviceProperties{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &descriptorHeapProperties,
		};

		vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &physicalDeviceProperties);

		return descriptorHeapProperties;
	}

	bool VulkanDevice::SetupDebugMessenger() {
		VkDebugUtilsMessengerCreateInfoEXT createInfo{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
			.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
			.pfnUserCallback = VulkanDebugCallback,
			.pUserData = reinterpret_cast<void*>(m_DebugCallback),
		};

		if (vkCreateDebugUtilsMessengerEXT(m_Instance, &createInfo, nullptr, &m_DebugMessenger) != VK_SUCCESS) {
			return false;
		}

		return true;
	}

	bool VulkanDevice::SupportsRequiredExtensions(VkPhysicalDevice device) const {
		uint32_t extensionCount = 0;
		if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr) != VK_SUCCESS) {
			SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to query Vulkan device extension count.");
			return false;
		}

		std::vector<VkExtensionProperties> extensions(extensionCount);
		if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data()) != VK_SUCCESS) {
			SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to enumerate Vulkan device extensions.");
			return false;
		}
		extensions.resize(extensionCount);

		for (const char* requiredExtension : s_DeviceExtensions) {
			const auto extension = std::find_if(extensions.begin(),
				extensions.end(),
				[requiredExtension](const VkExtensionProperties& availableExtension) {
					return std::strcmp(requiredExtension, availableExtension.extensionName) == 0;
				});

			if (extension == extensions.end()) {
				const std::string message =
					std::string("Skipping Vulkan device: Missing required extension ") + requiredExtension;
				SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, message.c_str());
				return false;
			}
		}

		return true;
	}

	bool VulkanDevice::SupportsRequiredFeatures(VkPhysicalDevice device) const {
		VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeapFeatures{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
		};
		VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
			.pNext = &descriptorHeapFeatures,
		};
		VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedPointerFeatures{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR,
			.pNext = &maintenance5Features,
		};
		VkPhysicalDeviceVulkan13Features vulkan13Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
			.pNext = &untypedPointerFeatures,
		};
		VkPhysicalDeviceVulkan12Features vulkan12Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			.pNext = &vulkan13Features,
		};
		VkPhysicalDeviceVulkan11Features vulkan11Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
			.pNext = &vulkan12Features,
		};
		VkPhysicalDeviceFeatures2 features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			.pNext = &vulkan11Features,
		};
		vkGetPhysicalDeviceFeatures2(device, &features);

		struct RequiredFeature {
			VkBool32 supported;
			const char* name;
		};

		const RequiredFeature requiredFeatures[] = {
			{vulkan11Features.shaderDrawParameters, "shaderDrawParameters"},
			{vulkan12Features.bufferDeviceAddress, "bufferDeviceAddress"},
			{vulkan12Features.timelineSemaphore, "timelineSemaphore"},
			{vulkan13Features.synchronization2, "synchronization2"},
			{vulkan13Features.dynamicRendering, "dynamicRendering"},
			{untypedPointerFeatures.shaderUntypedPointers, "shaderUntypedPointers"},
			{maintenance5Features.maintenance5, "maintenance5"},
			{descriptorHeapFeatures.descriptorHeap, "descriptorHeap"},
		};

		for (const RequiredFeature& feature : requiredFeatures) {
			if (feature.supported == VK_FALSE) {
				const std::string message =
					std::string("Skipping Vulkan device: Missing required feature ") + feature.name;
				SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, message.c_str());
				return false;
			}
		}

		return true;
	}

	int VulkanDevice::RatePhysicalDevice(VkPhysicalDevice device) {
		if (!SupportsRequiredExtensions(device) || !SupportsRequiredFeatures(device)) {
			return 0;
		}

		VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,
		};
		VkPhysicalDeviceProperties2 properties{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &descriptorHeapProperties,
		};
		vkGetPhysicalDeviceProperties2(device, &properties);

		if (properties.properties.apiVersion < VK_API_VERSION_1_3) {
			SendDebugMessage(DebugSeverity::Info,
				DebugMessageType::General,
				"Skipping Vulkan device: Vulkan 1.3 or newer is required.");
			return 0;
		}

		if (descriptorHeapProperties.maxPushDataSize == 0) {
			SendDebugMessage(DebugSeverity::Info,
				DebugMessageType::General,
				"Skipping Vulkan device: Descriptor heap push data is unavailable.");
			return 0;
		}

		const QueueFamilyIndices queueFamilies = FindQueueFamilies(device);
		if (!queueFamilies.IsComplete()) {
			SendDebugMessage(DebugSeverity::Info,
				DebugMessageType::General,
				"Skipping Vulkan device: Required graphics and presentation queues are unavailable.");
			return 0;
		}

		int score = static_cast<int>(properties.properties.limits.maxImageDimension2D);
		if (descriptorHeapProperties.bufferDescriptorSize > 0) {
			score += 1000;
		}

		switch (properties.properties.deviceType) {
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
				score += 1000;
				break;
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
				score += 100;
				break;
			default:
				break;
		}

		return score;
	}

	bool VulkanDevice::PickPhysicalDevice() {
		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);

		if (deviceCount == 0) {
			return false;
		}

		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

		VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
		int bestScore = 0;

		for (const auto& device : devices) {
			int score = RatePhysicalDevice(device);

			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(device, &props);

			std::string foundDevice = "Found device: " + std::string(props.deviceName);
			SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, foundDevice.c_str());

			if (score > bestScore) {
				bestScore = score;
				bestDevice = device;
			}
		}

		if (bestDevice == VK_NULL_HANDLE) {
			return false;
		}

		m_PhysicalDevice = bestDevice;

		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(m_PhysicalDevice, &props);
		std::string selectedDevice = "Selected device: " + std::string(props.deviceName);
		SendDebugMessage(DebugSeverity::Info, DebugMessageType::General, selectedDevice.c_str());

		return true;
	}

	bool VulkanDevice::CreateLogicalDevice() {
		QueueFamilyIndices indices = FindQueueFamilies(m_PhysicalDevice);

		m_GraphicsQueueFamily = indices.graphicsFamily.value();
		m_PresentQueueFamily = indices.presentFamily.value();
		m_ComputeQueueFamily = indices.computeFamily.value_or(m_GraphicsQueueFamily);
		m_TransferQueueFamily = indices.transferFamily.value_or(m_ComputeQueueFamily);

		std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
		std::set<uint32_t> uniqueQueueFamilies = {
			m_GraphicsQueueFamily, m_PresentQueueFamily, m_ComputeQueueFamily, m_TransferQueueFamily
		};

		float queuePriority = 1.0f;
		for (uint32_t queueFamily : uniqueQueueFamilies) {
			VkDeviceQueueCreateInfo queueCreateInfo{
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = queueFamily,
				.queueCount = 1,
				.pQueuePriorities = &queuePriority,
			};

			queueCreateInfos.push_back(queueCreateInfo);
		}

		VkPhysicalDeviceDescriptorHeapFeaturesEXT heapFeatures{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
			.descriptorHeap = VK_TRUE,
		};

		VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
			.pNext = &heapFeatures,
			.maintenance5 = VK_TRUE,
		};

		VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedPtrFeatures{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR,
			.pNext = &maintenance5,
			.shaderUntypedPointers = VK_TRUE,
		};

		VkPhysicalDeviceVulkan13Features enabledVk13Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
			.pNext = &untypedPtrFeatures,
			.synchronization2 = VK_TRUE,
			.dynamicRendering = VK_TRUE,
		};
		VkPhysicalDeviceVulkan12Features enabledVk12Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			.pNext = &enabledVk13Features,
			.timelineSemaphore = VK_TRUE,
			.bufferDeviceAddress = VK_TRUE,
		};
		// Slang uses base-vertex/base-instance built-ins for SV_VertexID and SV_InstanceID.
		VkPhysicalDeviceVulkan11Features enabledVk11Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
			.pNext = &enabledVk12Features,
			.shaderDrawParameters = VK_TRUE,
		};

		VkPhysicalDeviceVulkan12Features supportedVk12Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		};
		VkPhysicalDeviceFeatures2 supportedFeatures{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			.pNext = &supportedVk12Features,
		};
		vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &supportedFeatures);
		m_Capabilities.features.multiDrawIndirect = supportedFeatures.features.multiDrawIndirect == VK_TRUE;
		m_Capabilities.features.drawIndirectFirstInstance =
			supportedFeatures.features.drawIndirectFirstInstance == VK_TRUE;
		m_Capabilities.features.wireframe = supportedFeatures.features.fillModeNonSolid == VK_TRUE;
		m_Capabilities.features.independentBlend = supportedFeatures.features.independentBlend == VK_TRUE;
		m_Capabilities.features.depthBiasClamp = supportedFeatures.features.depthBiasClamp == VK_TRUE;
		m_SupportsHostQueryReset = supportedVk12Features.hostQueryReset == VK_TRUE;
		enabledVk12Features.hostQueryReset = supportedVk12Features.hostQueryReset;

		VkPhysicalDeviceFeatures deviceFeatures{
			.independentBlend = supportedFeatures.features.independentBlend,
			.multiDrawIndirect = supportedFeatures.features.multiDrawIndirect,
			.drawIndirectFirstInstance = supportedFeatures.features.drawIndirectFirstInstance,
			.depthBiasClamp = supportedFeatures.features.depthBiasClamp,
			.fillModeNonSolid = supportedFeatures.features.fillModeNonSolid,
		};

		VkDeviceCreateInfo createInfo{
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.pNext = &enabledVk11Features,
			.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
			.pQueueCreateInfos = queueCreateInfos.data(),
			.enabledExtensionCount = static_cast<uint32_t>(s_DeviceExtensions.size()),
			.ppEnabledExtensionNames = s_DeviceExtensions.data(),
			.pEnabledFeatures = &deviceFeatures,
		};

		if (vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device) != VK_SUCCESS) {
			return false;
		}

		volkLoadDevice(m_Device);
		return true;
	}

	QueueFamilyIndices VulkanDevice::FindQueueFamilies(VkPhysicalDevice device) {
		QueueFamilyIndices indices;

		uint32_t count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

		for (uint32_t i = 0; i < count; ++i) {
			const VkQueueFlags flags = families[i].queueFlags;
			if (families[i].queueCount == 0) {
				continue;
			}
			if (!indices.graphicsFamily && (flags & VK_QUEUE_GRAPHICS_BIT)) {
				indices.graphicsFamily = i;
			}
			if (!indices.presentFamily && SDL_Vulkan_GetPresentationSupport(m_Instance, device, i)) {
				indices.presentFamily = i;
			}
			if ((flags & VK_QUEUE_COMPUTE_BIT) && (!indices.computeFamily || !(flags & VK_QUEUE_GRAPHICS_BIT))) {
				indices.computeFamily = i;
			}
			if ((flags & VK_QUEUE_TRANSFER_BIT) &&
				(!indices.transferFamily || !(flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))) {
				indices.transferFamily = i;
			}
		}

		if (!indices.presentFamily && indices.graphicsFamily) {
			indices.presentFamily = indices.graphicsFamily;
		}
		if (!indices.computeFamily) {
			indices.computeFamily = indices.graphicsFamily;
		}

		// If no dedicated transfer queue is found, use the compute queue if available,
		// otherwise fallback to graphics.
		if (!indices.transferFamily) {
			indices.transferFamily = indices.computeFamily;
		}

		return indices;
	}

	bool VulkanDevice::CreateCommandQueues() {
		if (!m_Queues.empty()) {
			SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Command queues are already initialized.");
			return false;
		}

		const uint32_t families[] = {
			m_GraphicsQueueFamily, m_PresentQueueFamily, m_ComputeQueueFamily, m_TransferQueueFamily
		};
		for (uint32_t family : families) {
			const QueueId id{family, 0};
			if (m_Queues.contains(id)) {
				continue;
			}
			VkQueue queue = VK_NULL_HANDLE;
			vkGetDeviceQueue(m_Device, family, 0, &queue);
			if (queue == VK_NULL_HANDLE) {
				return false;
			}

			std::unique_ptr<VulkanCommandQueue> commandQueue;
			if (VulkanCommandQueue::Create(this, family, 0, queue, commandQueue) != Result::Success) {
				return false;
			}

			m_Queues.emplace(id, std::move(commandQueue));
		}

		m_GraphicsCommandQueue = m_Queues.at(QueueId{m_GraphicsQueueFamily, 0}).get();
		m_PresentQueue = m_Queues.at(QueueId{m_PresentQueueFamily, 0})->GetQueue();

		return true;
	}

	bool VulkanDevice::CreateAllocator() {
		VmaVulkanFunctions vkFunctions{
			.vkGetInstanceProcAddr = vkGetInstanceProcAddr,
			.vkGetDeviceProcAddr = vkGetDeviceProcAddr,
			.vkCreateImage = vkCreateImage,
		};
		VmaAllocatorCreateInfo allocatorCI{
			.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
			.physicalDevice = m_PhysicalDevice,
			.device = m_Device,
			.pVulkanFunctions = &vkFunctions,
			.instance = m_Instance,
		};

		return vmaCreateAllocator(&allocatorCI, &m_Allocator) == VK_SUCCESS;
	}

	VKAPI_ATTR VkBool32 VulkanDevice::VulkanDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT type,
		const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
		void* userData) {
		DebugCallback debugCallback = reinterpret_cast<DebugCallback>(userData);

		DebugMessage message;
		switch (severity) {
			case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
				message.severity = DebugSeverity::Info;
				break;
			case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
				message.severity = DebugSeverity::Warning;
				break;
			case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
				message.severity = DebugSeverity::Error;
				break;
			default:
				message.severity = DebugSeverity::Info;
		}

		if ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0) {
			message.type = DebugMessageType::Validation;
		}
		else if ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0) {
			message.type = DebugMessageType::Performance;
		}
		else {
			message.type = DebugMessageType::General;
		}

		message.message = callbackData->pMessage;
		debugCallback(message);

		return VK_FALSE;
	}

	namespace {
		ResourceStatistics& SelectResource(DeviceStatistics& statistics, VulkanProfiledResource type) {
			switch (type) {
				case VulkanProfiledResource::Buffer:
					return statistics.buffers;
				case VulkanProfiledResource::Texture:
					return statistics.textures;
				case VulkanProfiledResource::Pipeline:
					return statistics.pipelines;
			}
			return statistics.buffers;
		}

		void AddTiming(TimingStatistics& timing, double milliseconds) {
			++timing.sampleCount;
			timing.totalMilliseconds += milliseconds;
			timing.maximumMilliseconds = std::max(timing.maximumMilliseconds, milliseconds);
		}
	}

	DeviceStatistics VulkanDevice::GetStatistics() const {
		DeviceStatistics snapshot;
		{
			std::scoped_lock lock(m_StatisticsMutex);
			snapshot = m_Statistics;
		}

		if (m_DescriptorManager) {
			snapshot.descriptors = m_DescriptorManager->GetStatistics();
		}

		return snapshot;
	}

	void VulkanDevice::RecordResourceCreated(VulkanProfiledResource type, uint64_t bytes) const {
		std::scoped_lock lock(m_StatisticsMutex);
		auto& usage = SelectResource(m_Statistics, type);
		++usage.liveCount;
		usage.liveBytes += bytes;
		usage.peakCount = std::max(usage.peakCount, usage.liveCount);
		usage.peakBytes = std::max(usage.peakBytes, usage.liveBytes);
	}

	void VulkanDevice::RecordResourceDestroyed(VulkanProfiledResource type, uint64_t bytes) const {
		std::scoped_lock lock(m_StatisticsMutex);
		auto& usage = SelectResource(m_Statistics, type);
		--usage.liveCount;
		usage.liveBytes -= bytes;
	}

	void VulkanDevice::RecordCpuTiming(VulkanProfiledOperation operation, double milliseconds, uint64_t bytes) const {
		std::scoped_lock lock(m_StatisticsMutex);
		switch (operation) {
			case VulkanProfiledOperation::UploadStaging:
				AddTiming(m_Statistics.uploadStaging, milliseconds);
				m_Statistics.stagedUploadBytes += bytes;
				break;
			case VulkanProfiledOperation::Submission:
				AddTiming(m_Statistics.submissions, milliseconds);
				break;
			case VulkanProfiledOperation::GraphicsPipeline:
				AddTiming(m_Statistics.graphicsPipelineCreation, milliseconds);
				break;
			case VulkanProfiledOperation::ComputePipeline:
				AddTiming(m_Statistics.computePipelineCreation, milliseconds);
				break;
		}
	}

	void VulkanDevice::RecordUploadBatch(uint64_t bytes) const {
		if (!m_EnableProfiling) {
			return;
		}

		std::scoped_lock lock(m_StatisticsMutex);
		++m_Statistics.uploadBatchCount;
		m_Statistics.submittedUploadBytes += bytes;
	}

	void VulkanDevice::RecordGpuTimings(QueueId queue, std::span<const GpuTimestamp> timestamps) const {
		std::scoped_lock lock(m_StatisticsMutex);
		for (size_t index = 1; index < timestamps.size(); ++index) {
			const auto& begin = timestamps[index - 1];
			const double milliseconds = timestamps[index].milliseconds - begin.milliseconds;
			auto found =
				std::find_if(m_Statistics.gpuTimings.begin(), m_Statistics.gpuTimings.end(), [&](const auto& timing) {
					return timing.name == begin.name && timing.queue == queue;
				});
			if (found == m_Statistics.gpuTimings.end()) {
				m_Statistics.gpuTimings.push_back({.name = begin.name, .queue = queue});
				found = m_Statistics.gpuTimings.end() - 1;
			}

			AddTiming(found->timing, milliseconds);
		}
	}
}
