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

#include "SwapchainDesc.h"
#include "Swapchain.h"

#include "TextureDesc.h"
#include "Texture.h"

#include "SamplerDesc.h"
#include "Sampler.h"

#include "FrameContext.h"

#include <memory>
#include <filesystem>

#include "RefCounted.h"

namespace RealRHI {
    class Device {
    public:
        virtual ~Device() = default;

		virtual std::filesystem::path GetShaderDirectory() const = 0;
		virtual bool IsDebugEnabled() const = 0;

		virtual Result CreateWindow(const WindowDesc& desc, Ref<Window>& outWindow) = 0;
		virtual Result CreateShader(const ShaderDesc& desc, Ref<Shader>& outShader) = 0;
		virtual Result CreateGraphicsPipeline(const PipelineDesc& desc, Ref<Pipeline>& outPipeline) = 0;
        virtual Result CreateSwapchain(const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain) = 0;
		virtual Result CreateBuffer(const BufferDesc& desc, Ref<Buffer>& outBuffer) = 0;
		virtual Result CreateTexture(const TextureDesc& desc, Ref<Texture>& outTexture) = 0;
		virtual Result CreateSampler(const SamplerDesc& desc, Ref<Sampler>& outSampler) = 0;
		virtual Result CreateCommandList(Ref<CommandList>& outCommandList) = 0;

		virtual void Submit(CommandList* cmd, Swapchain* swapchain, const FrameContext& frame) = 0;
		virtual Result ImmediateSubmit(CommandList* cmd) const = 0;
		virtual void WaitIdle() = 0;

		static Result Create(const DeviceDesc& desc, std::unique_ptr<Device>& outDevice);
    };
}