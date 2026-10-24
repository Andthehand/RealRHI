#pragma once
#include "Buffer.h"
#include "BufferDesc.h"
#include "CommandAllocator.h"
#include "CommandList.h"
#include "DeviceDesc.h"
#include "FrameContext.h"
#include "Pipeline.h"
#include "PipelineDesc.h"
#include "Queue.h"
#include "RefCounted.h"
#include "Result.h"
#include "Shader.h"
#include "ShaderDesc.h"
#include "Swapchain.h"
#include "SwapchainDesc.h"
#include "Texture.h"
#include "TextureDesc.h"
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

		virtual std::filesystem::path GetShaderDirectory() const = 0;
		virtual bool IsDebugEnabled() const = 0;

		virtual Result CreateWindow(const WindowDesc& desc, Ref<Window>& outWindow) const = 0;
		virtual Result CreateShader(const ShaderDesc& desc, Ref<Shader>& outShader) const = 0;
		virtual Result CreateGraphicsPipeline(const PipelineDesc& desc, Ref<Pipeline>& outPipeline) const = 0;
		virtual Result CreateComputePipeline(const ComputePipelineDesc& desc, Ref<Pipeline>& outPipeline) const = 0;
		virtual Result CreateSwapchain(const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain) const = 0;
		virtual Result CreateBuffer(const BufferDesc& desc, Ref<Buffer>& outBuffer) const = 0;
		virtual Result CreateTexture(const TextureDesc& desc, Ref<Texture>& outTexture) const = 0;
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
