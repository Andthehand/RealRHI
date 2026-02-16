#pragma once
#include "BufferDesc.h"
#include "Buffer.h"

#include "PipelineDesc.h"
#include "Pipeline.h"

#include "CommandList.h"

#include "SwapchainDesc.h"
#include "Swapchain.h"

#include <memory>

namespace RealRHI {
    class Device {
    public:
        virtual ~Device() = default;

        virtual Buffer* CreateBuffer(const BufferDesc&) = 0;
        virtual Pipeline* CreateGraphicsPipeline(const PipelineDesc&) = 0;
        virtual CommandList* CreateCommandList() = 0;
        virtual std::unique_ptr<Swapchain> CreateSwapchain(const SwapchainDesc& desc) = 0;
    };
}