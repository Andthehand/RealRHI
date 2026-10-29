#pragma once
#include "Buffer.h"
#include "BufferDesc.h"
#include "CommandAllocator.h"
#include "CommandList.h"
#include "DeviceCapabilities.h"
#include "DeviceDesc.h"
#include "FrameContext.h"
#include "Pipeline.h"
#include "PipelineDesc.h"
#include "Profiling.h"
#include "Queue.h"
#include "RefCounted.h"
#include "Result.h"
#include "Sampler.h"
#include "SamplerDesc.h"
#include "Shader.h"
#include "ShaderDesc.h"
#include "Swapchain.h"
#include "SwapchainDesc.h"
#include "Texture.h"
#include "TextureDesc.h"
#include "TextureViewDesc.h"
#include "UploadContext.h"
#include "Window.h"
#include "WindowDesc.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#undef CreateWindow // Windows.h defines a macro for CreateWindow, which conflicts with our Device::CreateWindow method

namespace RealRHI {
	// Objects passed to RHI operations must belong to the receiving backend.
	// Backend compatibility is a caller precondition; backend downcasts use static_cast.
	class Device {
	public:
		virtual ~Device() = default;

		// Immutable snapshot; valid for the lifetime of this device.
		virtual const DeviceCapabilitiesInfo& GetCapabilities() const = 0;
		virtual TextureFormatSupport GetTextureFormatSupport(TextureFormat format) const = 0;
		virtual bool SupportsVertexFormat(VertexFormat format) const = 0;
		// Quiet, allocation-free check of the complete description. Success does not guarantee memory availability.
		virtual bool SupportsTexture(const TextureDesc& desc) const = 0;

		virtual std::filesystem::path GetShaderDirectory() const = 0;
		virtual bool IsDebugEnabled() const = 0;
		virtual bool IsProfilingEnabled() const = 0;
		// Snapshot only; poll completion tokens or WaitIdle first to include completed GPU intervals.
		virtual DeviceStatistics GetStatistics() const = 0;

		// Optional debugName strings need only survive the creation call. Names are ignored
		// when debug naming is unavailable; textures also pass their name to default views/samplers.
		virtual Result CreateWindow(const WindowDesc& desc, Ref<Window>& outWindow) const = 0;
		// Thread-safe, per-device shader/pipeline reuse. Source dependencies are checked
		// on CreateShader; existing shaders and pipelines keep their compiled version.
		// Debug names do not affect identity; the first creation names the shared object.
		virtual Result CreateShader(const ShaderDesc& desc, Ref<Shader>& outShader) const = 0;
		virtual Result CreateGraphicsPipeline(const PipelineDesc& desc, Ref<Pipeline>& outPipeline) const = 0;
		virtual Result CreateComputePipeline(const ComputePipelineDesc& desc, Ref<Pipeline>& outPipeline) const = 0;

		// In-memory caches retain successful objects until cleared or device destruction.
		// Drop cache-owned references and compiled programs. Existing objects remain valid.
		virtual void ClearShaderAndPipelineCaches() const = 0;

		virtual Result CreateSwapchain(const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain) const = 0;
		virtual Result CreateBuffer(const BufferDesc& desc, Ref<Buffer>& outBuffer) const = 0;
		virtual Result CreateTexture(const TextureDesc& desc, Ref<Texture>& outTexture) const = 0;
		virtual Result CreateTextureView(Texture* texture, const TextureViewDesc& desc, Ref<TextureView>& outView) const = 0;
		// Thread-safe. Identical settings may share a descriptor; the first owner's debug name is retained.
		// Sampler objects remain independently owned; the slot is freed after its final owner is released.
		virtual Result CreateSampler(const SamplerDesc& desc, Ref<Sampler>& outSampler) const = 0;
		virtual Result CreateUploadContext(const UploadContextDesc& desc, std::unique_ptr<UploadContext>& outContext) const = 0;
		virtual Result CreateCommandAllocator(QueueType type, Ref<CommandAllocator>& outAllocator) const = 0;
		// Non-owning queue access; the device retains ownership.
		virtual CommandQueue* GetGraphicsCommandQueue() const = 0;
		virtual CommandQueue* GetCommandQueue(QueueType type) const = 0;
		virtual Result CreateCommandList(CommandAllocator* allocator, Ref<CommandList>& outCommandList) const = 0;

		virtual Result Submit(CommandList* cmd,
			Swapchain* swapchain,
			const FrameContext& frame,
			SubmissionToken* outToken = nullptr,
			std::span<const SubmissionToken> waitFor = {}) = 0;
		virtual Result ImmediateSubmit(CommandList* cmd) const = 0;
		virtual void WaitIdle() = 0;
		// Defer backend cleanup until every submission dependency completes.
		virtual void DeferDeletion(std::vector<SubmissionToken> dependencies, std::function<void()> destroy) const = 0;
		virtual void ProcessDeferredDeletions() = 0;

		static Result Create(const DeviceDesc& desc, std::unique_ptr<Device>& outDevice);
	};
}
