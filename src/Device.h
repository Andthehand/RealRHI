#pragma once
#include "DeviceDesc.h"

#include "BufferDesc.h"
#include "Buffer.h"

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

		virtual std::unique_ptr<Shader> CreateShader(const char* moduleName) = 0;
        virtual Buffer* CreateBuffer(const BufferDesc&) = 0;
        virtual Pipeline* CreateGraphicsPipeline(const PipelineDesc&) = 0;
        virtual CommandList* CreateCommandList() = 0;
        virtual std::unique_ptr<Swapchain> CreateSwapchain(const SwapchainDesc& desc) = 0;
    };
}