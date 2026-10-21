#pragma once
#include "DeviceDesc.h"

#include "Window.h"
#include "WindowDesc.h"

#include "Result.h"

#include "BufferDesc.h"
#include "Buffer.h"

#include "ShaderDesc.h"
#include "Shader.h"

#include "PipelineDesc.h"
#include "Pipeline.h"

#include "CommandList.h"
#include "CommandAllocator.h"

#include "SwapchainDesc.h"
#include "Swapchain.h"

#include "TextureDesc.h"
#include "Texture.h"

#include "FrameContext.h"
#include "Queue.h"

#include <memory>
#include <vector>
#include <functional>
#include <filesystem>

#include "RefCounted.h"

#undef CreateWindow // Windows.h defines a macro for CreateWindow, which conflicts with our Device::CreateWindow method

namespace RealRHI {
	class Device {
	public:
		virtual ~Device() = default;

		virtual std::filesystem::path GetShaderDirectory() const = 0;
		virtual bool IsDebugEnabled() const = 0;

		virtual Result CreateWindow(const WindowDesc& desc, Ref<Window>& outWindow) const = 0;
		virtual Result CreateShader(const ShaderDesc& desc, Ref<Shader>& outShader) const = 0;
		virtual Result CreateGraphicsPipeline(const PipelineDesc& desc, Ref<Pipeline>& outPipeline) const = 0;
		virtual Result CreateSwapchain(const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain) const = 0;
		virtual Result CreateBuffer(const BufferDesc& desc, Ref<Buffer>& outBuffer) const = 0;
		virtual Result CreateTexture(const TextureDesc& desc, Ref<Texture>& outTexture) const = 0;
		virtual Result CreateCommandAllocator(Ref<CommandAllocator>& outAllocator) const = 0;
        // Non-owning queue access; the device retains ownership.
        virtual CommandQueue* GetGraphicsCommandQueue() const = 0;
		virtual Result CreateCommandList(CommandAllocator* allocator, Ref<CommandList>& outCommandList) const = 0;

		virtual Result Submit(CommandList* cmd, Swapchain* swapchain, const FrameContext& frame, SubmissionToken* outToken = nullptr) = 0;
		virtual Result ImmediateSubmit(CommandList* cmd) const = 0;
		virtual void WaitIdle() = 0;
        // Defer backend-owned cleanup until all supplied GPU submissions complete.
        virtual void DeferDeletion(std::vector<SubmissionToken> dependencies, std::function<void()> destroy) = 0;
        virtual void ProcessDeferredDeletions() = 0;

		static Result Create(const DeviceDesc& desc, std::unique_ptr<Device>& outDevice);
	};
}