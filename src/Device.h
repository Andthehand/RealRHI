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

#include <memory>
#include <filesystem>

namespace RealRHI {
    class Device {
    public:
        virtual ~Device() = default;

		virtual std::filesystem::path GetShaderDirectory() const = 0;
		virtual DebugCallback GetDebugCallback() const = 0;
		virtual bool IsDebugEnabled() const = 0;

		virtual std::unique_ptr<Window> CreateWindow(const WindowDesc& desc) = 0;
		virtual std::unique_ptr<Shader> CreateShader(const ShaderDesc& desc) = 0;
		virtual std::unique_ptr<Pipeline> CreateGraphicsPipeline(const PipelineDesc& desc) = 0;
        virtual std::unique_ptr<Swapchain> CreateSwapchain(const SwapchainDesc& desc) = 0;
		virtual std::unique_ptr<Buffer> CreateBuffer(const BufferDesc& desc) = 0;
		virtual std::unique_ptr<CommandList> CreateCommandList() = 0;
    };
}