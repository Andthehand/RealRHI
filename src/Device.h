#pragma once
#include "DeviceDesc.h"

#include "Window.h"
#include "WindowDesc.h"

#include "BufferDesc.h"
#include "Buffer.h"

#include "ShaderDesc.h"
#include "Shader.h"

#include "PipelineDesc.h"
#include "Pipeline.h"

#include "CommandList.h"

#include "SwapchainDesc.h"
#include "Swapchain.h"

#include "FrameContext.h"

#include <memory>
#include <filesystem>

#include "RefCounted.h"

namespace RealRHI {
    class Device {
    public:
        virtual ~Device() = default;

		virtual std::filesystem::path GetShaderDirectory() const = 0;
		virtual DebugCallback GetDebugCallback() const = 0;
		virtual bool IsDebugEnabled() const = 0;

		virtual Ref<Window> CreateWindow(const WindowDesc& desc) = 0;
		virtual Ref<Shader> CreateShader(const ShaderDesc& desc) = 0;
		virtual Ref<Pipeline> CreateGraphicsPipeline(const PipelineDesc& desc) = 0;
        virtual Ref<Swapchain> CreateSwapchain(const SwapchainDesc& desc) = 0;
		virtual Ref<Buffer> CreateBuffer(const BufferDesc& desc) = 0;
		virtual Ref<CommandList> CreateCommandList() = 0;

		virtual void Submit(CommandList* cmd, Swapchain* swapchain, const FrameContext& frame) = 0;
		virtual void WaitIdle() = 0;

		static std::unique_ptr<Device> Create(const DeviceDesc& desc);
    };
}