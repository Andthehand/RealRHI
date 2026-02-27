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
std::unique_ptr<RealRHI::Window> window;
std::unique_ptr<RealRHI::Swapchain> swapchain;
std::unique_ptr<RealRHI::Pipeline> pipeline;
std::unique_ptr<RealRHI::Buffer> vertexBuffer;
std::vector<std::unique_ptr<RealRHI::CommandList>> commandLists;

void CreateSwapChain() {
    window = device->CreateWindow({
        .Title = "RealRHI Triangle Example",
        .Width = WIDTH,
        .Height = HEIGHT
    });
    swapchain = device->CreateSwapchain({
        .window = window.get(),
    });
}

void CreateGraphicsPipeline() {
    const RealRHI::ShaderDesc shaderDesc{
        .moduleName = "shader",
        .entryPoints = {
            { .entryPoint = "main", .stage = RealRHI::ShaderStage::Vertex },
            { .entryPoint = "main", .stage = RealRHI::ShaderStage::Fragment },
        }
    };
    auto shader = device->CreateShader(shaderDesc);

    RealRHI::PipelineDesc desc{
        .shader = shader.get(),
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

    pipeline = device->CreateGraphicsPipeline(desc);
}

void CreateVertexBuffer() {
    vertexBuffer = device->CreateBuffer({
        .size = sizeof(vertices[0]) * vertices.size(),
        .usage = RealRHI::BufferUsage::Vertex,
        .memoryUsage = RealRHI::MemoryUsage::CPUToGPU,
        .initialData = vertices.data(),
    });
}

void CreateCommandLists() {
    commandLists.resize(swapchain->GetMaxFramesInFlight());
    for (auto& cmd : commandLists) {
        cmd = device->CreateCommandList();
    }
}

void RecordCommandBuffer(RealRHI::CommandList* cmd, const RealRHI::FrameContext& frame) {
    cmd->Begin();

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
    cmd->BindPipeline(pipeline.get());

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

    cmd->BindVertexBuffer(vertexBuffer.get());
    cmd->Draw(static_cast<uint32_t>(vertices.size()));
    cmd->EndRenderPass();

    cmd->End();
}

void DrawFrame() {
    RealRHI::FrameContext frame = swapchain->BeginFrame();

    auto* cmd = commandLists[frame.frameIndex].get();
    RecordCommandBuffer(cmd, frame);

    device->Submit(cmd, swapchain.get(), frame);
    swapchain->Present(frame);
}

void Cleanup() {
    device->WaitIdle();

    pipeline.reset();
    vertexBuffer.reset();
    commandLists.clear();
    swapchain.reset();
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

    device = RealRHI::Device::Create(createInfo);
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

