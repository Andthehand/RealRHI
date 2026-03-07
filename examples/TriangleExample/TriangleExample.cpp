#include <SDL3/SDL.h>

#include "Device.h"
#include "FrameContext.h"

#include <iostream>
#include <vector>

constexpr uint32_t WIDTH = 800;
constexpr uint32_t HEIGHT = 600;

struct Vertex {
    float pos[2];
    float color[3];
};

const std::vector<Vertex> vertices = {
    {{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, 0.5f},  {0.0f, 1.0f, 0.0f}},
    {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}}
};

std::unique_ptr<RealRHI::Device> device;
RealRHI::Ref<RealRHI::Window> window;
RealRHI::Ref<RealRHI::Swapchain> swapchain;
RealRHI::Ref<RealRHI::Pipeline> pipeline;
RealRHI::Ref<RealRHI::Buffer> vertexBuffer;
std::vector<RealRHI::Ref<RealRHI::CommandList>> commandLists;

void CreateSwapChain() {
    RealRHI::Result result = device->CreateWindow({
        .Title = "RealRHI Triangle Example",
        .Width = WIDTH,
        .Height = HEIGHT
    }, window);
    if (result != RealRHI::Result::Success) {
		std::cerr << "Failed to create window" << std::endl;
    }
    
    result = device->CreateSwapchain({
        .window = window.Raw(),
    }, swapchain);
    if (result != RealRHI::Result::Success) {
		std::cerr << "Failed to create swapchain" << std::endl;
    }
}

void CreateGraphicsPipeline() {
    const RealRHI::ShaderDesc shaderDesc{
        .moduleName = "shader",
        .entryPoints = {
            { .entryPoint = "main", .stage = RealRHI::ShaderStage::Vertex },
            { .entryPoint = "main", .stage = RealRHI::ShaderStage::Fragment },
        }
    };
    
    RealRHI::Ref<RealRHI::Shader> shader;
    RealRHI::Result result = device->CreateShader(shaderDesc, shader);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create shader" << std::endl;
    }

    RealRHI::PipelineDesc desc{
        .shader = shader.Raw(),
        .vertexLayout = {
            RealRHI::DataType::Float2, // pos
            RealRHI::DataType::Float3, // color
        },
        .rasterState = {
            .cullMode = RealRHI::CullMode::Back,
            .fillMode = RealRHI::FillMode::Solid,
            .frontCounterClockwise = false,
        },
        .depthState = {
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .compareOp = RealRHI::CompareOp::Less,
        },
        .blendState = {
            .enable = false,
        },
        .renderTargetFormats = {
            .colorFormats = { swapchain->GetBackBufferFormat() },
            .depthFormat = RealRHI::TextureFormat::Unknown,
        },
    };

    result = device->CreateGraphicsPipeline(desc, pipeline);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create pipeline" << std::endl;
    }
}

void CreateVertexBuffer() {
    RealRHI::Result result = device->CreateBuffer({
        .size = sizeof(vertices[0]) * vertices.size(),
        .usage = RealRHI::BufferUsage::Vertex,
        .memoryUsage = RealRHI::MemoryUsage::CPUToGPU,
        .initialData = vertices.data(),
    }, vertexBuffer);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create vertex buffer" << std::endl;
    }
}

void CreateCommandLists() {
    commandLists.resize(swapchain->GetMaxFramesInFlight());
    for (auto& cmd : commandLists) {
        RealRHI::Result result = device->CreateCommandList(cmd);
        if (result != RealRHI::Result::Success) {
            std::cerr << "Failed to create command list" << std::endl;
        }
    }
}

void RecordCommandBuffer(RealRHI::CommandList* cmd, const RealRHI::FrameContext& frame) {
    if (cmd->Begin() != RealRHI::Result::Success) {
		std::cerr << "Failed to begin command list" << std::endl;
        return;
	}

    RealRHI::RenderingInfo renderingInfo{
        .colorAttachments = {
            {
                .target = frame.backBufferView,
                .loadOp = RealRHI::LoadOp::Clear,
                .storeOp = RealRHI::StoreOp::Store,
                .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}
            }
        },
        .renderArea = {
            .x = 0,
            .y = 0,
            .width = frame.width,
            .height = frame.height
        }
    };

    cmd->BeginRendering(renderingInfo);
    cmd->BindPipeline(pipeline.Raw());

    RealRHI::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(frame.width),
        .height = static_cast<float>(frame.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    cmd->SetViewport(viewport);

    RealRHI::Rect scissor{
        .x = 0,
        .y = 0,
        .width = frame.width,
        .height = frame.height,
    };
    cmd->SetScissor(scissor);

    cmd->BindVertexBuffer(vertexBuffer.Raw());
    cmd->Draw(static_cast<uint32_t>(vertices.size()));
    cmd->EndRendering();

    if (cmd->End() != RealRHI::Result::Success) {
        std::cerr << "Failed to end command list" << std::endl;
    }
}

void DrawFrame() {
    RealRHI::FrameContext frame = swapchain->BeginFrame();

    auto* cmd = commandLists[frame.frameIndex].Raw();
    RecordCommandBuffer(cmd, frame);

    device->Submit(cmd, swapchain.Raw(), frame);
    swapchain->Present(frame);
}

void Cleanup() {
    device->WaitIdle();

    pipeline.Reset();
    vertexBuffer.Reset();
    commandLists.clear();
    swapchain.Reset();
    device.reset();
}

int main() {
    std::cout << "RealRHI Triangle Example - Simple Triangle Rendering" << std::endl;
    std::cout << "====================================================" << std::endl;

    RealRHI::DeviceDesc createInfo {
        .applicationName = "RealRHI Triangle Example",
        .shaderDirectory = "shaders",
        .enableDebug = true,
        .enableValidationLayers = true,
    };

    RealRHI::Result result = RealRHI::Device::Create(createInfo, device);
    if (result != RealRHI::Result::Success) {
		std::cerr << " Failed to create device" << std::endl;
        return -1;
	}
    std::cout << " Device created successfully" << std::endl;

    CreateSwapChain();
    std::cout << " Swapchain created" << std::endl;

    CreateGraphicsPipeline();
    std::cout << " Graphics pipeline created" << std::endl;

    CreateVertexBuffer();
    std::cout << " Vertex buffer created" << std::endl;

    CreateCommandLists();
    std::cout << " Command lists created" << std::endl;

    std::cout << std::endl << "Rendering triangle..." << std::endl;

    bool running = true;
    SDL_Event event;
    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }
        DrawFrame();
    }

    Cleanup();

    std::cout << std::endl << "Program completed successfully!" << std::endl;
    return 0;
}

